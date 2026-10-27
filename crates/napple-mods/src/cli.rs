//! `napple mods pack|inspect|validate|apply|convert|build-id`.
use crate::apply::{ModuleBytes, apply_mods_to_module, dry_run_mods_for_module};
use crate::ips::{self, IpsPatch, IpsRecord};
use crate::model::{LoadedMod, Selection};
use crate::pack::pack_dir;
use crate::pchtxt::{self, PchBuild, PchPatch, PchRecord, Pchtxt};
use crate::util::{build_id_hex, norm_build_id, safe_rel_path};
use crate::validate::validate;
use serde_json::json;
use std::path::{Path, PathBuf};

const USAGE: &str = "usage:
  napple mods pack <dir> [-o <file.napmod>]
  napple mods inspect <mod> [--json]
  napple mods validate <mod> [--json]                       (exit 1 when there are errors)
  napple mods apply <mod> --image <file> --build-id <hex> [--layout <text_end>,<ro_end>] [--enable-code]
        [--enable <patch>[#<block>]] [--disable <patch>[#<block>]] [--dry-run | -o <patched image>] [--json]
  napple mods convert <in> <out> [--build-id <hex>]          (.pchtxt, .json and .ips in either direction)
  napple mods build-id <file.nso>
<mod> is a .napmod file or an unpacked mod directory. Offsets in patches are NSO file offsets (see docs/mods/format.md).";

fn num(s: &str) -> Result<usize, String> {
    let t = s.trim();
    match t.strip_prefix("0x") {
        Some(h) => usize::from_str_radix(h, 16),
        None => t.parse(),
    }
    .map_err(|_| format!("bad number {s:?}"))
}

fn read_limited(p: &Path, limit: u64) -> Result<Vec<u8>, String> {
    let m = std::fs::metadata(p).map_err(|e| format!("{}: {e}", p.display()))?;
    if m.len() > limit {
        return Err(format!("{}: larger than {} MiB", p.display(), limit >> 20));
    }
    std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))
}

pub fn run(args: &[String]) -> Result<(), String> {
    let Some(cmd) = args.first() else {
        return Err(USAGE.into());
    };
    let rest = &args[1..];
    match cmd.as_str() {
        "pack" => pack(rest),
        "inspect" => inspect(rest),
        "validate" => validate_cmd(rest),
        "apply" => apply(rest),
        "convert" => convert(rest),
        "build-id" => build_id(rest),
        _ => Err(USAGE.into()),
    }
}

fn pack(a: &[String]) -> Result<(), String> {
    let mut dir = None;
    let mut out: Option<PathBuf> = None;
    let mut it = a.iter();
    while let Some(x) = it.next() {
        match x.as_str() {
            "-o" => out = Some(it.next().ok_or(USAGE)?.into()),
            d if dir.is_none() => dir = Some(PathBuf::from(d)),
            _ => return Err(USAGE.into()),
        }
    }
    let dir = dir.ok_or(USAGE)?;
    let out = match out {
        Some(o) => o,
        None => {
            let m = LoadedMod::open(&dir).map_err(|e| e.to_string())?;
            let (id, v) = (&m.manifest.id, &m.manifest.version);
            safe_rel_path(id)
                .and(safe_rel_path(v))
                .map_err(|e| e.to_string())?;
            PathBuf::from(format!("{id}-{v}.napmod"))
        }
    };
    let h = pack_dir(&dir, &out).map_err(|e| e.to_string())?;
    println!("packed {} (sha256 {h})", out.display());
    Ok(())
}

fn open(a: &[String]) -> Result<(LoadedMod, bool), String> {
    let json = a.iter().any(|x| x == "--json");
    let p = a.iter().find(|x| !x.starts_with("--")).ok_or(USAGE)?;
    Ok((
        LoadedMod::open(Path::new(p)).map_err(|e| e.to_string())?,
        json,
    ))
}

fn inspect(a: &[String]) -> Result<(), String> {
    let (m, json) = open(a)?;
    let rep = validate(&m);
    let content = m.content_sha256().map_err(|e| e.to_string())?;
    let c = rep.capabilities;
    if json {
        let v = json!({
            "id": m.manifest.id, "name": m.manifest.name, "version": m.manifest.version,
            "titleId": m.manifest.title_id, "buildIds": m.manifest.build_ids, "author": m.manifest.author,
            "license": m.manifest.license, "description": m.manifest.description,
            "packageSha256": m.package_sha256, "contentSha256": content,
            "signed": false, "capabilities": c, "files": rep.file_count, "bytes": rep.uncompressed_bytes,
            "patchRecords": rep.patch_records, "cheats": rep.cheat_count,
            "errors": rep.errors, "warnings": rep.warnings,
        });
        println!("{}", serde_json::to_string_pretty(&v).unwrap_or_default());
        return Ok(());
    }
    println!(
        "{} {} ({})",
        m.manifest.name, m.manifest.version, m.manifest.id
    );
    println!(
        "title      {}   builds {}",
        m.manifest.title_id,
        if m.manifest.build_ids.is_empty() {
            "any".into()
        } else {
            m.manifest.build_ids.join(", ")
        }
    );
    println!(
        "author     {}   license {}",
        m.manifest.author, m.manifest.license
    );
    println!("signature  none (unsigned: only enable mods from people you trust)");
    if let Some(h) = &m.package_sha256 {
        println!("package    sha256 {h}");
    }
    println!(
        "content    sha256 {content}   hash manifest: {}",
        rep.hash_manifest
    );
    let yn = |b: bool| if b { "YES" } else { "no" };
    println!(
        "contains   code patches: {} ({} records)   cheats: {} ({})   romfs: {}   config: {}   shaders: {}",
        yn(c.code_patches),
        rep.patch_records,
        yn(c.cheats),
        rep.cheat_count,
        yn(c.romfs),
        yn(c.config),
        yn(c.shaders)
    );
    if c.code_patches {
        println!(
            "           code patches change the game's code: they stay off until you enable them for this mod"
        );
    }
    println!(
        "size       {} files, {} bytes",
        rep.file_count, rep.uncompressed_bytes
    );
    if !m.manifest.description.is_empty() {
        println!("\n{}", m.manifest.description);
    }
    for w in &rep.warnings {
        println!("warning: {w}");
    }
    for e in &rep.errors {
        println!("error: {e}");
    }
    Ok(())
}

fn validate_cmd(a: &[String]) -> Result<(), String> {
    let (m, json) = open(a)?;
    let rep = validate(&m);
    if json {
        println!("{}", serde_json::to_string_pretty(&rep).unwrap_or_default());
    } else {
        for w in &rep.warnings {
            println!("warning: {w}");
        }
        for e in &rep.errors {
            println!("error: {e}");
        }
        println!(
            "{}: {} error(s), {} warning(s)",
            m.source.display(),
            rep.errors.len(),
            rep.warnings.len()
        );
    }
    if rep.ok() {
        Ok(())
    } else {
        Err(format!("{} validation error(s)", rep.errors.len()))
    }
}

fn apply(a: &[String]) -> Result<(), String> {
    let (mut modp, mut image, mut bid, mut layout, mut out) = (None, None, None, None, None);
    let (mut dry, mut json, mut code) = (false, false, false);
    let mut sel = Selection {
        enabled: true,
        ..Default::default()
    };
    let mut it = a.iter();
    while let Some(x) = it.next() {
        match x.as_str() {
            "--image" => image = Some(it.next().ok_or(USAGE)?.clone()),
            "--build-id" => bid = Some(it.next().ok_or(USAGE)?.clone()),
            "--layout" => layout = Some(it.next().ok_or(USAGE)?.clone()),
            "-o" => out = Some(it.next().ok_or(USAGE)?.clone()),
            "--dry-run" => dry = true,
            "--json" => json = true,
            "--enable-code" => code = true,
            "--enable" => {
                sel.overrides.insert(it.next().ok_or(USAGE)?.clone(), true);
            }
            "--disable" => {
                sel.overrides.insert(it.next().ok_or(USAGE)?.clone(), false);
            }
            p if modp.is_none() => modp = Some(p.to_string()),
            _ => return Err(USAGE.into()),
        }
    }
    sel.code_patches = code;
    if !dry && out.is_none() {
        return Err("apply needs --dry-run or -o <patched image>".into());
    }
    let m = LoadedMod::load(Path::new(&modp.ok_or(USAGE)?)).map_err(|e| e.to_string())?;
    let build_id = norm_build_id(&bid.ok_or(USAGE)?).map_err(|e| e.to_string())?;
    let img = read_limited(Path::new(&image.ok_or(USAGE)?), 1 << 30)?;
    let (te, re) = match layout {
        Some(l) => {
            let (t, r) = l.split_once(',').ok_or("--layout is <text_end>,<ro_end>")?;
            (num(t)?, num(r)?)
        }
        None => (img.len(), img.len()),
    };
    let mut module = ModuleBytes::from_flat(&img, te, re);
    let resolved = m.resolve(sel).map_err(|e| e.to_string())?;
    let rep = if dry {
        dry_run_mods_for_module(&build_id, &module, std::slice::from_ref(&resolved))
    } else {
        apply_mods_to_module(&build_id, &mut module, std::slice::from_ref(&resolved))
    };
    if json {
        println!("{}", rep.to_json());
    } else {
        println!(
            "module {}  {}",
            rep.build_id,
            if rep.dry_run { "(dry run)" } else { "" }
        );
        for h in &rep.hits {
            println!(
                "  hit   {:#010x} (+{:#x}) {:>5} B {:?}  {}",
                h.file_offset, h.module_offset, h.len, h.segment, h.source
            );
        }
        for x in &rep.misses {
            println!(
                "  MISS  {:#010x} {:>5} B {:?}  {}",
                x.file_offset, x.len, x.reason, x.source
            );
        }
        for o in &rep.overlaps {
            println!(
                "  overlap at +{:#x} ({} B, {}): {} / {}",
                o.module_offset,
                o.len,
                if o.identical {
                    "identical"
                } else {
                    "DIFFERENT, later wins"
                },
                o.first,
                o.second
            );
        }
        for s in &rep.skipped {
            println!("  skip  {}: {}", s.source, s.reason);
        }
        println!(
            "{} hit(s), {} miss(es), {} overlap(s), {} skipped, {} byte(s) {}\nsha256 {} -> {}",
            rep.hits.len(),
            rep.misses.len(),
            rep.overlaps.len(),
            rep.skipped.len(),
            rep.bytes_patched,
            if dry { "would change" } else { "changed" },
            rep.sha256_before,
            rep.sha256_after
        );
    }
    if let Some(o) = out {
        std::fs::write(&o, module.flatten()).map_err(|e| format!("{o}: {e}"))?;
    }
    Ok(())
}

fn convert(a: &[String]) -> Result<(), String> {
    let mut pos = vec![];
    let mut bid = None;
    let mut it = a.iter();
    while let Some(x) = it.next() {
        if x == "--build-id" {
            bid = Some(norm_build_id(it.next().ok_or(USAGE)?).map_err(|e| e.to_string())?);
        } else {
            pos.push(x.clone());
        }
    }
    let [inp, outp] = pos.as_slice() else {
        return Err(USAGE.into());
    };
    let ext = |p: &str| {
        Path::new(p)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("")
            .to_ascii_lowercase()
    };
    let bytes = read_limited(Path::new(inp), 64 << 20)?;
    let text = |b: &[u8]| String::from_utf8(b.to_vec()).map_err(|_| format!("{inp}: not UTF-8"));
    // everything goes through the pchtxt model
    let model: Pchtxt = match ext(inp).as_str() {
        "pchtxt" => pchtxt::parse(&text(&bytes)?).map_err(|e| e.to_string())?,
        "json" => {
            let v: serde_json::Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
            pchtxt::from_json(&v).map_err(|e| e.to_string())?
        }
        "ips" => {
            let p = ips::parse(&bytes).map_err(|e| e.to_string())?;
            let build_id = bid.ok_or("converting an .ips needs --build-id <module build id>")?;
            Pchtxt {
                builds: vec![PchBuild {
                    build_id,
                    patches: vec![PchPatch {
                        name: "ips".into(),
                        enabled: true,
                        records: p
                            .records
                            .into_iter()
                            .map(|r| PchRecord {
                                offset: r.offset,
                                data: r.data,
                            })
                            .collect(),
                    }],
                }],
                warnings: vec![],
            }
        }
        _ => return Err(format!("{inp}: input must be .pchtxt, .json or .ips")),
    };
    for w in &model.warnings {
        eprintln!("warning: {w}");
    }
    let out: Vec<u8> = match ext(outp).as_str() {
        "pchtxt" => pchtxt::write(&model).into_bytes(),
        "json" => (serde_json::to_string_pretty(&pchtxt::to_json(&model)).unwrap_or_default()
            + "\n")
            .into_bytes(),
        "ips" => {
            let b = match (&bid, model.builds.as_slice()) {
                (Some(id), bs) => bs
                    .iter()
                    .find(|b| &b.build_id == id)
                    .ok_or(format!("no build {} in the input", build_id_hex(id)))?,
                (None, [b]) => b,
                _ => return Err("the input has several builds: choose one with --build-id".into()),
            };
            let records: Vec<IpsRecord> = b
                .patches
                .iter()
                .filter(|p| p.enabled)
                .flat_map(|p| {
                    p.records.iter().map(|r| IpsRecord {
                        offset: r.offset,
                        data: r.data.clone(),
                    })
                })
                .collect();
            let wide = records
                .iter()
                .any(|r| r.offset + r.data.len() as u64 > 0xFF_FFFF);
            ips::write(&IpsPatch {
                wide,
                records,
                truncate: None,
            })
            .map_err(|e| e.to_string())?
        }
        _ => return Err(format!("{outp}: output must be .pchtxt, .json or .ips")),
    };
    std::fs::write(outp, out).map_err(|e| format!("{outp}: {e}"))?;
    println!("wrote {outp}");
    Ok(())
}

fn build_id(a: &[String]) -> Result<(), String> {
    let [p] = a else {
        return Err(USAGE.into());
    };
    let b = read_limited(Path::new(p), 1 << 30)?;
    if b.len() < 0x60 || &b[..4] != b"NSO0" {
        return Err(format!("{p}: not an NSO"));
    }
    let id: [u8; 32] = b[0x40..0x60].try_into().map_err(|_| "short")?;
    println!("{}", build_id_hex(&id));
    Ok(())
}
