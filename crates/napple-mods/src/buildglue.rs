//! Glue for `napple build --mods`: load and validate the requested mods, apply their code patches to a module before the
//! AOT rewrite, key caches by the applied set, and write `mods/overlay.json` plus the overlay files for the runtime.
//!
//! Passing a mod to `--mods` is the explicit enable: the build selection turns on the mod, its code patches and cheats
//! (the user typed the path). Nothing here reads the network or the game; the portable core stays macOS-free.
use crate::apply::{ApplyReport, ModuleBytes, apply_mods_to_module};
use crate::model::{LoadedMod, ResolvedMod, Selection};
use crate::romfs::{self, Entry, OverlayMap};
use crate::util::{ModError, Result, build_id_hex, err, sha256_hex};
use crate::validate;
use std::path::{Path, PathBuf};

/// `<data dir>/mods/overlay.json`, relative to the data directory (version directory of the build).
pub const OVERLAY_JSON: &str = "mods/overlay.json";
/// Overlay files are copied to `<data dir>/mods/files/<mod index>/<romfs path>`.
pub const OVERLAY_FILES: &str = "mods/files";

/// The mods of one build, in order (later wins), loaded, validated and resolved.
pub struct ModSet {
    pub loaded: Vec<LoadedMod>,
    pub resolved: Vec<ResolvedMod>,
    pub warnings: Vec<String>,
}

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

impl ModSet {
    /// Loads every path (a `.napmod` or a mod folder). A validation error fails the build; a mod made for another title
    /// (when `title_id` is known) fails too. Warnings are kept for the progress log.
    pub fn load(paths: &[PathBuf], title_id: Option<u64>) -> Result<Self> {
        let mut set = ModSet {
            loaded: vec![],
            resolved: vec![],
            warnings: vec![],
        };
        for p in paths {
            let m = LoadedMod::load(p).map_err(|e| ModError(format!("{}: {e}", p.display())))?;
            let rep = validate::validate(&m);
            if !rep.ok() {
                return err(format!("{}: {}", p.display(), rep.errors.join("; ")));
            }
            if let Some(t) = title_id
                && !m
                    .manifest
                    .title_id
                    .eq_ignore_ascii_case(&format!("{t:016X}"))
            {
                return err(format!(
                    "{}: mod {} is for title {}, this build is {t:016X}",
                    p.display(),
                    m.manifest.id,
                    m.manifest.title_id
                ));
            }
            if set.loaded.iter().any(|o| o.manifest.id == m.manifest.id) {
                return err(format!(
                    "{}: mod id {} given twice",
                    p.display(),
                    m.manifest.id
                ));
            }
            set.warnings.extend(
                rep.warnings
                    .iter()
                    .map(|w| format!("{}: {w}", m.manifest.id)),
            );
            set.resolved.push(m.resolve(Selection {
                enabled: true,
                code_patches: true,
                cheats: true,
                shaders: true,
                overrides: Default::default(),
            })?);
            set.loaded.push(m);
        }
        Ok(set)
    }

    pub fn empty() -> Self {
        ModSet {
            loaded: vec![],
            resolved: vec![],
            warnings: vec![],
        }
    }

    pub fn is_empty(&self) -> bool {
        self.resolved.is_empty()
    }

    /// Stable key of the applied set (ids, versions, content hashes and selections, in order): `""` when empty.
    /// Caches and the manifest use it so a different mod set never reuses another set's patched output.
    pub fn key(&self) -> String {
        if self.is_empty() {
            return String::new();
        }
        let mut s = String::new();
        for r in &self.resolved {
            s.push_str(&format!(
                "{}@{}:{}:{}{}{}{};",
                r.id,
                r.version,
                r.content_sha256,
                r.selection.enabled as u8,
                r.selection.code_patches as u8,
                r.selection.cheats as u8,
                r.selection.shaders as u8,
            ));
        }
        sha256_hex(s.as_bytes())
    }

    /// The `"mods"` array of `manifest.json`, one object per applied mod with the per-module patch results.
    pub fn manifest_json(&self, reports: &[(String, ApplyReport)]) -> String {
        let items: Vec<String> = self
            .resolved
            .iter()
            .map(|r| {
                let tag = format!("{}@{}:", r.id, r.version);
                let mods: Vec<String> = reports
                    .iter()
                    .map(|(name, rep)| {
                        let n = |v: &[String]| v.iter().filter(|s| s.starts_with(&tag)).count();
                        let hits: Vec<String> = rep.hits.iter().map(|h| h.source.clone()).collect();
                        let miss: Vec<String> =
                            rep.misses.iter().map(|h| h.source.clone()).collect();
                        format!(
                            "{{\"module\":{},\"hits\":{},\"misses\":{}}}",
                            json_str(name),
                            n(&hits),
                            n(&miss)
                        )
                    })
                    .collect();
                format!(
                    "{{\"id\":{},\"version\":{},\"content_sha256\":{},\"modules\":[{}]}}",
                    json_str(&r.id),
                    json_str(&r.version),
                    json_str(&r.content_sha256),
                    mods.join(",")
                )
            })
            .collect();
        format!("[{}]", items.join(","))
    }

    /// Applies the code patches to one module (its own 32-byte build id) before the rewrite. Returns the report;
    /// a miss is a warning (the caller decides about strict mode).
    pub fn apply(&self, build_id: &[u8; 32], module: &mut ModuleBytes) -> ApplyReport {
        apply_mods_to_module(build_id, module, &self.resolved)
    }

    /// Writes `<dir>/mods/overlay.json` and copies the overlay files below `<dir>/mods/files/`. Returns the file count;
    /// writes nothing when no mod carries romfs content.
    pub fn write_overlay(&self, dir: &Path) -> Result<usize> {
        let refs: Vec<&LoadedMod> = self
            .loaded
            .iter()
            .zip(&self.resolved)
            .filter(|(_, r)| r.selection.enabled)
            .map(|(m, _)| m)
            .collect();
        let map = romfs::build_overlay(&refs)?;
        if map.entries.is_empty() {
            return Ok(0);
        }
        write_overlay_map(dir, &map, &refs)
    }
}

/// Runtime-readable form: one entry per line so the C reader needs no JSON library.
fn write_overlay_map(dir: &Path, map: &OverlayMap, mods: &[&LoadedMod]) -> Result<usize> {
    let mut files = Vec::new();
    let mut deleted = Vec::new();
    for (path, e) in &map.entries {
        if path
            .chars()
            .any(|c| c == '"' || c == '\\' || (c as u32) < 0x20)
        {
            return err(format!(
                "overlay path {path:?} has a quote, backslash or control character"
            ));
        }
        match e {
            Entry::File {
                mod_index, size, ..
            } => {
                let rel = format!("{OVERLAY_FILES}/{mod_index}{path}");
                let bytes = romfs::read_entry(mods, e, crate::model::MAX_FILE)?;
                let dst = dir.join(&rel);
                if let Some(p) = dst.parent() {
                    std::fs::create_dir_all(p)?;
                }
                std::fs::write(&dst, &bytes)?;
                files.push(format!(
                    "{{\"path\":\"{path}\",\"file\":\"{rel}\",\"size\":{size}}}"
                ));
            }
            Entry::Deleted { .. } => deleted.push(format!("{{\"delete\":\"{path}\"}}")),
        }
    }
    let n = files.len();
    let mut all = files;
    all.extend(deleted);
    let body = format!("{{\"format\":1,\"entries\":[\n{}\n]}}\n", all.join(",\n"));
    std::fs::create_dir_all(dir.join("mods"))?;
    std::fs::write(dir.join(OVERLAY_JSON), body)?;
    Ok(n)
}

/// One line for logs: `id@version (N hit(s))`.
pub fn summary(report: &ApplyReport) -> String {
    format!(
        "{} {} hit(s), {} miss(es), {} overlap(s)",
        build_id_hex(&report_build_id(report)),
        report.hits.len(),
        report.misses.len(),
        report.overlaps.len()
    )
}

fn report_build_id(r: &ApplyReport) -> [u8; 32] {
    crate::util::norm_build_id(&r.build_id).unwrap_or([0; 32])
}
