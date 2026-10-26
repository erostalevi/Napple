//! napple-compat: compatibility matrix (game x pipeline stage). See docs/architecture/compat-format.md.
pub mod model;
pub mod render;
pub mod runner;
pub mod store;

use model::*;
use std::path::PathBuf;

const USAGE: &str = "usage:
  napple compat run <folder> -k <prod.keys> [-o work/compat] [--matrix compat/matrix.json] [--md COMPATIBILITY.md]
  napple compat show [titleid]
  napple compat set <titleid> <stage> <status> \"<reason>\" [--build <id16>] [--evidence <path>]
  napple compat render [--matrix compat/matrix.json] [--md COMPATIBILITY.md] [-o work/compat]
  napple compat npdm <game.nsp|xci> -k <prod.keys>
stages: ingest build-plan nso-load site-scan rewrite rewrite-verify runtime-boot video audio input saves title-screen ingame
status: untested pass partial fail blocked";

#[derive(Default)]
struct Opts {
    pos: Vec<String>,
    keys: Option<PathBuf>,
    out: Option<PathBuf>,
    matrix: Option<PathBuf>,
    md: Option<PathBuf>,
    build: Option<String>,
    evidence: Option<String>,
}

fn parse(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts::default();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = |n: &str| {
            it.next()
                .cloned()
                .ok_or_else(|| format!("{n} needs a value"))
        };
        match a.as_str() {
            "-k" => o.keys = Some(val("-k")?.into()),
            "-o" => o.out = Some(val("-o")?.into()),
            "--matrix" => o.matrix = Some(val("--matrix")?.into()),
            "--md" => o.md = Some(val("--md")?.into()),
            "--build" => o.build = Some(val("--build")?),
            "--evidence" => o.evidence = Some(val("--evidence")?),
            _ => o.pos.push(a.clone()),
        }
    }
    Ok(o)
}

impl Opts {
    fn matrix(&self) -> PathBuf {
        self.matrix
            .clone()
            .unwrap_or_else(|| "compat/matrix.json".into())
    }
    fn md(&self) -> PathBuf {
        self.md.clone().unwrap_or_else(|| "COMPATIBILITY.md".into())
    }
    fn out(&self) -> PathBuf {
        self.out.clone().unwrap_or_else(|| "work/compat".into())
    }
}

pub fn run(args: &[String]) -> Result<(), String> {
    let Some(cmd) = args.first() else {
        return Err(USAGE.into());
    };
    let o = parse(&args[1..])?;
    match cmd.as_str() {
        "run" => {
            let folder = o.pos.first().ok_or(USAGE)?;
            let keys = o.keys.clone().ok_or("-k <prod.keys> is required")?;
            let opts = runner::RunOpts {
                folder: folder.into(),
                keys,
                stage_dir: o.out(),
                matrix: o.matrix(),
                md: o.md(),
            };
            let m = runner::run(&opts, &mut |l| println!("{l}"))?;
            println!(
                "matrix: {}  markdown: {}  games: {}",
                opts.matrix.display(),
                opts.md.display(),
                m.games.len()
            );
            Ok(())
        }
        "render" => {
            let mut m = store::load_matrix(&o.matrix())?;
            for w in store::overlay_stage_files(&mut m, &o.out()) {
                eprintln!("warning: {w}");
            }
            m.sort();
            store::save_matrix(&o.matrix(), &m)?;
            store::atomic_write(&o.md(), &render::render(&m))?;
            println!("rendered {}", o.md().display());
            Ok(())
        }
        "show" => {
            let m = store::load_matrix(&o.matrix())?;
            let filter = o.pos.first().map(|s| s.to_ascii_uppercase());
            for g in m
                .games
                .iter()
                .filter(|g| filter.as_ref().is_none_or(|f| &g.title_id == f))
            {
                println!("{} {} ({})", g.title_id, g.name, g.publisher);
                for v in &g.versions {
                    println!(
                        "  {} v{} build {} {}",
                        v.kind, v.version, v.build_id, v.display_version
                    );
                    for s in STAGES {
                        let r = v.stage(s);
                        println!("    {:<15} {:<9} {}", s, r.status.name(), r.reason);
                    }
                }
            }
            Ok(())
        }
        "set" => {
            let [tid, stage, status, reason] = o.pos.as_slice() else {
                return Err(USAGE.into());
            };
            let tid = tid.to_ascii_uppercase();
            if !store::is_title_id(&tid) {
                return Err("title id must be 16 hex digits".into());
            }
            if !is_stage(stage) {
                return Err(format!("unknown stage '{stage}'"));
            }
            let status =
                Status::parse(status).ok_or_else(|| format!("unknown status '{status}'"))?;
            let mut m = store::load_matrix(&o.matrix())?;
            let g = m
                .games
                .iter_mut()
                .find(|g| g.title_id == tid)
                .ok_or("game not in matrix (run `compat run` first)")?;
            let r = StageResult {
                status,
                reason: reason.clone(),
                evidence: o.evidence.clone().unwrap_or_default(),
                time: store::now(),
                tool: "manual".into(),
                counters: Counters::default(),
            };
            let mut n = 0;
            for v in g
                .versions
                .iter_mut()
                .filter(|v| o.build.as_ref().is_none_or(|b| &v.build_id == b))
            {
                store::write_stage(&o.out(), &tid, &v.build_id, stage, &r)?;
                v.stages.insert(stage.clone(), r.clone());
                n += 1;
            }
            if n == 0 {
                return Err("no matching version".into());
            }
            store::save_matrix(&o.matrix(), &m)?;
            store::atomic_write(&o.md(), &render::render(&m))?;
            println!("updated {n} version(s)");
            Ok(())
        }
        "npdm" => {
            use napple_core::application::package::scan;
            let f = o.pos.first().ok_or(USAGE)?;
            let keys =
                napple_core::domain::keys::KeySet::load(o.keys.as_ref().ok_or("-k required")?)
                    .map_err(|e| e.to_string())?;
            for p in scan(std::path::Path::new(f), &keys).map_err(|e| e.to_string())? {
                if !matches!(
                    p.meta.kind,
                    napple_core::infrastructure::cnmt::TitleKind::Application
                        | napple_core::infrastructure::cnmt::TitleKind::Patch
                ) {
                    continue;
                }
                let prog = p.program(&keys).map_err(|e| e.to_string())?;
                let a = runner::analyze(&prog);
                let Some(n) = a.npdm else { continue };
                let i = runner::npdm_info(&n);
                println!(
                    "{:016X} v{} process {} 64-bit={} stack={:#x} handles={:?} kernel={:?}",
                    p.meta.title_id,
                    p.meta.version,
                    i.name,
                    i.is_64bit,
                    i.main_thread_stack_size,
                    i.handle_table_size,
                    i.kernel_version
                );
                println!("services used: {}", i.services_used.join(" "));
                println!("services hosted: {}", i.services_hosted.join(" "));
            }
            Ok(())
        }
        _ => Err(USAGE.into()),
    }
}
