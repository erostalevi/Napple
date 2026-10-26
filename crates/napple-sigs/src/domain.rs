//! Domain types for shared signatures. Pure rules: no I/O, no process execution.
//!
//! A signature never holds game bytes beyond a short masked instruction pattern
//! ([`MAX_PATTERN_BYTES`]). Relocatable operands are wildcarded so the pattern is
//! stable across builds and carries as little of the original code as possible.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fmt;

pub const SCHEMA_VERSION: u32 = 1;
/// Hard cap on a stored pattern. 16 A64 instructions is enough to identify a function in a module.
pub const MAX_PATTERN_BYTES: usize = 64;
/// Smallest pattern the resolver will generate (3 instructions).
pub const MIN_PATTERN_BYTES: usize = 12;
/// A pattern must pin at least this many bytes, otherwise it is not identifying.
pub const MIN_FIXED_BYTES: usize = 8;
/// Pattern-hash length in hex characters (128 bits).
pub const HASH_HEX_LEN: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum SigKind {
    Function,
    Pointer,
    Struct,
    PatchSite,
    ServiceHook,
}

impl SigKind {
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s {
            "function" => Self::Function,
            "pointer" => Self::Pointer,
            "struct" => Self::Struct,
            "patch-site" => Self::PatchSite,
            "service-hook" => Self::ServiceHook,
            _ => return None,
        })
    }
    /// Kinds whose pattern is executable A64 code (4-byte aligned search).
    pub fn is_code(self) -> bool {
        matches!(self, Self::Function | Self::PatchSite | Self::ServiceHook)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Provenance {
    Local,
    Rea,
    Community,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Confidence {
    Low,
    Medium,
    High,
}

/// What the rewriter/runtime does with a matched item.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ResolutionAction {
    /// Annotate only (name, layout); no behaviour change.
    Annotate,
    /// Redirect to a host (HLE) function named in `params.target`.
    Hook,
    /// Replace with a fixed return, `params.value` (short literal).
    Stub,
    /// Apply a named rewriter recipe, `params.recipe`.
    Rewrite,
    /// Leave untouched on purpose.
    Skip,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Resolution {
    pub action: ResolutionAction,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub params: BTreeMap<String, String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StructField {
    pub name: String,
    pub offset: u32,
    pub size: u32,
    /// Short type hint, e.g. `u32`, `ptr`, `f32`.
    #[serde(rename = "type")]
    pub ty: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StructLayout {
    pub size: u32,
    pub fields: Vec<StructField>,
}

/// Masked byte pattern. `bytes` already has the wildcarded bits zeroed (`bytes & mask == bytes`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pattern {
    pub bytes: Vec<u8>,
    pub mask: Vec<u8>,
}

impl Pattern {
    pub fn new(bytes: Vec<u8>, mask: Vec<u8>) -> Result<Self, String> {
        if bytes.len() != mask.len() {
            return Err("pattern and mask differ in length".into());
        }
        if bytes.is_empty() {
            return Err("empty pattern".into());
        }
        let bytes = bytes.iter().zip(&mask).map(|(b, m)| b & m).collect();
        Ok(Self { bytes, mask })
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Number of bytes whose bits are all pinned.
    pub fn fixed_bytes(&self) -> usize {
        self.mask.iter().filter(|m| **m == 0xFF).count()
    }

    /// Fraction-free count of pinned bits.
    pub fn fixed_bits(&self) -> u32 {
        self.mask.iter().map(|m| m.count_ones()).sum()
    }

    /// Stable hash: sha256("napple-sig/1" 0 bytes mask) truncated to 128 bits, lower hex.
    pub fn hash(&self) -> String {
        let mut h = Sha256::new();
        h.update(b"napple-sig/1\0");
        h.update((self.bytes.len() as u32).to_le_bytes());
        h.update(&self.bytes);
        h.update(&self.mask);
        hex(&h.finalize()[..HASH_HEX_LEN / 2])
    }

    pub fn matches_at(&self, hay: &[u8], at: usize) -> bool {
        let Some(win) = hay.get(at..at.saturating_add(self.len())) else {
            return false;
        };
        win.iter()
            .zip(&self.bytes)
            .zip(&self.mask)
            .all(|((h, b), m)| h & m == *b)
    }

    /// All match offsets in `hay` (`step` 4 for code, 1 otherwise), capped at `limit`.
    pub fn find_all(&self, hay: &[u8], step: usize, limit: usize) -> Vec<usize> {
        let mut out = Vec::new();
        if hay.len() < self.len() {
            return out;
        }
        // First fully-pinned byte is the cheap prefilter.
        let pivot = self.mask.iter().position(|m| *m == 0xFF);
        let mut i = 0;
        while i + self.len() <= hay.len() {
            let ok = match pivot {
                Some(p) => hay[i + p] == self.bytes[p] && self.matches_at(hay, i),
                None => self.matches_at(hay, i),
            };
            if ok {
                out.push(i);
                if out.len() >= limit {
                    break;
                }
            }
            i += step;
        }
        out
    }

    /// Build a masked A64 pattern from `code` (little-endian instruction words),
    /// wildcarding relocatable operands. Length is truncated to whole words.
    pub fn from_a64(code: &[u8]) -> Result<Self, String> {
        let words = code.len() / 4;
        let mut bytes = Vec::with_capacity(words * 4);
        let mut mask = Vec::with_capacity(words * 4);
        let mut prev: Option<u32> = None;
        for c in code.as_chunks::<4>().0 {
            let w = u32::from_le_bytes([c[0], c[1], c[2], c[3]]);
            let m = a64_fixed_mask(w, prev);
            bytes.extend_from_slice(&(w & m).to_le_bytes());
            mask.extend_from_slice(&m.to_le_bytes());
            prev = Some(w);
        }
        Self::new(bytes, mask)
    }

    pub fn bytes_hex(&self) -> String {
        hex(&self.bytes)
    }

    pub fn mask_hex(&self) -> String {
        hex(&self.mask)
    }
}

/// Bits of an A64 word that stay fixed across builds. Operands that the linker or the
/// code layout rewrites (branch targets, PC-relative offsets, page-relative immediates
/// after ADRP) are wildcarded.
pub fn a64_fixed_mask(w: u32, prev: Option<u32>) -> u32 {
    let rd_rn = |x: u32| ((x >> 5) & 0x1F, x & 0x1F);
    // B / BL imm26
    if w & 0x7C00_0000 == 0x1400_0000 {
        return 0xFC00_0000;
    }
    // ADR / ADRP
    if w & 0x1F00_0000 == 0x1000_0000 {
        return 0x9F00_001F;
    }
    // LDR (literal), incl. SIMD and PRFM literal
    if w & 0x3B00_0000 == 0x1800_0000 {
        return 0xFF00_001F;
    }
    // B.cond / BC.cond
    if w & 0xFF00_0010 == 0x5400_0000 {
        return 0xFF00_001F;
    }
    // CBZ / CBNZ
    if w & 0x7E00_0000 == 0x3400_0000 {
        return 0xFF00_001F;
    }
    // TBZ / TBNZ
    if w & 0x7E00_0000 == 0x3600_0000 {
        return 0xFFF8_001F;
    }
    // imm12 low-half of a symbol address: ADD (imm) / LDR/STR (unsigned imm) based on the ADRP result.
    if let Some(p) = prev
        && p & 0x9F00_0000 == 0x9000_0000
    {
        let adrp_rd = p & 0x1F;
        let (rn, _) = rd_rn(w);
        let add_imm = w & 0x7F80_0000 == 0x1100_0000;
        let ldst_uimm = w & 0x3B00_0000 == 0x3900_0000;
        if rn == adrp_rd && (add_imm || ldst_uimm) {
            return 0xFFC0_03FF;
        }
    }
    0xFFFF_FFFF
}

pub fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{x:02x}"));
    }
    s
}

pub fn unhex(s: &str) -> Result<Vec<u8>, String> {
    if !s.len().is_multiple_of(2) || !s.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err("not an even-length hex string".into());
    }
    Ok((0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect())
}

/// Entry as stored in JSON (`pattern`/`mask` as hex).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Signature {
    pub kind: SigKind,
    /// Module name inside the title, e.g. `main`, `sdk`, `subsdk0`.
    pub module: String,
    /// Inclusive start / exclusive end, relative to the module image base.
    pub offset_start: u64,
    pub offset_end: u64,
    /// Masked pattern bytes (hex) and mask (hex), same length.
    pub pattern: String,
    pub mask: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout: Option<StructLayout>,
    pub resolution: Resolution,
    pub provenance: Provenance,
    pub confidence: Confidence,
}

impl Signature {
    pub fn pattern(&self) -> Result<Pattern, String> {
        Pattern::new(unhex(&self.pattern)?, unhex(&self.mask)?)
    }
}

/// One file: all signatures of one binary build of one title.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SignatureFile {
    pub schema_version: u32,
    pub title_id: String,
    pub build_id: String,
    /// Keyed by the pattern hash (see [`Pattern::hash`]).
    pub signatures: BTreeMap<String, Signature>,
}

impl SignatureFile {
    pub fn new(title: &TitleId, build: &BuildId) -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            title_id: title.0.clone(),
            build_id: build.0.clone(),
            signatures: BTreeMap::new(),
        }
    }

    pub fn to_json(&self) -> String {
        let mut s = serde_json::to_string_pretty(self).expect("serializable");
        s.push('\n');
        s
    }
}

/// 16 upper-case hex digits.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TitleId(pub String);

/// Build id (NSO header) as 16..64 lower-case hex digits.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct BuildId(pub String);

impl TitleId {
    pub fn parse(s: &str) -> Result<Self, String> {
        let t = s.trim();
        if t.len() == 16 && t.bytes().all(|c| c.is_ascii_hexdigit()) {
            Ok(Self(t.to_ascii_uppercase()))
        } else {
            Err(format!("invalid title id (need 16 hex digits): {t:?}"))
        }
    }
}

impl BuildId {
    pub fn parse(s: &str) -> Result<Self, String> {
        let t = s.trim();
        if (16..=64).contains(&t.len())
            && t.len().is_multiple_of(2)
            && t.bytes().all(|c| c.is_ascii_hexdigit())
        {
            Ok(Self(t.to_ascii_lowercase()))
        } else {
            Err(format!("invalid build id (need 16..64 hex digits): {t:?}"))
        }
    }
}

impl fmt::Display for TitleId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl fmt::Display for BuildId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn le(w: u32) -> [u8; 4] {
        w.to_le_bytes()
    }

    #[test]
    fn bl_target_is_wildcarded_so_hash_is_stable() {
        let a: Vec<u8> = [le(0xA9BF7BFD), le(0x9400_0010), le(0xD65F03C0)].concat();
        let b: Vec<u8> = [le(0xA9BF7BFD), le(0x9400_0FF0), le(0xD65F03C0)].concat();
        let (pa, pb) = (
            Pattern::from_a64(&a).unwrap(),
            Pattern::from_a64(&b).unwrap(),
        );
        assert_eq!(pa.hash(), pb.hash());
        assert_eq!(pa.fixed_bytes(), 8); // 2 full words; BL keeps only its opcode bits
    }

    #[test]
    fn adrp_add_pair_masks_page_and_imm12() {
        let adrp = 0x9000_0008u32 | (0x123 << 5); // adrp x8, <page>
        let add = 0x9100_0108u32 | (0x7AB << 10); // add x8, x8, #imm12
        let p = Pattern::from_a64(&[le(adrp), le(add)].concat()).unwrap();
        let adrp2 = 0xB000_0008u32 | (0x7FF << 5);
        let add2 = 0x9100_0108u32 | (0x111 << 10);
        let q = Pattern::from_a64(&[le(adrp2), le(add2)].concat()).unwrap();
        assert_eq!(p.hash(), q.hash());
        // an ADD on an unrelated register is NOT masked
        let other = 0x9100_0129u32 | (0x7AB << 10);
        let r = Pattern::from_a64(&[le(adrp), le(other)].concat()).unwrap();
        assert_ne!(p.hash(), r.hash());
    }

    #[test]
    fn different_instructions_hash_differently() {
        let p = Pattern::from_a64(&[le(0xD503201F), le(0xD65F03C0)].concat()).unwrap();
        let q = Pattern::from_a64(&[le(0xD65F03C0), le(0xD503201F)].concat()).unwrap();
        assert_ne!(p.hash(), q.hash());
        assert_eq!(p.hash().len(), HASH_HEX_LEN);
    }

    #[test]
    fn matching_honours_mask_and_alignment() {
        let code: Vec<u8> = [
            le(0xD503201F),
            le(0x9400_0010),
            le(0xA9BF7BFD),
            le(0xD65F03C0),
        ]
        .concat();
        let pat = Pattern::from_a64(&[le(0x9400_0999), le(0xA9BF7BFD)].concat()).unwrap();
        assert_eq!(pat.find_all(&code, 4, 10), vec![4]);
        // different opcode in the pinned part: no hit
        let miss = Pattern::from_a64(&[le(0x9400_0999), le(0xA9BF7BFC)].concat()).unwrap();
        assert!(miss.find_all(&code, 4, 10).is_empty());
        // hay shorter than pattern
        assert!(pat.find_all(&code[..4], 4, 10).is_empty());
    }

    #[test]
    fn ids_validate() {
        assert!(TitleId::parse("0100AAAA00000000").is_ok());
        assert!(TitleId::parse("../etc").is_err());
        assert!(BuildId::parse("AB".repeat(20).as_str()).is_ok());
        assert!(BuildId::parse("zz").is_err());
    }
}
