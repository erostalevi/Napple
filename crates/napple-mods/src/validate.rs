//! Validation and the trust summary of a mod.
use crate::cheats;
use crate::manifest::{self, Components};
use crate::model::{Capabilities, LoadedMod, MAX_FILE, PatchKind, is_patch_path};
use crate::shader;
use crate::util::ModError;
use serde::Serialize;
use std::collections::BTreeMap;

/// Files that must never ship in a mod (keys), and containers/dumps that are warned about.
const KEY_FILES: &[&str] = &["prod.keys", "title.keys", "dev.keys", "console.keys"];
const DUMP_EXT: &[&str] = &[
    "nsp", "xci", "nca", "xcz", "nsz", "nso", "nro", "elf", "keys", "tik", "cnmt",
];
const DUMP_NAMES: &[&str] = &["romfs.bin", "exefs.bin", "main", "rtld", "sdk", "main.npdm"];
pub const WARN_FILE_BYTES: u64 = 256 << 20;
pub const WARN_ROMFS_BYTES: u64 = 1 << 30;
pub const WARN_ROMFS_FILES: usize = 5000;
pub const WARN_PATCH_BYTES: u64 = 4 << 20;

#[derive(Debug, Clone, Default, Serialize)]
pub struct Report {
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
    pub capabilities: Capabilities,
    pub file_count: usize,
    pub uncompressed_bytes: u64,
    pub patch_records: usize,
    pub cheat_count: usize,
    pub signed: bool,
    pub hash_manifest: &'static str,
}

impl Report {
    pub fn ok(&self) -> bool {
        self.errors.is_empty()
    }
}

fn ext(p: &str) -> String {
    p.rsplit('/')
        .next()
        .unwrap_or("")
        .rsplit_once('.')
        .map(|(_, e)| e.to_ascii_lowercase())
        .unwrap_or_default()
}

pub fn validate(m: &LoadedMod) -> Report {
    let mut r = Report {
        capabilities: m.capabilities(),
        file_count: m.listing.entries.len(),
        ..Default::default()
    };
    r.uncompressed_bytes = m.listing.entries.values().sum();
    r.errors
        .extend(m.listing.bad.iter().map(|b| format!("unsafe entry: {b}")));
    r.warnings.extend(m.manifest_warnings.iter().cloned());
    r.errors.extend(manifest::check(&m.manifest));
    if m.manifest.author.trim().is_empty() {
        r.warnings.push("mod.json: author is empty".into());
    }
    if m.manifest.license.trim().is_empty() {
        r.warnings
            .push("mod.json: license is empty (say who may share this mod)".into());
    }
    if m.manifest.build_ids.is_empty() {
        r.warnings.push(
            "mod.json: buildIds is empty, the mod claims to fit every version of the title".into(),
        );
    }
    r.signed = false;
    r.hash_manifest = "absent";

    // case-insensitive collisions
    let mut lower: BTreeMap<String, &String> = BTreeMap::new();
    for p in m.listing.entries.keys() {
        if let Some(o) = lower.insert(p.to_ascii_lowercase(), p) {
            r.warnings.push(format!(
                "{p} and {o} differ only by case (collide on case-insensitive volumes)"
            ));
        }
    }

    // declared vs present components
    let caps = r.capabilities;
    let declared: Option<Components> = m.manifest.components.clone();
    if let Some(d) = &declared {
        let have_p = m.patch_paths();
        for p in &d.patches {
            if !have_p.contains(p) {
                r.errors.push(format!(
                    "components.patches lists {p} which is not a patch file in the mod"
                ));
            }
        }
        for p in &have_p {
            if !d.patches.contains(p) {
                r.errors.push(format!(
                    "{p} is a code patch not declared in components.patches"
                ));
            }
        }
        let have_c = m.cheat_paths();
        for p in &d.cheats {
            if !have_c.contains(p) {
                r.errors.push(format!(
                    "components.cheats lists {p} which is not in the mod"
                ));
            }
        }
        for p in &have_c {
            if !d.cheats.contains(p) {
                r.errors.push(format!(
                    "{p} is a cheat file not declared in components.cheats"
                ));
            }
        }
        for (name, want, have) in [
            ("romfs", d.romfs, caps.romfs),
            ("config", d.config, caps.config),
            ("shaders", d.shaders, caps.shaders),
        ] {
            if want != have {
                r.errors.push(format!(
                    "components.{name} is {want} but the mod {} {name} files",
                    if have { "contains" } else { "has no" }
                ));
            }
        }
    }
    if !(caps.code_patches || caps.cheats || caps.romfs || caps.config || caps.shaders) {
        r.warnings
            .push("the mod contains no patches, cheats, romfs files, config or shaders".into());
    }

    // content policy
    let mut romfs_bytes = 0u64;
    let mut romfs_files = 0usize;
    for (p, size) in &m.listing.entries {
        let name = p.rsplit('/').next().unwrap_or("");
        if KEY_FILES.contains(&name.to_ascii_lowercase().as_str()) {
            r.errors
                .push(format!("{p}: key files must never be shared in a mod"));
        } else if DUMP_EXT.contains(&ext(p).as_str())
            || DUMP_NAMES.contains(&name.to_ascii_lowercase().as_str())
        {
            r.warnings.push(format!("{p}: looks like an extracted game or console file; mods must not contain copyrighted game data"));
        }
        if *size > WARN_FILE_BYTES {
            r.warnings.push(format!(
                "{p}: {} MiB in one file looks like an extracted asset dump",
                size >> 20
            ));
        }
        if *size > MAX_FILE {
            r.errors
                .push(format!("{p}: larger than {} GiB", MAX_FILE >> 30));
        }
        if p.starts_with("files/") {
            romfs_bytes += size;
            romfs_files += 1;
        }
    }
    if romfs_bytes > WARN_ROMFS_BYTES || romfs_files > WARN_ROMFS_FILES {
        r.warnings.push(format!(
            "romfs overlay holds {romfs_files} files / {} MiB: looks like a game data dump, not a mod",
            romfs_bytes >> 20
        ));
    }
    for p in m.paths("files/") {
        if let Err(e) = crate::romfs::overlay_path(p) {
            r.errors.push(format!("{p}: {e}"));
        }
    }

    // components parse
    match m.load_patches() {
        Err(e) => r.errors.push(e.to_string()),
        Ok(list) => {
            let mut bytes = 0u64;
            for pf in &list {
                let (recs, warns): (Vec<&[u8]>, Vec<String>) = match &pf.kind {
                    PatchKind::Ips { patch, .. } => {
                        (patch.records.iter().map(|x| &x.data[..]).collect(), vec![])
                    }
                    PatchKind::Pchtxt(t) => {
                        let mut w: Vec<String> = t
                            .warnings
                            .iter()
                            .map(|x| format!("{}: {x}", pf.path))
                            .collect();
                        if t.builds.is_empty() {
                            w.push(format!(
                                "{}: no @nsobid- line, the patch applies to no module",
                                pf.path
                            ));
                        }
                        (
                            t.builds
                                .iter()
                                .flat_map(|b| &b.patches)
                                .flat_map(|p| &p.records)
                                .map(|x| &x.data[..])
                                .collect(),
                            w,
                        )
                    }
                };
                r.warnings.extend(warns);
                r.patch_records += recs.len();
                for d in recs {
                    bytes += d.len() as u64;
                    if d.len() > (1 << 20) {
                        r.warnings.push(format!(
                            "{}: a single record of {} KiB looks like a code dump",
                            pf.path,
                            d.len() >> 10
                        ));
                    }
                }
                let offsets: Vec<u64> = match &pf.kind {
                    PatchKind::Ips { patch, .. } => {
                        patch.records.iter().map(|x| x.offset).collect()
                    }
                    PatchKind::Pchtxt(t) => t
                        .builds
                        .iter()
                        .flat_map(|b| &b.patches)
                        .flat_map(|p| &p.records)
                        .map(|x| x.offset)
                        .collect(),
                };
                if offsets.iter().any(|&o| o < crate::apply::NSO_HEADER) {
                    r.warnings.push(format!(
                        "{}: a record lies inside the 0x100 NSO header and will never apply",
                        pf.path
                    ));
                }
            }
            if bytes > WARN_PATCH_BYTES {
                r.warnings.push(format!(
                    "patches write {} KiB in total; large code blobs are suspect",
                    bytes >> 10
                ));
            }
        }
    }
    match m.load_cheats() {
        Err(e) => r.errors.push(e.to_string()),
        Ok(list) => {
            for c in &list {
                for ch in &c.file.cheats {
                    r.cheat_count += 1;
                    if let Some(why) = cheats::decode(&ch.words).unsupported {
                        r.warnings
                            .push(format!("{}: cheat {:?}: {why}", c.path, ch.name));
                    }
                }
            }
        }
    }
    match m.load_config() {
        Err(e) => r.errors.push(e.to_string()),
        Ok(Some((_, w))) => r.warnings.extend(w),
        Ok(None) => {}
    }
    for p in m.paths("shaders/") {
        if !p.ends_with(".msl") {
            r.warnings
                .push(format!("{p}: only .msl files are used under shaders/"));
            continue;
        }
        match m
            .files
            .read(p, shader::MAX_SHADER as u64 + 1)
            .map_err(|e| e.to_string())
            .and_then(|b| {
                shader::validate(p, &b)
                    .map(|_| ())
                    .map_err(|e| e.to_string())
            }) {
            Ok(()) => {}
            Err(e) => r.errors.push(e),
        }
    }
    if let Some(t) = &m.manifest.thumbnail {
        match m.listing.entries.get(t) {
            None => r.errors.push(format!("thumbnail {t} is not in the mod")),
            Some(&s) if s > (1 << 20) => {
                r.errors.push(format!("thumbnail {t} is larger than 1 MiB"))
            }
            Some(_) => match m.files.read(t, 1 << 20) {
                Ok(b)
                    if b.starts_with(&[0x89, b'P', b'N', b'G']) || b.starts_with(&[0xFF, 0xD8]) => {
                }
                Ok(_) => r
                    .warnings
                    .push(format!("thumbnail {t} is not a PNG or JPEG")),
                Err(e) => r.errors.push(e.to_string()),
            },
        }
    }
    for p in m.paths("patches/") {
        if !is_patch_path(p) {
            r.warnings
                .push(format!("{p}: not a .pchtxt or .ips file, ignored"));
        }
    }

    // hash manifest
    match m.verify_sha_manifest() {
        Err(ModError(e)) => r.errors.push(e),
        Ok(None) if m.is_archive => r
            .errors
            .push("manifest.sha256 is missing from the package".into()),
        Ok(None) => {}
        Ok(Some(p)) if p.is_empty() => r.hash_manifest = "ok",
        Ok(Some(p)) => {
            r.hash_manifest = "mismatch";
            r.errors.extend(p);
        }
    }
    r
}
