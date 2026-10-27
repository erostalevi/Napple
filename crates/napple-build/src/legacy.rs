//! Legacy single-purpose subcommands used by tools/prepare_game.sh: `--dump-exefs` and `--dump-romfs`.
use napple_core::application::ingest::ingest;
use napple_core::domain::error::Error as CoreError;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{FileSource, Src};
use napple_core::infrastructure::nca::{self, ContentType, FsType, Nca, TitleKeys};
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::{
    ticket::{encrypted_title_key, parse_ticket},
    xci::parse_xci,
};
use std::io::Write;
use std::path::{Path, PathBuf};

pub(crate) const USAGE: &str = "usage:
  napple build --dump-romfs <game.nsp|xci> -k <prod.keys> -o <file>  stream the decrypted RomFS image (romfs.bin) to <file>
  napple build --dump-exefs <game.nsp|xci> -k <prod.keys> -o <dir>   write the ExeFS files (NSO, main.npdm) to <dir>";

pub(crate) struct Opts {
    dump_exefs: Option<PathBuf>,
    dump_romfs: Option<PathBuf>,
    keys: Option<PathBuf>,
    out: Option<PathBuf>,
}

pub(crate) fn parse(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts {
        dump_exefs: None,
        dump_romfs: None,
        keys: None,
        out: None,
    };
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = |n: &str| {
            it.next()
                .map(PathBuf::from)
                .ok_or_else(|| format!("{n} needs a value"))
        };
        match a.as_str() {
            "--dump-exefs" => o.dump_exefs = Some(val("--dump-exefs")?),
            "--dump-romfs" => o.dump_romfs = Some(val("--dump-romfs")?),
            "-k" => o.keys = Some(val("-k")?),
            "-o" => o.out = Some(val("-o")?),
            x => return Err(format!("unknown argument {x}\n{USAGE}")),
        }
    }
    Ok(o)
}

fn progress(stage: &str, pct: u32, msg: &str) {
    println!("PROGRESS {stage} {pct} {msg}");
}

fn safe_name(n: &str) -> bool {
    !n.is_empty() && !n.contains(['/', '\\', '\0']) && n != "." && n != ".."
}

fn dump_romfs(game: &Path, keys: &KeySet, out: &Path) -> Result<(), String> {
    let e = |x: CoreError| x.to_string();
    progress("romfs", 0, "reading container");
    let src = FileSource::open(game).map_err(e)?;
    let magic = napple_core::infrastructure::io::read_vec(&*src, 0, 4).map_err(e)?;
    let mut parts = Vec::new();
    if magic == b"PFS0" {
        parts.push(parse_pfs0(src).map_err(e)?);
    } else {
        parts.extend(
            parse_xci(src)
                .map_err(e)?
                .into_iter()
                .filter(|(n, _)| n != "logo")
                .map(|(_, f)| f),
        );
    }
    let (mut ncas, mut tks): (Vec<(String, Src)>, TitleKeys) = (Vec::new(), TitleKeys::new());
    for fs in &parts {
        for en in &fs.entries {
            if en.name.ends_with(".nca") {
                ncas.push((en.name.clone(), fs.open(&en.name).map_err(e)?));
            } else if en.name.ends_with(".ncz") {
                ncas.push((
                    napple_core::infrastructure::ncz::nca_name(&en.name),
                    napple_core::infrastructure::ncz::NczSource::open(
                        fs.open(&en.name).map_err(e)?,
                    )
                    .map_err(e)?,
                ));
            } else if en.name.ends_with(".tik") {
                let t = parse_ticket(&fs.read(&en.name).map_err(e)?).map_err(e)?;
                if let Some(k) = encrypted_title_key(&t, keys).map_err(e)? {
                    tks.insert(t.rights_id, k);
                }
            }
        }
    }
    for (_, s) in ncas {
        let n = match Nca::open(s, keys, &tks) {
            Ok(n) => n,
            Err(_) => continue,
        };
        if n.content_type != ContentType::Program {
            continue;
        }
        if let Some(sec) = n.sections.iter().find(|x| x.fs_type == FsType::RomFs) {
            let section = n.section(sec.index).map_err(e)?;
            let data = nca::romfs_data(&section, sec).map_err(e)?;
            let total = data.size();
            let tmp = out.with_extension("part");
            let mut f = std::fs::File::create(&tmp).map_err(|x| x.to_string())?;
            let mut buf = vec![0u8; 8 << 20];
            let mut off = 0u64;
            let mut last = 101;
            while off < total {
                let n = ((total - off) as usize).min(buf.len());
                data.read_at(off, &mut buf[..n]).map_err(e)?;
                f.write_all(&buf[..n]).map_err(|x| x.to_string())?;
                off += n as u64;
                let pct = (off * 100 / total.max(1)) as u32;
                if pct != last {
                    progress("romfs", pct, "writing romfs.bin");
                    last = pct;
                }
            }
            f.flush().map_err(|x| x.to_string())?;
            std::fs::rename(&tmp, out).map_err(|x| x.to_string())?;
            println!("romfs bytes: {total}");
            return Ok(());
        }
    }
    Err("no Program NCA with a RomFS section found".into())
}

pub fn run(args: &[String]) -> Result<(), String> {
    let o = parse(args)?;
    if let (Some(g), Some(k), Some(out), None) = (&o.dump_romfs, &o.keys, &o.out, &o.dump_exefs) {
        let keys = KeySet::load(k).map_err(|x| x.to_string())?;
        return dump_romfs(g, &keys, out);
    }
    let (Some(game), Some(keys), Some(out)) = (o.dump_exefs, o.keys, o.out) else {
        return Err(USAGE.into());
    };
    progress("ingest", 0, "reading container");
    let keys = KeySet::load(&keys).map_err(|e| e.to_string())?;
    let r = ingest(&game, &keys).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    let n = r.exefs.len().max(1);
    for (i, (name, data)) in r.exefs.iter().enumerate() {
        if !safe_name(name) {
            return Err(format!("unsafe ExeFS entry name {name:?}"));
        }
        std::fs::write(out.join(name), data).map_err(|e| e.to_string())?;
        progress("exefs", ((i + 1) * 100 / n) as u32, name);
    }
    println!("title id: {:016X}", r.title_id.unwrap_or(0));
    Ok(())
}
