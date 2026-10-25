//! Shared system fonts (what `pl:u` serves) and the firmware version, extracted from the user's firmware.
//!
//! The fonts live in the system data archives 0100000000000810..0814 as `.bfttf` files. They are found by content:
//! a BFTTF is `u32 magic^K, u32 size^K (big endian TTF size)`, then the TTF XORed with the 4-byte key K
//! (K = 0x06186249, applied as little-endian words). Output layout (documented in docs/runtime-modules.md):
//! `system/fonts/<name>.bfttf` (as stored), `system/fonts/<name>.ttf` (decoded) and `system/fonts/fonts.json`.
//! `napple build --dump-fonts <firmware> -k <prod.keys> -o <dir>` writes those files straight into `<dir>` (what `runtime/src/services/pl.c` reads).
use crate::error::{BuildError, Result};
use crate::firmware::Firmware;
use crate::json::{arr, hex, jstr, obj};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::read_vec;
use sha2::{Digest, Sha256};
use std::path::Path;

const XOR_KEY: u32 = 0x0618_6249;
const MAGIC: u32 = 0x1802_9A7F;
const MAX_FONT: u64 = 64 << 20;
const SYSTEM_VERSION_TITLE: u64 = 0x0100_0000_0000_0809;

/// `nn::pl::SharedFontType` ids; `None` for extra files that have no official type.
const TYPES: [(&str, Option<u32>, &str); 7] = [
    ("_std_", Some(0), "JapanUsEuropeStandard"),
    ("_org_zh-cn_", Some(1), "ChineseSimplified"),
    ("_ext_zh-cn_", Some(2), "ExtendedChineseSimplified"),
    ("_zh-tw_", Some(3), "ChineseTraditional"),
    ("_ko_", Some(4), "Korean"),
    ("nintendo_ext2_", None, "NintendoExtended2"),
    ("nintendo_ext_", Some(5), "NintendoExtended"),
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Font {
    pub file: String,
    pub type_name: &'static str,
    pub type_id: Option<u32>,
    pub title_id: u64,
    pub raw: Vec<u8>,
    pub ttf: Vec<u8>,
}

pub fn decode_bfttf(raw: &[u8]) -> Option<Vec<u8>> {
    if raw.len() < 12 || raw.len() as u64 > MAX_FONT {
        return None;
    }
    let w = |o: usize| u32::from_le_bytes(raw[o..o + 4].try_into().unwrap()) ^ XOR_KEY;
    if w(0) != MAGIC || u32::from_be_bytes(w(4).to_le_bytes()) as usize != raw.len() - 8 {
        return None;
    }
    let key = XOR_KEY.to_le_bytes();
    let ttf: Vec<u8> = raw[8..].iter().enumerate().map(|(i, b)| b ^ key[i % 4]).collect();
    matches!(&ttf[..4], [0, 1, 0, 0] | b"OTTO" | b"true" | b"ttcf").then_some(ttf)
}

#[cfg(test)]
pub(crate) fn encode_bfttf(ttf: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend((MAGIC ^ XOR_KEY).to_le_bytes());
    out.extend([0u8; 4]);
    let k = XOR_KEY.to_le_bytes();
    let size_word = u32::from_le_bytes((ttf.len() as u32).to_be_bytes()) ^ XOR_KEY;
    out[4..8].copy_from_slice(&size_word.to_le_bytes());
    out.extend(ttf.iter().enumerate().map(|(i, b)| b ^ k[i % 4]));
    out
}

fn classify(name: &str) -> (&'static str, Option<u32>) {
    TYPES.iter().find(|(pat, _, _)| name.contains(pat)).map(|(_, id, n)| (*n, *id)).unwrap_or(("Unknown", None))
}

/// Finds every shared font in the firmware's system data archives (title ids 0x...0800-0x...081F).
pub fn find_fonts(fw: &Firmware, keys: &KeySet) -> Result<Vec<Font>> {
    let mut fonts = Vec::new();
    for t in fw.titles(keys)? {
        let id = t.meta.title_id;
        if id >> 16 != 0x0100_0000_0000 || !(0x800..0x820).contains(&(id & 0xFFFF)) {
            continue;
        }
        let Some(romfs) = fw.data_romfs(&t, keys)? else { continue };
        for f in romfs.files.iter().filter(|f| f.path.ends_with(".bfttf") && f.size <= MAX_FONT && !f.path.contains('/')) {
            let raw = read_vec(&*romfs.open(f)?, 0, f.size as usize)?;
            let Some(ttf) = decode_bfttf(&raw) else { continue };
            let (type_name, type_id) = classify(&f.path);
            fonts.push(Font { file: f.path.clone(), type_name, type_id, title_id: id, raw, ttf });
        }
    }
    fonts.sort_by_key(|f| (f.type_id.unwrap_or(99), f.file.clone()));
    fonts.dedup_by(|a, b| a.file == b.file);
    Ok(fonts)
}

/// Firmware version string ("22.1.0") from the SystemVersion archive.
pub fn firmware_version(fw: &Firmware, keys: &KeySet) -> Result<Option<String>> {
    for t in fw.titles(keys)?.into_iter().filter(|t| t.meta.title_id == SYSTEM_VERSION_TITLE) {
        let Some(romfs) = fw.data_romfs(&t, keys)? else { continue };
        if let Some(f) = romfs.files.iter().find(|f| f.path == "file" && f.size >= 0x80) {
            let d = read_vec(&*romfs.open(f)?, 0, f.size as usize)?;
            let s = &d[0x68..0x80];
            let end = s.iter().position(|&c| c == 0).unwrap_or(s.len());
            return Ok(Some(String::from_utf8_lossy(&s[..end]).into_owned()));
        }
    }
    Ok(None)
}

/// Writes the font files and `fonts.json` into `fd` and returns the index JSON.
pub fn write_fonts(fonts: &[Font], firmware_version: Option<&str>, fd: &Path) -> Result<String> {
    std::fs::create_dir_all(fd).map_err(|e| BuildError::io("create fonts dir", e))?;
    let mut items = Vec::new();
    for f in fonts {
        if f.file.contains(['/', '\\']) || f.file.is_empty() || f.file.starts_with('.') {
            return Err(BuildError::new("firmware", "unsafe font file name"));
        }
        let stem = f.file.trim_end_matches(".bfttf");
        std::fs::write(fd.join(&f.file), &f.raw).map_err(|e| BuildError::io("write font", e))?;
        std::fs::write(fd.join(format!("{stem}.ttf")), &f.ttf).map_err(|e| BuildError::io("write font", e))?;
        items.push(obj(
            &[
                ("type", f.type_id.map(|t| t.to_string()).unwrap_or_else(|| "null".into())),
                ("name", jstr(f.type_name)),
                ("bfttf", jstr(&f.file)),
                ("ttf", jstr(&format!("{stem}.ttf"))),
                ("bfttf_size", f.raw.len().to_string()),
                ("ttf_size", f.ttf.len().to_string()),
                ("sha256", jstr(&hex(&Sha256::digest(&f.raw)))),
                ("title_id", jstr(&format!("{:016X}", f.title_id))),
            ],
            false,
        ));
    }
    let index = obj(
        &[
            ("format", "1".into()),
            ("firmware_version", firmware_version.map(jstr).unwrap_or_else(|| "null".into())),
            ("bfttf_xor_key", jstr("0x06186249")),
            ("note", jstr("bfttf = u32 magic^K, u32 BE size^K, TTF xor K; ttf = decoded; type = nn::pl::SharedFontType")),
            ("fonts", arr(&items)),
        ],
        true,
    ) + "\n";
    std::fs::write(fd.join("fonts.json"), &index).map_err(|e| BuildError::io("write fonts.json", e))?;
    Ok(index)
}

/// System data archive ids written for the runtime: every data title 0100000000000800-0100000000000FFF that has a RomFS (fonts, the
/// built-in system data games ask `OpenDataStorageByDataId` for, ...). Archives larger than `MAX_ARCHIVE` are skipped.
const MAX_ARCHIVE: u64 = 64 << 20;

/// Writes each system data archive's RomFS image as `<dir>/<title id, 16 lowercase hex>.bin` (what runtime fs.c serves for
/// OpenDataStorageByDataId). Returns the ids written.
pub fn write_system_archives(fw: &Firmware, keys: &KeySet, dir: &Path) -> Result<Vec<u64>> {
    let mut done = Vec::new();
    for t in fw.titles(keys)? {
        let id = t.meta.title_id;
        if id >> 16 != 0x0100_0000_0000 || !(0x800..0x1000).contains(&(id & 0xFFFF)) || done.contains(&id) {
            continue;
        }
        let Some(romfs) = fw.data_romfs(&t, keys)? else { continue };
        let img = romfs.image();
        if img.size() > MAX_ARCHIVE {
            continue;
        }
        std::fs::create_dir_all(dir).map_err(|e| BuildError::io("create sysdata dir", e))?;
        let data = read_vec(&**img, 0, img.size() as usize)?;
        let fin = dir.join(format!("{id:016x}.bin"));
        let tmp = dir.join(format!("{id:016x}.bin.tmp"));
        std::fs::write(&tmp, &data).map_err(|e| BuildError::io("write sysdata", e))?;
        std::fs::rename(&tmp, &fin).map_err(|e| BuildError::io("write sysdata", e))?;
        done.push(id);
    }
    Ok(done)
}

/// `napple build --dump-fonts <firmware zip|dir> -k <prod.keys> -o <dir>`
pub fn run_dump_fonts(args: &[String]) -> std::result::Result<(), String> {
    run_dump_fonts_inner(args).map_err(|e| e.to_string())
}

fn run_dump_fonts_inner(args: &[String]) -> Result<()> {
    let (mut fw, mut keys, mut out) = (None, None, None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().map(std::path::PathBuf::from).ok_or_else(|| BuildError::usage(format!("{a} needs a value")));
        match a.as_str() {
            "--dump-fonts" => fw = Some(val()?),
            "-k" => keys = Some(val()?),
            "-o" => out = Some(val()?),
            x => return Err(BuildError::usage(format!("unknown argument {x}\nusage: napple build --dump-fonts <firmware zip|dir> -k <prod.keys> -o <dir>"))),
        }
    }
    let (Some(fw), Some(keys), Some(out)) = (fw, keys, out) else {
        return Err(BuildError::usage("usage: napple build --dump-fonts <firmware zip|dir> -k <prod.keys> -o <dir>"));
    };
    let keys = crate::util::load_keys(&keys)?;
    let fwi = Firmware::open(&fw)?;
    let fonts = find_fonts(&fwi, &keys)?;
    if fonts.is_empty() {
        return Err(BuildError::new("firmware", "no shared fonts found in the firmware (wrong firmware, or keys too old for it)"));
    }
    write_fonts(&fonts, firmware_version(&fwi, &keys)?.as_deref(), &out)?;
    println!("fonts: {} files ({} official shared font types of 6) written to {}", fonts.len(), official_count(&fonts), out.display());
    Ok(())
}

/// Number of official shared font types (0..=5) present.
pub fn official_count(fonts: &[Font]) -> usize {
    let mut ids: Vec<u32> = fonts.iter().filter_map(|f| f.type_id).collect();
    ids.sort_unstable();
    ids.dedup();
    ids.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ttf(n: usize) -> Vec<u8> {
        let mut v = vec![0, 1, 0, 0];
        v.extend((0..n).map(|i| (i * 7) as u8));
        v
    }

    #[test]
    fn bfttf_roundtrip_and_rejects() {
        for n in [0usize, 1, 5, 4096, 4099] {
            let t = ttf(n);
            let raw = encode_bfttf(&t);
            assert_eq!(decode_bfttf(&raw).unwrap(), t, "len {n}");
        }
        let mut raw = encode_bfttf(&ttf(100));
        raw[0] ^= 1;
        assert!(decode_bfttf(&raw).is_none()); // magic
        let mut raw = encode_bfttf(&ttf(100));
        raw.pop();
        assert!(decode_bfttf(&raw).is_none()); // size mismatch
        assert!(decode_bfttf(&[0u8; 4]).is_none());
        let mut notttf = encode_bfttf(&ttf(100));
        notttf[8] ^= 0xFF;
        assert!(decode_bfttf(&notttf).is_none()); // not a font once decoded
    }

    #[test]
    fn classifies_by_name() {
        assert_eq!(classify("nintendo_udsg-r_std_003.bfttf"), ("JapanUsEuropeStandard", Some(0)));
        assert_eq!(classify("nintendo_udsg-r_org_zh-cn_003.bfttf").1, Some(1));
        assert_eq!(classify("nintendo_udsg-r_ext_zh-cn_003.bfttf").1, Some(2));
        assert_eq!(classify("nintendo_udjxh-db_zh-tw_003.bfttf").1, Some(3));
        assert_eq!(classify("nintendo_udsg-r_ko_003.bfttf").1, Some(4));
        assert_eq!(classify("nintendo_ext_003.bfttf").1, Some(5));
        assert_eq!(classify("nintendo_ext2_003.bfttf"), ("NintendoExtended2", None));
        assert_eq!(classify("weird.bfttf").0, "Unknown");
    }

    #[test]
    fn writes_index_and_files() {
        let dir = std::env::temp_dir().join(format!("napple-fonts-{}", std::process::id()));
        let t = ttf(64);
        let f = Font { file: "nintendo_udsg-r_std_003.bfttf".into(), type_name: "JapanUsEuropeStandard", type_id: Some(0), title_id: 0x0100000000000811, raw: encode_bfttf(&t), ttf: t.clone() };
        let idx = write_fonts(&[f.clone()], Some("22.1.0"), &dir).unwrap();
        assert!(idx.contains("\"firmware_version\":\"22.1.0\"") && idx.contains("JapanUsEuropeStandard"));
        assert_eq!(std::fs::read(dir.join("nintendo_udsg-r_std_003.ttf")).unwrap(), t);
        assert_eq!(official_count(&[f.clone()]), 1);
        let mut bad = f;
        bad.file = "../x.bfttf".into();
        assert!(write_fonts(&[bad], None, &dir).is_err());
        let _ = std::fs::remove_dir_all(dir);
    }
}
