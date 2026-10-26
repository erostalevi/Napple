//! `napple clean`: lists and removes rebuildable artifacts. Never touches saves, keys, firmware or game files.
//!
//!   napple clean [--shader-cache] [--build-cache DIR] [--engine DIR] [--save-root DIR] [--dry-run]
//!
//! * shader cache: `<save-root>/<title id>/cache` (translated shaders and the Metal pipelines archive; the next boot rebuilds it).
//! * build cache: `<DIR>/.napple-cache.nosync` (resumable RomFS copies left by `napple build --keep-cache`).
//! * engine: copies of `napple-runtime`, `napple-runtime-ios` and `napple-launcher` inside the built game folder DIR (`napple build` puts them back).
//!
//! With no category flag the shader cache is selected (plus the other two when their DIR is given).

use std::fs;
use std::path::{Path, PathBuf};

const ENGINE_FILES: [&str; 3] = ["napple-runtime", "napple-runtime-ios", "napple-launcher"];
const BUILD_CACHE_DIR: &str = ".napple-cache.nosync";
const MAX_DEPTH: usize = 6;

#[derive(Debug, Default, Clone)]
pub struct Options {
    pub shader_cache: bool,
    pub build_cache: Option<PathBuf>,
    pub engine: Option<PathBuf>,
    pub save_root: Option<PathBuf>,
    pub dry_run: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Item {
    pub kind: &'static str,
    pub path: PathBuf,
    pub bytes: u64,
}

pub fn default_save_root() -> Option<PathBuf> {
    std::env::var_os("HOME").map(|h| Path::new(&h).join("Library/Application Support/Napple"))
}

fn size_of(p: &Path) -> u64 {
    match fs::symlink_metadata(p) {
        Ok(m) if m.is_dir() => fs::read_dir(p)
            .map(|d| d.flatten().map(|e| size_of(&e.path())).sum())
            .unwrap_or(0),
        Ok(m) => m.len(),
        Err(_) => 0,
    }
}

fn find_engine(dir: &Path, depth: usize, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_dir() {
            if depth < MAX_DEPTH {
                find_engine(&p, depth + 1, out);
            }
        } else if ft.is_file()
            && p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| ENGINE_FILES.contains(&n))
        {
            out.push(p);
        }
    }
}

/// What would be removed, in a stable order.
pub fn plan(o: &Options) -> Vec<Item> {
    let mut items = Vec::new();
    if o.shader_cache
        && let Some(root) = o.save_root.clone().or_else(default_save_root)
    {
        let mut titles: Vec<PathBuf> = fs::read_dir(&root)
            .map(|d| d.flatten().map(|e| e.path()).collect())
            .unwrap_or_default();
        titles.sort();
        for t in titles {
            let c = t.join("cache");
            if c.is_dir() {
                items.push(Item {
                    kind: "shader-cache",
                    bytes: size_of(&c),
                    path: c,
                });
            }
        }
    }
    if let Some(d) = &o.build_cache {
        let c = d.join(BUILD_CACHE_DIR);
        if c.is_dir() {
            items.push(Item {
                kind: "build-cache",
                bytes: size_of(&c),
                path: c,
            });
        }
    }
    if let Some(d) = &o.engine {
        let mut found = Vec::new();
        find_engine(d, 0, &mut found);
        found.sort();
        for p in found {
            items.push(Item {
                kind: "engine",
                bytes: size_of(&p),
                path: p,
            });
        }
    }
    items
}

pub fn execute(items: &[Item]) -> (u64, Vec<String>) {
    let mut freed = 0;
    let mut errors = Vec::new();
    for i in items {
        let r = if i.path.is_dir() {
            fs::remove_dir_all(&i.path)
        } else {
            fs::remove_file(&i.path)
        };
        match r {
            Ok(()) => freed += i.bytes,
            Err(e) => errors.push(format!("{}: {e}", i.path.display())),
        }
    }
    (freed, errors)
}

fn human(b: u64) -> String {
    const U: [&str; 4] = ["B", "KB", "MB", "GB"];
    let (mut v, mut u) = (b as f64, 0);
    while v >= 1024.0 && u < 3 {
        v /= 1024.0;
        u += 1;
    }
    if u == 0 {
        format!("{b} B")
    } else {
        format!("{v:.1} {}", U[u])
    }
}

pub fn parse(args: &[String]) -> Result<Options, String> {
    let mut o = Options::default();
    let mut explicit = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut path_arg = |flag: &str| {
            it.next()
                .map(PathBuf::from)
                .ok_or(format!("{flag} needs a directory"))
        };
        match a.as_str() {
            "--shader-cache" => {
                o.shader_cache = true;
                explicit = true;
            }
            "--build-cache" => {
                o.build_cache = Some(path_arg("--build-cache")?);
                explicit = true;
            }
            "--engine" => {
                o.engine = Some(path_arg("--engine")?);
                explicit = true;
            }
            "--save-root" => o.save_root = Some(path_arg("--save-root")?),
            "--dry-run" | "-n" => o.dry_run = true,
            "-h" | "--help" => return Err(USAGE.to_string()),
            x => return Err(format!("unknown option {x}\n{USAGE}")),
        }
    }
    if !explicit {
        o.shader_cache = true;
    }
    Ok(o)
}

const USAGE: &str = "usage: napple clean [--shader-cache] [--build-cache <game dir>] [--engine <game dir>] [--save-root <dir>] [--dry-run]
  --shader-cache   <save root>/<title id>/cache for every title (default selection; save root defaults to ~/Library/Application Support/Napple)
  --build-cache D  D/.napple-cache.nosync (resumable RomFS copies)
  --engine D       napple-runtime, napple-runtime-ios and napple-launcher copies inside D (napple build restores them)
  --dry-run, -n    list what would be removed and the sizes, remove nothing
Saves, keys, firmware and game files are never touched.";

pub fn run(args: &[String]) -> Result<(), String> {
    let o = parse(args)?;
    let items = plan(&o);
    if items.is_empty() {
        println!("nothing to clean");
        return Ok(());
    }
    for i in &items {
        println!(
            "{:>10}  {:<13} {}",
            human(i.bytes),
            i.kind,
            i.path.display()
        );
    }
    let total: u64 = items.iter().map(|i| i.bytes).sum();
    if o.dry_run {
        println!(
            "dry run: {} in {} item(s) would be removed",
            human(total),
            items.len()
        );
        return Ok(());
    }
    let (freed, errors) = execute(&items);
    println!(
        "removed {} in {} item(s)",
        human(freed),
        items.len() - errors.len()
    );
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("napple-clean-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }
    fn put(p: &Path, n: usize) {
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, vec![b'x'; n]).unwrap();
    }

    #[test]
    fn shader_cache_only_touches_cache_not_saves() {
        let root = scratch("shader");
        put(&root.join("0100000000010000/cache/shaders/a.msl"), 100);
        put(
            &root.join("0100000000010000/cache/pipelines-v9.metalar"),
            50,
        );
        put(&root.join("0100000000010000/save/slot0.bin"), 7);
        put(&root.join("01006BB00C6F0000/cache/x"), 10);
        let o = Options {
            shader_cache: true,
            save_root: Some(root.clone()),
            dry_run: true,
            ..Default::default()
        };
        let items = plan(&o);
        assert_eq!(items.len(), 2);
        assert_eq!(items[0].bytes, 150);
        assert!(items.iter().all(|i| i.kind == "shader-cache"));
        let (freed, errs) = execute(&items);
        assert!(errs.is_empty());
        assert_eq!(freed, 160);
        assert!(!root.join("0100000000010000/cache").exists());
        assert!(
            root.join("0100000000010000/save/slot0.bin").exists(),
            "saves must survive"
        );
        assert!(plan(&o).is_empty());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn game_dir_build_cache_and_engine_copies() {
        let g = scratch("game");
        put(&g.join(".napple-cache.nosync/abc/romfs.bin"), 1000);
        put(&g.join("Game.app/Contents/MacOS/napple-launcher"), 20);
        put(&g.join("Game.app/Contents/Resources/napple-runtime"), 300);
        put(&g.join("Game.app/Contents/Resources/main.npdm"), 5);
        let o = Options {
            build_cache: Some(g.clone()),
            engine: Some(g.clone()),
            save_root: Some(g.join("none")),
            ..Default::default()
        };
        let items = plan(&o);
        let kinds: Vec<_> = items.iter().map(|i| i.kind).collect();
        assert_eq!(kinds, ["build-cache", "engine", "engine"]);
        assert_eq!(items.iter().map(|i| i.bytes).sum::<u64>(), 1320);
        let (_, errs) = execute(&items);
        assert!(errs.is_empty());
        assert!(
            g.join("Game.app/Contents/Resources/main.npdm").exists(),
            "game files must survive"
        );
        let _ = fs::remove_dir_all(g);
    }

    #[test]
    fn parse_defaults_and_errors() {
        let o = parse(&[]).unwrap();
        assert!(o.shader_cache && !o.dry_run);
        let o = parse(&["--engine".into(), "/x".into(), "-n".into()]).unwrap();
        assert!(!o.shader_cache && o.engine.is_some() && o.dry_run);
        assert!(parse(&["--engine".into()]).is_err());
        assert!(parse(&["--bogus".into()]).is_err());
    }
}
