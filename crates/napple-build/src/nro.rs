//! NROs in a game's RomFS: modules the game maps at run time through `ldr:ro` (nn::ro::LoadModule). The runtime cannot rewrite code
//! while the game runs, so every NRO the RomFS ships is rewritten here like the ExeFS modules and written to
//! `<version dir>/nro/<module id hex>.{patched.bin,tables}`, where `runtime/src/ro.c` finds it by the module id in the NRO header.
//! The header checks mirror the ro system module's (`runtime/src/nro.c`); a file that fails them is reported, not rewritten.
use crate::error::{BuildError, Result};
use crate::json::{hex, jstr, obj};
use crate::tables::{build_tables, GateRow, Layout, TableInput};
use napple_core::infrastructure::io::read_vec;
use napple_core::infrastructure::nso::{Nso, Segment};
use napple_core::infrastructure::romfs::RomFs;
use napple_rewrite::check;
use napple_rewrite::image::ModuleImage;
use napple_rewrite::types::RewriteOptions;
use rayon::prelude::*;
use sha2::{Digest, Sha256};
use std::path::Path;

/// Largest NRO accepted (hostile input guard; real ones are a few MiB).
const MAX_NRO: u64 = 256 << 20;

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

/// The NRO as a flat module (text at 0, then rodata, data, bss): the same shape the rewriter takes from an NSO.
pub fn parse_nro(b: &[u8]) -> std::result::Result<Nso, String> {
    if b.len() < 0x80 || &b[0x10..0x14] != b"NRO0" {
        return Err("not an NRO".into());
    }
    let size = u32_at(b, 0x18) as usize;
    let [to, ts, ro, rs, dof, ds, bss] = [0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38].map(|o| u32_at(b, o));
    if size != b.len() {
        return Err(format!("header size {size:#x} differs from the file size {:#x}", b.len()));
    }
    if [ts, rs, ds, bss].iter().any(|v| v % 0x1000 != 0) {
        return Err("section sizes are not page aligned".into());
    }
    if to != 0 || to + ts != ro || ro.checked_add(rs) != Some(dof) || dof.checked_add(ds).map(|e| e as usize) != Some(size) {
        return Err("sections are not contiguous from offset 0 to the end of the file".into());
    }
    let seg = |o: u32, s: u32| Segment { mem_offset: o, data: b[o as usize..(o + s) as usize].to_vec() };
    Ok(Nso { build_id: b[0x40..0x60].try_into().unwrap(), text: seg(to, ts), rodata: seg(ro, rs), data: seg(dof, ds), bss_size: bss })
}

pub struct NroOut {
    pub path: String,
    pub module_id: [u8; 32],
    pub nro_sha256: String,
    pub file_size: usize,
    pub gates: usize,
    pub relocs: usize,
}

impl NroOut {
    pub fn json(&self) -> String {
        obj(&[
            ("path", jstr(&self.path)),
            ("module_id", jstr(&hex(&self.module_id))),
            ("nro_sha256", jstr(&self.nro_sha256)),
            ("file_size", self.file_size.to_string()),
            ("gates", self.gates.to_string()),
            ("relocs", self.relocs.to_string()),
        ], false)
    }
}

/// Rewrites one NRO into `nro_dir`.
pub fn rewrite_nro(path: &str, bytes: &[u8], nro_dir: &Path) -> Result<NroOut> {
    let bad = |m: String| BuildError::new("rewrite", format!("NRO {path}: {m}"));
    let nso = parse_nro(bytes).map_err(&bad)?;
    let id = nso.build_id_hex();
    let out = napple_rewrite::rewrite_module(&nso, &id[..32], &RewriteOptions::default()).map_err(|e| bad(e.to_string()))?;
    let orig = ModuleImage::from_nso(&nso).map_err(|e| bad(e.to_string()))?;
    let chk = check::verify(&orig, &out);
    if !out.unhandled.is_empty() || !chk.violations.is_empty() {
        let first = out.unhandled.first().map(|u| format!("{:#x}: {}", u.addr, u.reason)).or_else(|| chk.violations.first().map(|v| v.to_string())).unwrap_or_default();
        return Err(bad(format!("{} unhandled site(s), {} check violation(s) (first: {first}); the module cannot run", out.unhandled.len(), chk.violations.len())));
    }
    let l = out.layout;
    let (tables, st) = build_tables(&TableInput {
        name: path,
        image: &out.image,
        layout: Layout { text_end: l.text_end as u64, ro_off: l.ro_off as u64, data_off: l.data_off as u64, patch_off: l.patch_off as u64, key_page_off: l.key_page_off as u64 },
        gates: out.gates.iter().map(GateRow::from).collect(),
        nso: None,
        nro: Some(bytes),
    })?;
    std::fs::create_dir_all(nro_dir).map_err(|e| BuildError::io("create nro dir", e))?;
    std::fs::write(nro_dir.join(format!("{id}.patched.bin")), &out.image).map_err(|e| BuildError::io("write nro", e))?;
    std::fs::write(nro_dir.join(format!("{id}.tables")), &tables).map_err(|e| BuildError::io("write nro tables", e))?;
    Ok(NroOut { path: path.to_string(), module_id: nso.build_id, nro_sha256: hex(&Sha256::digest(bytes)), file_size: out.image.len(), gates: st.gates, relocs: st.relocs })
}

/// Every `*.nro` of the RomFS. Returns the rewritten modules and notes for files that are not usable NROs (a game may ship data with
/// that extension; a broken real NRO then fails at run time with its module id named).
pub fn rewrite_romfs_nros(romfs: &RomFs, dir: &Path, label: &str) -> Result<(Vec<NroOut>, Vec<String>)> {
    let files: Vec<_> = romfs.files.iter().filter(|f| f.path.to_ascii_lowercase().ends_with(".nro")).collect();
    let nro_dir = dir.join("nro");
    let results: Vec<std::result::Result<NroOut, String>> = files
        .par_iter()
        .map(|f| {
            if f.size > MAX_NRO {
                return Err(format!("{label}: {} is too large for an NRO ({} bytes)", f.path, f.size));
            }
            let src = romfs.open(f).map_err(|e| format!("{label}: {}: {e}", f.path))?;
            let bytes = read_vec(&*src, 0, f.size as usize).map_err(|e| format!("{label}: {}: {e}", f.path))?;
            rewrite_nro(&f.path, &bytes, &nro_dir).map_err(|e| format!("{label}: {}", e.msg))
        })
        .collect();
    let (mut outs, mut notes) = (Vec::new(), Vec::new());
    for r in results {
        match r {
            Ok(o) => outs.push(o),
            Err(n) => notes.push(n),
        }
    }
    outs.sort_by(|a, b| a.path.cmp(&b.path));
    Ok((outs, notes))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tables::fixture;

    /// The tables fixture module (MOD0, dynamic, symbols, RELA/RELR) wrapped as an NRO: text 0x1000, rodata 0x1000, data 0x2000.
    fn nro() -> Vec<u8> {
        let mut b = fixture::image()[..0x4000].to_vec();
        b[0x10..0x14].copy_from_slice(b"NRO0");
        for (o, v) in [(0x18, 0x4000u32), (0x20, 0), (0x24, 0x1000), (0x28, 0x1000), (0x2C, 0x1000), (0x30, 0x2000), (0x34, 0x2000), (0x38, 0)] {
            b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        }
        for (i, x) in b[0x40..0x60].iter_mut().enumerate() {
            *x = 0xC0 + i as u8;
        }
        b
    }

    #[test]
    fn parses_and_rejects_headers() {
        let b = nro();
        let n = parse_nro(&b).unwrap();
        assert_eq!((n.text.data.len(), n.rodata.mem_offset, n.data.mem_offset, n.data.data.len()), (0x1000, 0x1000, 0x2000, 0x2000));
        assert_eq!(n.build_id[0], 0xC0);
        let mut bad = b.clone();
        bad[0x10] = b'X';
        assert!(parse_nro(&bad).is_err());
        assert!(parse_nro(&b[..0x3000]).is_err(), "size differs from the header");
        let mut bad = b.clone();
        bad[0x24..0x28].copy_from_slice(&0x1008u32.to_le_bytes());
        assert!(parse_nro(&bad).is_err(), "unaligned text");
        let mut bad = b.clone();
        bad[0x28..0x2C].copy_from_slice(&0x2000u32.to_le_bytes());
        assert!(parse_nro(&bad).is_err(), "gap between text and rodata");
    }

    #[test]
    fn rewrites_into_the_runtime_layout() {
        let dir = std::env::temp_dir().join(format!("napple-nro-test-{}", std::process::id()));
        let b = nro();
        let o = rewrite_nro("nro/plugin.nro", &b, &dir).unwrap();
        let id = hex(&o.module_id);
        let tables = std::fs::read(dir.join(format!("{id}.tables"))).unwrap();
        let image = std::fs::read(dir.join(format!("{id}.patched.bin"))).unwrap();
        assert_eq!(&tables[..4], b"NTB1");
        assert_eq!(&tables[120..152], &b[0x40..0x60], "tables carry the module id the runtime looks the NRO up by");
        assert_eq!(&image[0x10..0x80], &b[0x10..0x80], "the header guest code reads stays as it was");
        assert_eq!(o.relocs, 6);
        assert!(o.json().contains(&format!("\"module_id\":\"{id}\"")));
        std::fs::remove_dir_all(&dir).ok();
    }
}
