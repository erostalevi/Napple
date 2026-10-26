//! Decomp stage: for each selected version (base + chosen updates) load every NSO and scan rewrite sites.
use super::ingest::Ingested;
use super::package::{BuildPlan, version_label};
use crate::analysis::a64::{self, SiteKind};
use crate::domain::error::Result;
use crate::domain::keys::KeySet;
use crate::infrastructure::nso::Nso;

pub struct ModuleReport {
    pub name: String,
    pub build_id: String,
    pub text_bytes: usize,
    pub sites: Vec<(SiteKind, usize)>,
}

pub struct VersionReport {
    pub label: String,
    pub title_id: u64,
    pub modules: Vec<ModuleReport>,
    pub romfs_files: Option<usize>,
    pub romfs_note: Option<String>,
}

const NSO_NAMES: [&str; 3] = ["main", "rtld", "sdk"];

fn is_nso(name: &str) -> bool {
    NSO_NAMES.contains(&name) || name.starts_with("subsdk")
}

pub fn analyze_program(label: String, title_id: u64, prog: &Ingested) -> Result<VersionReport> {
    let mut modules = Vec::new();
    for (name, data) in prog.exefs.iter().filter(|(n, _)| is_nso(n)) {
        let nso = Nso::parse(data)?;
        let rep = a64::scan(&nso.text.data, nso.text.mem_offset);
        modules.push(ModuleReport {
            name: name.clone(),
            build_id: nso.build_id_hex(),
            text_bytes: nso.text.data.len(),
            sites: rep.sites.iter().map(|(k, v)| (*k, v.len())).collect(),
        });
    }
    Ok(VersionReport {
        label,
        title_id,
        modules,
        romfs_files: prog.romfs.as_ref().map(|r| r.files.len()),
        romfs_note: prog.romfs_note.clone(),
    })
}

/// Analyzes the base and each selected update. `updates` are indices into `plan.packs`.
pub fn run(plan: &BuildPlan, updates: &[usize], keys: &KeySet) -> Result<Vec<VersionReport>> {
    let mut out = Vec::new();
    let mut targets: Vec<usize> = plan.base.into_iter().collect();
    targets.extend_from_slice(updates);
    for i in targets {
        let p = &plan.packs[i];
        let prog = p.program(keys)?;
        out.push(analyze_program(
            version_label(p.meta.version),
            p.meta.title_id,
            &prog,
        )?);
    }
    Ok(out)
}

impl VersionReport {
    /// JSON report: addresses/counts/ids only, never game bytes.
    pub fn to_json(&self) -> String {
        let mods: Vec<String> = self
            .modules
            .iter()
            .map(|m| {
                let sites: Vec<String> = m
                    .sites
                    .iter()
                    .map(|(k, n)| format!("\"{k:?}\":{n}"))
                    .collect();
                format!(
                    "{{\"name\":\"{}\",\"build_id\":\"{}\",\"text_bytes\":{},\"sites\":{{{}}}}}",
                    m.name,
                    m.build_id,
                    m.text_bytes,
                    sites.join(",")
                )
            })
            .collect();
        format!(
            "{{\"title_id\":\"{:016X}\",\"version\":\"{}\",\"modules\":[{}]}}",
            self.title_id,
            self.label,
            mods.join(",")
        )
    }
}
