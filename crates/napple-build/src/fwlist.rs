//! `napple build --list-firmware <firmware zip|dir> -k <prod.keys> [--filter <text>]`: lists the system titles of a firmware
//! (id, kind, version, contents; program NPDM name and data archive file counts). Read-only; writes nothing.
use crate::error::{BuildError, Result};
use crate::firmware::Firmware;
use napple_core::infrastructure::cnmt::{ContentRecord, TitleKind};
use napple_core::infrastructure::nca::{self, FsType, Nca, TitleKeys};
use napple_core::infrastructure::npdm::parse_npdm;
use napple_core::infrastructure::pfs::parse_pfs0;

pub fn run(args: &[String]) -> std::result::Result<(), String> {
    inner(args).map_err(|e| e.to_string())
}

fn describe(
    fw: &Firmware,
    keys: &napple_core::domain::keys::KeySet,
    rec: &ContentRecord,
) -> String {
    let kind = [
        "meta", "program", "data", "control", "html", "legal", "delta",
    ]
    .get(rec.ctype as usize)
    .copied()
    .unwrap_or("?");
    let mut s = format!("    {} {kind} {} bytes", rec.nca_name(), rec.size);
    let Some(src) = fw.nca_src(&rec.nca_name()) else {
        return s + " (missing from firmware)";
    };
    let Ok(n) = Nca::open(src, keys, &TitleKeys::new()) else {
        return s + " (cannot open)";
    };
    for sec in &n.sections {
        let Ok(d) = n.section(sec.index) else {
            continue;
        };
        match sec.fs_type {
            FsType::Pfs0 => {
                if let Ok(pfs) = nca::pfs0_data(&d, sec).and_then(parse_pfs0) {
                    let names: Vec<_> = pfs.entries.iter().map(|e| e.name.clone()).collect();
                    s += &format!("\n      exefs: {}", names.join(" "));
                    if let Ok(m) = pfs.read("main.npdm").and_then(|b| parse_npdm(&b)) {
                        let sv: Vec<_> = m
                            .services
                            .iter()
                            .filter(|x| !x.host)
                            .map(|x| x.name.as_str())
                            .collect();
                        let hv: Vec<_> = m
                            .services
                            .iter()
                            .filter(|x| x.host)
                            .map(|x| x.name.as_str())
                            .collect();
                        s += &format!(
                            "\n      npdm: name={:?} 64bit={} title={:016x} hosts: {} svcs-used: {}",
                            m.name,
                            m.is_64bit,
                            m.title_id,
                            hv.join(","),
                            sv.join(",")
                        );
                    }
                }
            }
            FsType::RomFs => {
                if let Ok(r) = nca::romfs_data(&d, sec)
                    .and_then(napple_core::infrastructure::romfs::RomFs::parse)
                {
                    let first: Vec<_> = r
                        .files
                        .iter()
                        .take(24)
                        .map(|f| format!("{}({})", f.path, f.size))
                        .collect();
                    s += &format!(
                        "\n      romfs: {} files: {}",
                        r.files.len(),
                        first.join(" ")
                    );
                }
            }
        }
    }
    s
}

fn inner(args: &[String]) -> Result<()> {
    let (mut fw, mut keys, mut filter, mut ids) = (None, None, None, Vec::new());
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || {
            it.next()
                .cloned()
                .ok_or_else(|| BuildError::usage(format!("{a} needs a value")))
        };
        match a.as_str() {
            "--list-firmware" => fw = Some(val()?),
            "-k" => keys = Some(val()?),
            "--filter" => filter = Some(val()?.to_lowercase()),
            "--id" => ids.push(
                u64::from_str_radix(val()?.trim_start_matches("0x"), 16)
                    .map_err(|_| BuildError::usage("--id takes hex"))?,
            ),
            x => {
                return Err(BuildError::usage(format!(
                    "unknown argument {x}\nusage: napple build --list-firmware <firmware> -k <prod.keys> [--filter text] [--id hex]..."
                )));
            }
        }
    }
    let (Some(fw), Some(keys)) = (fw, keys) else {
        return Err(BuildError::usage(
            "usage: napple build --list-firmware <firmware> -k <prod.keys> [--filter text] [--id hex]...",
        ));
    };
    let keys = crate::util::load_keys(std::path::Path::new(&keys))?;
    let fwi = Firmware::open(std::path::Path::new(&fw))?;
    for t in fwi.titles(&keys)? {
        let m = &t.meta;
        if !ids.is_empty() && !ids.contains(&m.title_id) {
            continue;
        }
        let kind = match m.kind {
            TitleKind::Other(n) => format!("type{n:#x}"),
            k => format!("{k:?}"),
        };
        let lines: Vec<String> = m
            .contents
            .iter()
            .filter(|c| c.ctype != 0)
            .map(|c| describe(&fwi, &keys, c))
            .collect();
        let text = format!(
            "{:016x} v{} {kind} app={:016x}\n{}",
            m.title_id,
            m.version,
            m.application_id,
            lines.join("\n")
        );
        if filter
            .as_ref()
            .is_none_or(|f| text.to_lowercase().contains(f))
        {
            println!("{text}");
        }
    }
    Ok(())
}
