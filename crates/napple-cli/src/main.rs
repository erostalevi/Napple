use napple_core::application::decomp;
use napple_core::application::ingest::ingest;
use napple_core::application::package::{scan, version_label, BuildPlan, Choice, DlcChoice};
use napple_core::domain::error::{Error, Result};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::cnmt::TitleKind;
use std::path::PathBuf;

const USAGE: &str = "usage:
  napple ingest <game> -k <prod.keys>
  napple plan   <files...> -k <prod.keys> [--updates all|latest|none|<v,v>] [--dlc all|none|<id,id>]
  napple decomp <files...> -k <prod.keys> [--updates ...] [--dlc ...] [-o <dir>]
files: base game, updates and DLC (XCI/NSP), any order, one or several files.";

struct Args {
    cmd: String,
    files: Vec<PathBuf>,
    keys: Option<PathBuf>,
    updates: Choice,
    dlc: DlcChoice,
    out: Option<PathBuf>,
}

fn parse() -> Option<Args> {
    let mut it = std::env::args().skip(1);
    let mut a = Args { cmd: it.next()?, files: vec![], keys: None, updates: Choice::Latest, dlc: DlcChoice::All, out: None };
    while let Some(x) = it.next() {
        match x.as_str() {
            "-k" => a.keys = Some(it.next()?.into()),
            "-o" => a.out = Some(it.next()?.into()),
            "--updates" => {
                a.updates = match it.next()?.as_str() {
                    "all" => Choice::All,
                    "latest" => Choice::Latest,
                    "none" => Choice::None,
                    v => Choice::Only(v.split(',').map(|s| s.trim().parse().ok()).collect::<Option<_>>()?),
                }
            }
            "--dlc" => {
                a.dlc = match it.next()?.as_str() {
                    "all" => DlcChoice::All,
                    "none" => DlcChoice::None,
                    v => DlcChoice::Only(v.split(',').map(|s| u64::from_str_radix(s.trim(), 16).ok()).collect::<Option<_>>()?),
                }
            }
            f => a.files.push(f.into()),
        }
    }
    (a.keys.is_some() && !a.files.is_empty()).then_some(a)
}

fn gb(n: u64) -> String {
    format!("{:.2} GB", n as f64 / 1e9)
}

fn build_plan(a: &Args, keys: &KeySet) -> Result<BuildPlan> {
    let mut packs = Vec::new();
    for f in &a.files {
        packs.extend(scan(f, keys)?);
    }
    Ok(BuildPlan::from_packs(packs))
}

fn print_plan(plan: &BuildPlan, upd: &[usize], dlc: &[usize]) {
    let mark = |sel: bool| if sel { "[x]" } else { "[ ]" };
    if let Some(b) = plan.base {
        let p = &plan.packs[b];
        println!("Base    [x] {:016X}  {}  {}", p.meta.title_id, version_label(p.meta.version), gb(p.size()));
    }
    for &i in &plan.updates {
        let p = &plan.packs[i];
        println!("Update  {} {:016X}  {}  {}", mark(upd.contains(&i)), p.meta.title_id, version_label(p.meta.version), gb(p.size()));
    }
    for &i in &plan.dlcs {
        let p = &plan.packs[i];
        println!("DLC     {} {:016X}  {}  {}", mark(dlc.contains(&i)), p.meta.title_id, version_label(p.meta.version), gb(p.size()));
    }
    for w in &plan.warnings {
        println!("warning: {w}");
    }
    let _ = TitleKind::Application;
}

fn run(a: Args) -> Result<()> {
    let keys = KeySet::load(a.keys.as_ref().unwrap())?;
    match a.cmd.as_str() {
        "ingest" => {
            let r = ingest(&a.files[0], &keys)?;
            println!("title id: {:016X}", r.title_id.unwrap_or(0));
            for (n, d) in &r.exefs {
                println!("exefs {n} ({} bytes)", d.len());
            }
            if let Some(rf) = &r.romfs {
                println!("romfs: {} files", rf.files.len());
            }
        }
        "plan" => {
            let plan = build_plan(&a, &keys)?;
            print_plan(&plan, &plan.pick_updates(&a.updates), &plan.pick_dlcs(&a.dlc));
        }
        "decomp" => {
            let plan = build_plan(&a, &keys)?;
            let (upd, dlc) = (plan.pick_updates(&a.updates), plan.pick_dlcs(&a.dlc));
            print_plan(&plan, &upd, &dlc);
            if plan.base.is_none() {
                return Err(Error::Container("a base game is required".into()));
            }
            let reports = decomp::run(&plan, &upd, &keys)?;
            for r in &reports {
                println!("\n== {:016X} {}", r.title_id, r.label);
                for m in &r.modules {
                    let s: Vec<String> = m.sites.iter().map(|(k, n)| format!("{k:?}={n}")).collect();
                    println!("{:<8} build {}  text {:>9} B  {}", m.name, &m.build_id[..16], m.text_bytes, s.join(" "));
                }
                match (r.romfs_files, &r.romfs_note) {
                    (Some(n), _) => println!("romfs: {n} files"),
                    (None, Some(note)) => println!("romfs: pending ({note})"),
                    _ => {}
                }
            }
            if let Some(dir) = &a.out {
                std::fs::create_dir_all(dir)?;
                let body: Vec<String> = reports.iter().map(|r| r.to_json()).collect();
                std::fs::write(dir.join("decomp-report.json"), format!("[{}]\n", body.join(",")))?;
                println!("\nreport written to {}", dir.join("decomp-report.json").display());
            }
        }
        _ => {
            eprintln!("{USAGE}");
            std::process::exit(2);
        }
    }
    Ok(())
}

fn main() {
    let Some(a) = parse() else {
        eprintln!("{USAGE}");
        std::process::exit(2)
    };
    if let Err(e) = run(a) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}
