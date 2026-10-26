//! Pre-flight survey of a title's code modules (`napple rewrite --survey`): what a build will meet, and the early,
//! readable failure for what it cannot do, before any slow rewrite. Reads only the NSOs and `main.npdm`.
//!
//! Idea after nxnative's `dm/games/profile_guess.py` facts and `scripts/build_ipa_from_nsp.py` pre-checks (MIT, seedds):
//! facts first, rules second. Per-game knowledge stays data-only (knownSignatures); nothing here is keyed by build id.
use crate::image::ModuleImage;
use crate::relocs;
use crate::scan::scan;
use crate::types::RewriteOptions;
use napple_core::infrastructure::npdm::parse_npdm;
use napple_core::infrastructure::nso::Nso;
use std::collections::BTreeMap;
use std::fmt::Write;

/// Bytes of patch area per rewritten site, measured: LAS 39-40, ACNH sdk 43 (docs/archive/changes/2026-10-26-claude-nx-rewriter-2026-10-26-nx.md).
/// Jump-table landing pads come on top (ACNH main: 151 B/site), so this is a floor; `--survey-exact` runs the real rewriter.
const EST_BYTES_PER_SITE: usize = 40;

#[derive(Debug, Clone, Default)]
pub struct ModuleSurvey {
    pub name: String,
    pub build_id: String,
    pub text: usize,
    pub ro: usize,
    pub data: usize,
    pub bss: u32,
    pub aarch32: bool,
    pub sites: BTreeMap<String, usize>,
    pub relocs: BTreeMap<String, usize>,
    pub est_patch: usize,
    pub exact_patch: Option<usize>,
    pub problems: Vec<String>,
}

#[derive(Debug, Clone, Default)]
pub struct Survey {
    pub label: String,
    pub npdm: Option<String>,
    pub modules: Vec<ModuleSurvey>,
    pub problems: Vec<String>,
}

fn is_module(name: &str) -> bool {
    matches!(name, "main" | "rtld" | "sdk") || name.starts_with("subsdk")
}

/// ELF32 (DT_REL=17, no DT_RELA=7) dynamic section behind MOD0: the AArch32 layout (MK8D base).
pub fn is_aarch32(nso: &Nso) -> bool {
    let Ok(img) = ModuleImage::from_nso(nso) else {
        return false;
    };
    let u32_at = |o: usize| {
        img.bytes
            .get(o..o + 4)
            .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
    };
    let Some(at) = u32_at(4).map(|v| v as usize) else {
        return false;
    };
    if img.bytes.get(at..at + 4) != Some(b"MOD0") {
        return false;
    }
    let Some(dynoff) = u32_at(at + 4).map(|v| (at as i64 + v as i32 as i64) as usize) else {
        return false;
    };
    let mut tags = Vec::new();
    for i in 0..256 {
        let (Some(t), Some(_)) = (u32_at(dynoff + 8 * i), u32_at(dynoff + 8 * i + 4)) else {
            break;
        };
        if t == 0 {
            break;
        }
        tags.push(t);
    }
    tags.contains(&17) && !tags.contains(&7)
}

fn reloc_name(kind: u32) -> String {
    match kind {
        257 => "ABS64".into(),
        1025 => "GLOB_DAT".into(),
        1026 => "JUMP_SLOT".into(),
        1027 => "RELATIVE".into(),
        k => format!("type{k}"),
    }
}

pub fn survey_module(name: &str, bytes: &[u8], exact: bool) -> ModuleSurvey {
    let mut m = ModuleSurvey {
        name: name.into(),
        ..Default::default()
    };
    let nso = match Nso::parse(bytes) {
        Ok(n) => n,
        Err(e) => {
            m.problems.push(format!("not a readable NSO: {e}"));
            return m;
        }
    };
    m.build_id = nso.build_id_hex();
    (m.text, m.ro, m.data, m.bss) = (
        nso.text.data.len(),
        nso.rodata.data.len(),
        nso.data.data.len(),
        nso.bss_size,
    );
    if is_aarch32(&nso) {
        m.aarch32 = true;
        m.problems.push("AArch32 module: no AArch64 rewrite, built as a plain image + NTB2 tables (MK8D base game); the A64 patch-size figures do not apply".into());
        return m;
    }
    let img = match ModuleImage::from_nso(&nso) {
        Ok(i) => i,
        Err(e) => {
            m.problems.push(format!("unsupported layout: {e}"));
            return m;
        }
    };
    let found = scan(&img);
    for k in found.sites.values() {
        let n = format!("{k:?}");
        *m.sites
            .entry(n.split('(').next().unwrap_or("?").to_string())
            .or_default() += 1;
    }
    if !found.undecodable.is_empty() {
        m.problems.push(format!(
            "{} undecodable word(s) with a register-18 field (left untouched)",
            found.undecodable.len()
        ));
    }
    if !found.other_sysregs.is_empty() {
        m.problems.push(format!(
            "{} system-register access(es) that are neither native nor rewritten",
            found.other_sysregs.len()
        ));
    }
    match relocs::collect(
        &img.bytes,
        &img.dynamic_tags(),
        img.layout.data_off as u64,
        img.layout.image_len as u64,
    ) {
        Ok(rs) => {
            for r in &rs {
                *m.relocs.entry(reloc_name(r.kind)).or_default() += 1;
            }
            if let Some(k) = rs
                .iter()
                .map(|r| r.kind)
                .find(|k| ![257, 1025, 1026, 1027].contains(k))
            {
                m.problems.push(format!(
                    "relocation kind {k} (type{k}) is not handled by the loader"
                ));
            }
        }
        Err(e) => m.problems.push(format!("relocation tables: {e}")),
    }
    m.est_patch = found.sites.len() * EST_BYTES_PER_SITE;
    if exact {
        match crate::rewrite_module(&nso, &m.build_id[..32], &RewriteOptions::default()) {
            Ok(out) => {
                m.exact_patch = Some(out.patch().len());
                for u in out.unhandled.iter().take(5) {
                    m.problems.push(format!("UNHANDLED {u}"));
                }
                if out.unhandled.len() > 5 {
                    m.problems.push(format!(
                        "... and {} more unhandled sites",
                        out.unhandled.len() - 5
                    ));
                }
            }
            Err(e) => m.problems.push(format!("rewrite fails: {e}")),
        }
    }
    m
}

pub fn survey_program(label: &str, exefs: &[(String, Vec<u8>)], exact: bool) -> Survey {
    use rayon::prelude::*;
    let mut s = Survey {
        label: label.into(),
        ..Default::default()
    };
    match exefs.iter().find(|(n, _)| n == "main.npdm") {
        Some((_, d)) => match parse_npdm(d) {
            Ok(n) => {
                s.npdm = Some(format!(
                    "{} product '{}' title {:016X} {} address_space {} sys_resource {:#x} stack {:#x} prio {} core {} acid_flags {:#x}{} svcs {} services {}",
                    n.name,
                    n.product_code,
                    n.title_id,
                    if n.is_64bit { "64-bit" } else { "32-bit" },
                    n.address_space,
                    n.system_resource_size,
                    n.main_thread_stack_size,
                    n.main_thread_priority,
                    n.main_thread_core,
                    n.acid_flags,
                    if n.acid_production { " production" } else { "" },
                    n.kernel.allowed_svcs.len(),
                    n.services.len()
                ));
                if !n.is_64bit {
                    s.problems.push(
                        "NPDM says 32-bit: this program runs on the AArch32 path only".into(),
                    );
                }
            }
            Err(e) => s.problems.push(format!("main.npdm unreadable: {e}")),
        },
        None => s.problems.push("no main.npdm in the ExeFS".into()),
    }
    let mods: Vec<&(String, Vec<u8>)> = exefs.iter().filter(|(n, _)| is_module(n)).collect();
    if !mods.iter().any(|(n, _)| n == "main") {
        s.problems.push("no `main` module in the ExeFS".into());
    }
    s.modules = mods
        .par_iter()
        .map(|(n, b)| survey_module(n, b, exact))
        .collect();
    s.modules.sort_by(|a, b| a.name.cmp(&b.name));
    let mixed = s.modules.iter().any(|m| m.aarch32) && s.modules.iter().any(|m| !m.aarch32);
    if mixed {
        s.problems.push("modules mix AArch32 and AArch64".into());
    }
    s
}

fn mb(n: usize) -> String {
    format!("{:.2} MB", n as f64 / 1e6)
}

impl Survey {
    pub fn render(&self) -> String {
        let mut o = String::new();
        let _ = writeln!(o, "== {}", self.label);
        if let Some(n) = &self.npdm {
            let _ = writeln!(o, "npdm: {n}");
        }
        for m in &self.modules {
            let _ = writeln!(
                o,
                "{:<8} {} build {}  text {}  ro {}  data {}  bss {}",
                m.name,
                if m.aarch32 { "A32" } else { "A64" },
                m.build_id.get(..16).unwrap_or(&m.build_id),
                mb(m.text),
                mb(m.ro),
                mb(m.data),
                mb(m.bss as usize)
            );
            if !m.aarch32 {
                let kv = |t: &BTreeMap<String, usize>| {
                    t.iter()
                        .map(|(k, v)| format!("{k}={v}"))
                        .collect::<Vec<_>>()
                        .join(" ")
                };
                let _ = writeln!(o, "         sites: {}", kv(&m.sites));
                let _ = writeln!(o, "         relocs: {}", kv(&m.relocs));
                match m.exact_patch {
                    Some(p) => {
                        let _ = writeln!(
                            o,
                            "         patch area: {} (exact; estimate was {})",
                            mb(p),
                            mb(m.est_patch)
                        );
                    }
                    None => {
                        let _ = writeln!(
                            o,
                            "         patch area: ~{} (estimate, excludes jump pads)",
                            mb(m.est_patch)
                        );
                    }
                }
            }
            for p in &m.problems {
                let _ = writeln!(o, "         ! {p}");
            }
        }
        for p in &self.problems {
            let _ = writeln!(o, "! {p}");
        }
        o
    }

    /// Problems that make a rewrite pointless (AArch32 modules are routed elsewhere and are not failures).
    pub fn blocking(&self) -> Vec<String> {
        let mut v = self.problems.clone();
        for m in &self.modules {
            v.extend(
                m.problems
                    .iter()
                    .filter(|p| !p.starts_with("AArch32 module"))
                    .map(|p| format!("{}: {p}", m.name)),
            );
        }
        v.retain(|p| !p.starts_with("NPDM says 32-bit") || !self.modules.iter().all(|m| m.aarch32));
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::nso::Segment;

    /// Uncompressed NSO0 container for a synthetic module.
    fn nso_file(n: &Nso) -> Vec<u8> {
        let mut f = vec![0u8; 0x100];
        f[..4].copy_from_slice(b"NSO0");
        f[0x3C..0x40].copy_from_slice(&n.bss_size.to_le_bytes());
        f[0x40..0x60].copy_from_slice(&n.build_id);
        for (i, sg) in [&n.text, &n.rodata, &n.data].iter().enumerate() {
            let off = f.len() as u32;
            f[0x10 + i * 0x10..0x14 + i * 0x10].copy_from_slice(&off.to_le_bytes());
            f[0x14 + i * 0x10..0x18 + i * 0x10].copy_from_slice(&sg.mem_offset.to_le_bytes());
            f[0x18 + i * 0x10..0x1C + i * 0x10]
                .copy_from_slice(&(sg.data.len() as u32).to_le_bytes());
            f[0x60 + i * 4..0x64 + i * 4].copy_from_slice(&(sg.data.len() as u32).to_le_bytes());
            f.extend_from_slice(&sg.data);
        }
        f
    }

    #[test]
    fn survey_counts_sites_and_flags_garbage() {
        // mov x18,x0 ; nop
        let text: Vec<u8> = [0xAA00_03F2u32, 0xD503_201F]
            .iter()
            .flat_map(|w| w.to_le_bytes())
            .collect();
        let n = Nso {
            build_id: [0x11; 32],
            text: Segment {
                mem_offset: 0,
                data: text,
            },
            rodata: Segment {
                mem_offset: 0x1000,
                data: vec![0; 16],
            },
            data: Segment {
                mem_offset: 0x2000,
                data: vec![0; 16],
            },
            bss_size: 0,
        };
        let bytes = nso_file(&n);
        let m = survey_module("main", &bytes, true);
        assert_eq!(m.sites.get("X18"), Some(&1), "{m:?}");
        assert!(m.exact_patch.is_some());
        assert!(
            survey_module("main", b"garbage", false).problems[0].contains("not a readable NSO")
        );
    }

    #[test]
    fn missing_npdm_and_main_fail_early() {
        let s = survey_program("t", &[], false);
        assert_eq!(s.problems.len(), 2);
        assert!(s.render().contains("no main.npdm"));
    }
}
