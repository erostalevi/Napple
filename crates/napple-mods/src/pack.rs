//! Packing a mod directory into a deterministic `.napmod`.
use crate::manifest::{self, Components, MANIFEST_PATH, SHA_MANIFEST_PATH};
use crate::model::{LoadedMod, MAX_FILE};
use crate::util::{Result, err, sha256_hex};
use crate::validate::validate;
use std::io::Write;
use std::path::Path;

/// Reads `dir`, checks it and writes `out`. Fails on validation errors. Returns the SHA-256 of the package.
pub fn pack_dir(dir: &Path, out: &Path) -> Result<String> {
    if !dir.is_dir() {
        return err(format!("{} is not a directory", dir.display()));
    }
    let m = LoadedMod::open(dir)?;
    let rep = validate(&m);
    if !rep.ok() {
        return err(format!(
            "mod does not validate:\n  {}",
            rep.errors.join("\n  ")
        ));
    }
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    for p in m.listing.entries.keys() {
        if p == SHA_MANIFEST_PATH {
            continue; // always regenerated
        }
        files.push((p.clone(), m.files.read(p, MAX_FILE)?));
    }
    if m.manifest.components.is_none() {
        let c = Components {
            patches: m.patch_paths(),
            cheats: m.cheat_paths(),
            romfs: m.capabilities().romfs,
            config: m.capabilities().config,
            shaders: m.capabilities().shaders,
        };
        let mut mf = m.manifest.clone();
        mf.components = Some(c);
        let bytes = manifest::to_json(&mf);
        if let Some(e) = files.iter_mut().find(|(p, _)| p == MANIFEST_PATH) {
            e.1 = bytes;
        }
    }
    let mut sums = String::new();
    for (p, b) in &files {
        sums.push_str(&format!("{}  {}\n", sha256_hex(b), p));
    }
    files.push((SHA_MANIFEST_PATH.to_string(), sums.into_bytes()));
    files.sort_by(|a, b| a.0.cmp(&b.0));
    let mut buf = Vec::new();
    {
        let mut z = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
        let opts = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .last_modified_time(zip::DateTime::default())
            .unix_permissions(0o644)
            .large_file(false);
        for (p, b) in &files {
            z.start_file(
                p.as_str(),
                opts.large_file(b.len() as u64 >= u32::MAX as u64),
            )?;
            z.write_all(b)?;
        }
        z.finish()?;
    }
    if let Some(parent) = out.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(out, &buf)?;
    Ok(sha256_hex(&buf))
}
