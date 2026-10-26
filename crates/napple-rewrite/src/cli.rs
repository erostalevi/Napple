//! `napple rewrite <game files...> -k <prod.keys> -o <outdir> [--updates all|latest|none|v,v]`
//! A raw `.nso` file may be given instead of a game container (module name = file stem).
use crate::check;
use crate::error::RewriteError;
use crate::image::ModuleImage;
use crate::rewrite::rewrite_module;
use crate::types::RewriteOptions;
use napple_core::application::package::{scan, BuildPlan, Choice};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::nso::Nso;
use rayon::prelude::*;
use std::path::{Path, PathBuf};

const USAGE: &str = "usage: napple rewrite <base/update files or .nso...> -k <prod.keys> -o <outdir> [--updates all|latest|none|<v,v>]
       napple rewrite --survey|--survey-exact <base/update files...> -k <prod.keys> [--updates ...]   (no output; NPDM, modules, sites, relocations, patch size)";

fn is_module(name: &str) -> bool {
    matches!(name, "main" | "rtld" | "sdk") || name.starts_with("subsdk")
}

struct Job {
    dir: PathBuf,
    modules: Vec<(String, Vec<u8>)>,
}

/// Rewrites + verifies every module; returns `Err` when anything is unhandled or fails the check.
fn process(job: &Job) -> Result<Vec<String>, RewriteError> {
    let results: Vec<Result<(String, bool), RewriteError>> = job
        .modules
        .par_iter()
        .map(|(name, bytes)| {
            let nso = Nso::parse(bytes).map_err(|e| RewriteError::Layout(format!("{name}: {e}")))?;
            if crate::survey::is_aarch32(&nso) {
                return Ok((format!("{name:<8} SKIPPED: AArch32 module, no AArch64 rewrite applies (napple build routes it to the plain image + NTB2 tables)"), true));
            }
            let t = std::time::Instant::now();
            let out = rewrite_module(&nso, &nso.build_id_hex()[..32], &RewriteOptions::default())?;
            let orig = ModuleImage::from_nso(&nso)?;
            let chk = check::verify(&orig, &out);
            out.write_to(&job.dir, name)?;
            let mut line = format!("{name:<8} build {}  {:?}  gates {}  patch {} B  {:?}", &out.build_id[..16], out.stats, out.gates.len(), out.patch().len(), t.elapsed());
            let mut ok = true;
            for u in &out.unhandled {
                line += &format!("\n    UNHANDLED {u}");
                ok = false;
            }
            for v in chk.violations.iter().take(10) {
                line += &format!("\n    CHECK FAILED {v}");
                ok = false;
            }
            if !out.warnings.is_empty() {
                line += &format!("\n    {} warning(s), see {name}.warnings.txt", out.warnings.len());
            }
            Ok((line, ok))
        })
        .collect();
    let mut lines = Vec::new();
    let mut all_ok = true;
    for r in results {
        let (l, ok) = r?;
        all_ok &= ok;
        lines.push(l);
    }
    if all_ok { Ok(lines) } else { Err(RewriteError::Usage(lines.join("\n") + "\nrewrite incomplete: unhandled sites or failed structural check")) }
}

pub fn run(args: &[String]) -> Result<(), String> {
    if args.first().map(String::as_str) == Some("a32") {
        return crate::a32::run(&args[1..]).map_err(|e| e.to_string());
    }
    run_inner(args).map_err(|e| e.to_string())
}

fn run_inner(args: &[String]) -> Result<(), RewriteError> {
    let (mut files, mut keys, mut out, mut updates) = (Vec::<PathBuf>::new(), None, None, Choice::None);
    let mut survey: Option<bool> = None; // Some(exact)
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().ok_or_else(|| RewriteError::Usage(USAGE.into()));
        match a.as_str() {
            "-k" => keys = Some(PathBuf::from(val()?)),
            "-o" => out = Some(PathBuf::from(val()?)),
            "--updates" => {
                updates = match val()?.as_str() {
                    "all" => Choice::All,
                    "latest" => Choice::Latest,
                    "none" => Choice::None,
                    v => Choice::Only(v.split(',').map(|s| s.trim().parse().map_err(|_| RewriteError::Usage(USAGE.into()))).collect::<Result<_, _>>()?),
                }
            }
            "--survey" => survey = Some(false),
            "--survey-exact" => survey = Some(true),
            "-h" | "--help" => return Err(RewriteError::Usage(USAGE.into())),
            f => files.push(f.into()),
        }
    }
    if let Some(exact) = survey {
        return run_survey(&files, keys, &updates, exact);
    }
    let out = out.ok_or_else(|| RewriteError::Usage(USAGE.into()))?;
    if files.is_empty() {
        return Err(RewriteError::Usage(USAGE.into()));
    }
    let io = |e: std::io::Error| RewriteError::Io(e.to_string());
    let mut jobs = Vec::new();
    let raw: Vec<&PathBuf> = files.iter().filter(|f| std::fs::read(f).map(|b| b.starts_with(b"NSO0")).unwrap_or(false)).collect();
    if !raw.is_empty() {
        let modules = raw.iter().map(|f| Ok((f.file_stem().unwrap().to_string_lossy().to_string(), std::fs::read(f).map_err(io)?))).collect::<Result<Vec<_>, RewriteError>>()?;
        jobs.push(Job { dir: out.clone(), modules });
    } else {
        let keys = KeySet::load(&keys.ok_or_else(|| RewriteError::Usage(USAGE.into()))?).map_err(|e| RewriteError::Io(e.to_string()))?;
        let mut packs = Vec::new();
        for f in &files {
            packs.extend(scan(f, &keys).map_err(|e| RewriteError::Io(e.to_string()))?);
        }
        let plan = BuildPlan::from_packs(packs);
        let base = plan.base.ok_or_else(|| RewriteError::Usage("no base game among the supplied files".into()))?;
        let mut targets = vec![(base, out.clone())];
        for i in plan.pick_updates(&updates) {
            targets.push((i, out.join(format!("update-v{}", plan.packs[i].meta.version))));
        }
        for (i, dir) in targets {
            let prog = plan.packs[i].program(&keys).map_err(|e| RewriteError::Io(e.to_string()))?;
            let modules = prog.exefs.into_iter().filter(|(n, _)| is_module(n)).collect();
            jobs.push(Job { dir, modules });
        }
    }
    let mut failed = false;
    for job in &jobs {
        println!("== {}", Path::new(&job.dir).display());
        match process(job) {
            Ok(lines) => lines.iter().for_each(|l| println!("{l}")),
            Err(RewriteError::Usage(m)) => {
                println!("{m}");
                failed = true;
            }
            Err(e) => return Err(e),
        }
    }
    if failed { Err(RewriteError::Usage("rewrite failed".into())) } else { Ok(()) }
}

/// `--survey`: prints facts per program and fails (exit 1) with a clear message when a build cannot work.
fn run_survey(files: &[PathBuf], keys: Option<PathBuf>, updates: &Choice, exact: bool) -> Result<(), RewriteError> {
    use crate::survey::{survey_module, survey_program};
    if files.is_empty() {
        return Err(RewriteError::Usage(USAGE.into()));
    }
    let io = |e: std::io::Error| RewriteError::Io(e.to_string());
    let mut surveys = Vec::new();
    let raw: Vec<&PathBuf> = files.iter().filter(|f| std::fs::read(f).map(|b| b.starts_with(b"NSO0")).unwrap_or(false)).collect();
    if !raw.is_empty() {
        let mut s = crate::survey::Survey { label: "raw NSO files".into(), ..Default::default() };
        for f in raw {
            s.modules.push(survey_module(&f.file_stem().unwrap().to_string_lossy(), &std::fs::read(f).map_err(io)?, exact));
        }
        surveys.push(s);
    } else {
        let keys = KeySet::load(&keys.ok_or_else(|| RewriteError::Usage(USAGE.into()))?).map_err(|e| RewriteError::Io(e.to_string()))?;
        let mut packs = Vec::new();
        for f in files {
            packs.extend(scan(f, &keys).map_err(|e| RewriteError::Io(e.to_string()))?);
        }
        let plan = BuildPlan::from_packs(packs);
        let base = plan.base.ok_or_else(|| RewriteError::Usage("no base game among the supplied files: a build needs the base container".into()))?;
        let mut targets = vec![base];
        targets.extend(plan.pick_updates(updates));
        for i in targets {
            let p = &plan.packs[i];
            let prog = p.program(&keys).map_err(|e| RewriteError::Io(e.to_string()))?;
            surveys.push(survey_program(&format!("{:016X} {}", p.meta.title_id, napple_core::application::package::version_label(p.meta.version)), &prog.exefs, exact));
        }
    }
    let mut blocking = Vec::new();
    for s in &surveys {
        print!("{}", s.render());
        blocking.extend(s.blocking().into_iter().map(|b| format!("{}: {b}", s.label)));
    }
    if blocking.is_empty() {
        Ok(())
    } else {
        Err(RewriteError::Usage(format!("survey: this title cannot be built as is:\n  {}", blocking.join("\n  "))))
    }
}
