//! `napple sigs ...` interface.
use crate::domain::*;
use crate::repo::*;
use crate::resolver::*;
use crate::reverser::{ReaCliReverser, ReaConfig, Reverser};
use crate::share;
use crate::validate;
use napple_core::application::ingest::ingest;
use napple_core::domain::keys::KeySet;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

const USAGE: &str = "usage:
  napple sigs lookup <titleId> <buildId>
  napple sigs match   <module.nso|patched.bin> [--title ID --build ID --name main]
  napple sigs match   --game <xci|nsp> -k <prod.keys> [--module main]
  napple sigs resolve <module> --title ID [--build ID] [--no-rea] OFFSET[:KIND[:NAME[:SIZE]]]...
  napple sigs resolve --game <file> -k <prod.keys> --module main [--no-rea] OFFSET[:KIND[:NAME[:SIZE]]]...
  napple sigs pending
  napple sigs share [--dry-run | --yes]
  napple sigs validate <file.json>
  napple sigs sync                      (explicit network: refresh work/sigcache from the public repo)
common: --work DIR (default ./work)  --known DIR (default ./knownSignatures)  --remote URL
Default is local only: nothing is sent anywhere without `share --yes`; `sync` is the only other network use.";

#[derive(Default)]
struct Opts {
    pos: Vec<String>,
    val: HashMap<String, String>,
    flag: Vec<String>,
}

fn parse(args: &[String]) -> Result<Opts, String> {
    const VALUE: [&str; 9] = ["--work", "--known", "--remote", "--title", "--build", "--name", "--game", "-k", "--module"];
    const FLAG: [&str; 3] = ["--no-rea", "--dry-run", "--yes"];
    let mut o = Opts::default();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        if VALUE.contains(&a.as_str()) {
            o.val.insert(a.clone(), it.next().ok_or(format!("{a} needs a value"))?.clone());
        } else if FLAG.contains(&a.as_str()) {
            o.flag.push(a.clone());
        } else if a.starts_with('-') && a.len() > 1 {
            return Err(format!("unknown option {a}\n{USAGE}"));
        } else {
            o.pos.push(a.clone());
        }
    }
    Ok(o)
}

impl Opts {
    fn v(&self, k: &str) -> Option<&String> {
        self.val.get(k)
    }
    fn has(&self, f: &str) -> bool {
        self.flag.iter().any(|x| x == f)
    }
}

struct Env {
    work: PathBuf,
    pending: DirRepository,
    view: LayeredRepository,
    remote: String,
}

fn env(o: &Opts) -> Env {
    let work = PathBuf::from(o.v("--work").map(|s| s.as_str()).unwrap_or("work"));
    let known = PathBuf::from(o.v("--known").map(|s| s.as_str()).unwrap_or("knownSignatures"));
    let remote = o.v("--remote").cloned().unwrap_or_else(|| DEFAULT_REMOTE.to_string());
    let sigs = work.join("sigs");
    let view = LayeredRepository::new(
        vec![
            Box::new(DirRepository::new(sigs.join("pending"), "pending", true)),
            Box::new(DirRepository::new(sigs.join("shared"), "shared", true)),
            Box::new(DirRepository::new(known, "known", false)),
            Box::new(GitCacheRepository::new(&remote, work.join("sigcache"))),
        ],
        0,
    );
    Env { pending: DirRepository::new(sigs.join("pending"), "pending", true), work, view, remote }
}

fn load_modules(o: &Opts, path_arg: Option<&String>) -> Result<(Vec<Module>, Option<TitleId>), String> {
    if let Some(game) = o.v("--game") {
        let keys_path = o.v("-k").ok_or("--game needs -k <prod.keys>")?;
        let keys = KeySet::load(Path::new(keys_path)).map_err(|e| e.to_string())?;
        let r = ingest(Path::new(game), &keys).map_err(|e| e.to_string())?;
        let title = r.title_id.map(|t| TitleId(format!("{t:016X}")));
        let mut mods = Vec::new();
        for (name, data) in &r.exefs {
            if name == "main.npdm" || data.len() < 4 || &data[..4] != b"NSO0" {
                continue;
            }
            if o.v("--module").map(|m| m != name).unwrap_or(false) {
                continue;
            }
            mods.push(Module::from_nso(name, data)?);
        }
        if mods.is_empty() {
            return Err("no matching NSO module in the game".into());
        }
        return Ok((mods, title));
    }
    let p = path_arg.ok_or("missing module path")?;
    let bytes = std::fs::read(p).map_err(|e| format!("{p}: {e}"))?;
    let name = o.v("--name").cloned().unwrap_or_else(|| module_name_from_path(Path::new(p)));
    let title = o.v("--title").map(|t| TitleId::parse(t)).transpose()?;
    let m = if bytes.len() >= 4 && &bytes[..4] == b"NSO0" {
        Module::from_nso(&name, &bytes)?
    } else {
        let build = o.v("--build").map(|b| BuildId::parse(b)).transpose()?;
        Module::from_raw(&name, bytes, build)
    };
    Ok((vec![m], title))
}

fn parse_item(s: &str) -> Result<Item, String> {
    let mut p = s.splitn(4, ':');
    let off = p.next().unwrap();
    let offset = match off.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16),
        None => off.parse(),
    }
    .map_err(|_| format!("bad offset {off:?}"))?;
    let kind = match p.next().filter(|k| !k.is_empty()) {
        Some(k) => SigKind::parse(k).ok_or(format!("unknown kind {k:?}"))?,
        None => SigKind::Function,
    };
    let name = p.next().filter(|n| !n.is_empty()).map(|n| n.to_string());
    let size = p
        .next()
        .map(|z| match z.strip_prefix("0x") {
            Some(h) => u64::from_str_radix(h, 16),
            None => z.parse(),
        })
        .transpose()
        .map_err(|_| "bad size".to_string())?;
    Ok(Item { offset, kind, name, size })
}

fn print_sig(hash: &str, s: &Signature) {
    println!(
        "{hash}  {:<12} {:<8} {:#x}..{:#x}  {:?}/{:?}  {}",
        format!("{:?}", s.kind),
        s.module,
        s.offset_start,
        s.offset_end,
        s.provenance,
        s.confidence,
        s.name.as_deref().unwrap_or("-")
    );
}

pub fn run(args: &[String]) -> Result<(), String> {
    let Some(cmd) = args.first() else {
        return Err(USAGE.into());
    };
    let o = parse(&args[1..])?;
    let e = env(&o);
    match cmd.as_str() {
        "lookup" => {
            let (t, b) = match (o.pos.first(), o.pos.get(1)) {
                (Some(t), Some(b)) => (TitleId::parse(t)?, BuildId::parse(b)?),
                _ => return Err(USAGE.into()),
            };
            match e.view.load(&t, &b)? {
                None => {
                    println!("no known signatures for {t}/{b} (layers: {})", e.view.label());
                    let others = e.view.builds(&t)?;
                    if !others.is_empty() {
                        println!("other builds of this title: {}", others.iter().map(|b| b.0.as_str()).collect::<Vec<_>>().join(", "));
                    }
                }
                Some(f) => {
                    println!("{t}/{b}: {} signature(s)", f.signatures.len());
                    for (h, s) in &f.signatures {
                        print_sig(h, s);
                    }
                }
            }
            Ok(())
        }
        "match" => {
            let (mods, title) = load_modules(&o, o.pos.first())?;
            let title = title.ok_or("need --title (raw files and NSOs do not carry the title id)")?;
            for m in &mods {
                let build = m.build_id.clone().ok_or("raw modules need --build")?;
                println!("== {title}/{build} module {}", m.name);
                let Some(f) = e.view.load(&title, &build)? else {
                    println!("no known signatures for this build");
                    continue;
                };
                let r = match_module(&f, m)?;
                for h in &r.hits {
                    let offs: Vec<String> = h.offsets.iter().map(|x| format!("{x:#x}")).collect();
                    let note = if h.offsets.len() > 1 { " AMBIGUOUS" } else if h.in_range { "" } else { " (outside recorded range)" };
                    println!("hit      {}  {:<12} {}  {}{note}", h.hash, format!("{:?}", h.kind), offs.join(","), h.name.as_deref().unwrap_or("-"));
                }
                for u in &r.unknown {
                    println!("unknown  {u}  (signature not found in this module)");
                }
                println!("{} hit(s), {} unknown, {} for other modules", r.hits.len(), r.unknown.len(), r.skipped_other_module);
            }
            Ok(())
        }
        "resolve" => {
            let (path_arg, items_start) = if o.v("--game").is_some() { (None, 0) } else { (o.pos.first(), 1) };
            let (mods, title) = load_modules(&o, path_arg)?;
            if mods.len() != 1 {
                return Err("resolve needs exactly one module (use --module with --game)".into());
            }
            let m = &mods[0];
            let title = title.ok_or("need --title")?;
            let build = m.build_id.clone().ok_or("raw modules need --build")?;
            let items: Vec<Item> = o.pos.iter().skip(items_start).map(|s| parse_item(s)).collect::<Result<_, _>>()?;
            if items.is_empty() {
                return Err("no items given (OFFSET[:KIND[:NAME[:SIZE]]])".into());
            }
            let rea = ReaCliReverser::new(ReaConfig::detect(&e.work));
            let reverser: Option<&dyn Reverser> = if o.has("--no-rea") || !rea.available() { None } else { Some(&rea) };
            if reverser.is_none() && !o.has("--no-rea") {
                println!("note: rea CLI not found under {}; resolving without a Reverser", e.work.display());
            }
            let scratch = e.work.join("rea-in");
            let ctx = Context { title: &title, build: &build, known: &e.view, pending: &e.pending, reverser, scratch: &scratch };
            let mut failed = 0;
            for it in &items {
                match resolve_item(&ctx, m, it) {
                    Outcome::Known { hash } => println!("{:#x}  known         {hash}", it.offset),
                    Outcome::Carried { hash, from_build } => println!("{:#x}  carried       {hash}  (from build {from_build}; stored as pending-share)", it.offset),
                    Outcome::Learned { hash, via } => println!("{:#x}  learned       {hash}  (via {via}; stored as pending-share)", it.offset),
                    Outcome::Manual { hash, why } => println!("{:#x}  local         {hash}  (low confidence; {why}; stored as pending-share)", it.offset),
                    Outcome::Failed { reason } => {
                        failed += 1;
                        println!("{:#x}  UNRESOLVED    {reason}", it.offset);
                    }
                }
            }
            println!("nothing was sent anywhere. Review with `napple sigs pending`; share with `napple sigs share`.");
            if failed > 0 { Err(format!("{failed} item(s) unresolved")) } else { Ok(()) }
        }
        "pending" => {
            let mut n = 0;
            for t in e.pending.titles()? {
                for b in e.pending.builds(&t)? {
                    match e.pending.load(&t, &b) {
                        Ok(Some(f)) => {
                            println!("{t}/{b}: {} signature(s)", f.signatures.len());
                            for (h, s) in &f.signatures {
                                print_sig(h, s);
                                n += 1;
                            }
                        }
                        Ok(None) => {}
                        Err(err) => println!("{t}/{b}: UNREADABLE ({err})"),
                    }
                }
            }
            println!("{n} pending signature(s) (local only; `napple sigs share` to review and send)");
            Ok(())
        }
        "share" => {
            let dry = o.has("--dry-run");
            let plan = share::build_plan(&e.pending, &e.remote)?;
            print!("{}", share::preview(&plan, dry));
            if plan.files.is_empty() {
                println!("\nnothing to share.");
                return Ok(());
            }
            if dry {
                let t = share::Tools::detect();
                println!("\ndry run complete; gh/git available: {}", if t.is_ok() { "yes" } else { "no (install gh to share)" });
                return Ok(());
            }
            if !o.has("--yes") {
                println!("\nNothing was sent. Review the above, then re-run with --yes to open the pull request.");
                return Ok(());
            }
            let tools = share::Tools::detect()?;
            let url = share::execute(&plan, &tools, &e.work)?;
            // Move shared entries out of pending so they are not offered again.
            let shared = DirRepository::new(e.work.join("sigs/shared"), "shared", true);
            for (rel, _) in &plan.files {
                let parts: Vec<&str> = rel.split('/').collect();
                let t = TitleId::parse(parts[1])?;
                let b = BuildId::parse(parts[2].trim_end_matches(".json"))?;
                if let Some(f) = e.pending.load(&t, &b)? {
                    shared.save(&f)?;
                    e.pending.remove(&t, &b)?;
                }
            }
            println!("\npull request: {url}");
            Ok(())
        }
        "validate" => {
            let p = o.pos.first().ok_or(USAGE)?;
            let text = std::fs::read_to_string(p).map_err(|e| format!("{p}: {e}"))?;
            match validate::validate_json(&text) {
                Ok(f) => {
                    println!("OK: {}/{} with {} signature(s)", f.title_id, f.build_id, f.signatures.len());
                    Ok(())
                }
                Err(errs) => {
                    for e in &errs {
                        println!("REJECTED: {e}");
                    }
                    Err(format!("{} problem(s)", errs.len()))
                }
            }
        }
        "sync" => {
            let c = GitCacheRepository::new(&e.remote, e.work.join("sigcache"));
            println!("fetching {} into {} (read-only cache)...", c.url, c.cache.display());
            c.sync()?;
            println!("done");
            Ok(())
        }
        _ => Err(USAGE.into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn item_syntax() {
        let i = parse_item("0x1f0:function:my_fn:0x40").unwrap();
        assert_eq!((i.offset, i.kind, i.name.as_deref(), i.size), (0x1f0, SigKind::Function, Some("my_fn"), Some(0x40)));
        assert_eq!(parse_item("32").unwrap().offset, 32);
        assert!(parse_item("zz").is_err());
        assert!(parse_item("0x10:bogus").is_err());
    }

    #[test]
    fn unknown_options_and_commands_fail() {
        assert!(parse(&["--wat".to_string()]).is_err());
        assert!(run(&["nope".to_string()]).is_err());
        assert!(run(&[]).is_err());
    }

    #[test]
    fn validate_command_accepts_and_rejects() {
        let d = tempfile::tempdir().unwrap();
        let good = d.path().join("g.json");
        std::fs::write(&good, crate::validate::fixtures::sample_file().to_json()).unwrap();
        assert!(run(&["validate".into(), good.to_string_lossy().into()]).is_ok());
        let bad = d.path().join("b.json");
        std::fs::write(&bad, "{\"x\":1}").unwrap();
        assert!(run(&["validate".into(), bad.to_string_lossy().into()]).is_err());
    }

    #[test]
    fn share_dry_run_end_to_end_is_local_only() {
        let d = tempfile::tempdir().unwrap();
        let work = d.path().join("work");
        let pend = DirRepository::new(work.join("sigs/pending"), "p", true);
        pend.save(&crate::validate::fixtures::sample_file()).unwrap();
        let a = |s: &[&str]| s.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        let w = work.to_string_lossy().to_string();
        assert!(run(&a(&["share", "--dry-run", "--work", &w])).is_ok());
        assert!(run(&a(&["share", "--work", &w])).is_ok()); // no --yes: preview only
        assert!(run(&a(&["pending", "--work", &w])).is_ok());
        // still pending (nothing moved)
        assert_eq!(pend.titles().unwrap().len(), 1);
    }
}
