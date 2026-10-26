//! Signature validator: accepts only what is safe to commit and share.
//!
//! The goal is to protect copyright and privacy: a signature file may carry addresses, short masked
//! instruction patterns, names, hashes and struct layouts, never game dumps, long byte runs, text
//! blobs, paths or personal data. Anything that looks like game data is REJECTED, not trimmed.
use crate::domain::*;

pub const MAX_FILE_BYTES: usize = 1 << 20;
pub const MAX_ENTRIES: usize = 5000;
pub const MAX_RANGE: u64 = 1 << 20;
pub const MAX_NAME: usize = 200;
const MAX_FIELDS: usize = 256;
const MAX_PARAMS: usize = 8;
const MAX_PARAM_VALUE: usize = 64;
/// bits per char above which a long string is treated as an encoded blob
const ENTROPY_LIMIT: f64 = 4.6;
const ENTROPY_MIN_LEN: usize = 32;
const MAX_HEX_RUN: usize = 24;
const MAX_ASCII_RUN: usize = 8;

pub fn shannon(s: &[u8]) -> f64 {
    if s.is_empty() {
        return 0.0;
    }
    let mut c = [0usize; 256];
    for b in s {
        c[*b as usize] += 1;
    }
    let n = s.len() as f64;
    c.iter()
        .filter(|x| **x > 0)
        .map(|x| {
            let p = *x as f64 / n;
            -p * p.log2()
        })
        .sum()
}

fn longest_run(s: &str, f: impl Fn(u8) -> bool) -> usize {
    let (mut best, mut cur) = (0, 0);
    for b in s.bytes() {
        if f(b) {
            cur += 1;
            best = best.max(cur);
        } else {
            cur = 0;
        }
    }
    best
}

/// Free-text field check shared by name/module/params/layout strings.
fn check_text(what: &str, s: &str, max: usize, errs: &mut Vec<String>) {
    if s.is_empty() || s.len() > max {
        errs.push(format!("{what}: length must be 1..={max}"));
        return;
    }
    if !s
        .bytes()
        .all(|c| c.is_ascii_alphanumeric() || b"_:.<>~$-*&,[]()".contains(&c))
    {
        errs.push(format!(
            "{what}: characters outside the identifier set (spaces, paths, text are not allowed)"
        ));
    }
    if longest_run(s, |c| c.is_ascii_hexdigit()) > MAX_HEX_RUN {
        errs.push(format!(
            "{what}: contains a long hex run (looks like a data blob)"
        ));
    }
    if s.len() >= ENTROPY_MIN_LEN && shannon(s.as_bytes()) > ENTROPY_LIMIT {
        errs.push(format!(
            "{what}: high-entropy string (looks like encoded data)"
        ));
    }
    let low = s.to_ascii_lowercase();
    if low.contains("/users/") || low.contains("\\users\\") || s.contains('@') {
        errs.push(format!("{what}: looks like a path or an identity"));
    }
}

fn check_pattern(key: &str, e: &Signature, errs: &mut Vec<String>) {
    let at = |m: &str| format!("signatures[{key}].pattern: {m}");
    let (b, m) = match (unhex(&e.pattern), unhex(&e.mask)) {
        (Ok(b), Ok(m)) => (b, m),
        _ => return errs.push(at("pattern/mask must be even-length hex")),
    };
    if b.len() != m.len() {
        return errs.push(at("pattern and mask differ in length"));
    }
    if b.len() > MAX_PATTERN_BYTES {
        return errs.push(at(&format!(
            "{} bytes exceeds the {MAX_PATTERN_BYTES}-byte limit (no game dumps)",
            b.len()
        )));
    }
    if b.is_empty() {
        return errs.push(at("empty"));
    }
    if e.kind.is_code() && b.len() % 4 != 0 {
        errs.push(at("code patterns must be whole A64 words"));
    }
    if b.iter().zip(&m).any(|(x, y)| x & !y != 0) {
        errs.push(at(
            "pattern has bits set under the wildcard mask (not canonical)",
        ));
    }
    let p = Pattern {
        bytes: b.clone(),
        mask: m.clone(),
    };
    if p.fixed_bytes() < MIN_FIXED_BYTES {
        errs.push(at(&format!(
            "fewer than {MIN_FIXED_BYTES} fully pinned bytes (not identifying)"
        )));
    }
    // A run of printable ASCII in the pinned bytes means a string dump, never code.
    let mut run = 0;
    let mut best = 0;
    for (x, y) in b.iter().zip(&m) {
        if *y == 0xFF && (0x20..0x7F).contains(x) {
            run += 1;
            best = best.max(run);
        } else {
            run = 0;
        }
    }
    if best > MAX_ASCII_RUN {
        errs.push(at("long printable-ASCII run (string data)"));
    }
    if p.hash() != key {
        errs.push(at(&format!(
            "key does not equal the pattern hash {}",
            p.hash()
        )));
    }
}

pub fn validate(f: &SignatureFile) -> Vec<String> {
    let mut errs = Vec::new();
    if f.schema_version != SCHEMA_VERSION {
        errs.push(format!("schema_version must be {SCHEMA_VERSION}"));
    }
    if TitleId::parse(&f.title_id)
        .map(|t| t.0 != f.title_id)
        .unwrap_or(true)
    {
        errs.push("title_id must be 16 upper-case hex digits".into());
    }
    if BuildId::parse(&f.build_id)
        .map(|t| t.0 != f.build_id)
        .unwrap_or(true)
    {
        errs.push("build_id must be 16..64 lower-case hex digits".into());
    }
    if f.signatures.len() > MAX_ENTRIES {
        errs.push(format!("more than {MAX_ENTRIES} signatures in one file"));
    }
    for (key, e) in &f.signatures {
        let at = |m: &str| format!("signatures[{key}]: {m}");
        if key.len() != HASH_HEX_LEN || !key.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
        {
            errs.push(at(&format!(
                "key must be {HASH_HEX_LEN} lower-case hex digits"
            )));
        }
        check_pattern(key, e, &mut errs);
        check_text(&at("module"), &e.module, 32, &mut errs);
        if let Some(n) = &e.name {
            check_text(&at("name"), n, MAX_NAME, &mut errs);
        }
        if e.offset_end <= e.offset_start
            || e.offset_end - e.offset_start > MAX_RANGE
            || e.offset_end > u32::MAX as u64
        {
            errs.push(at(&format!(
                "offset range must be non-empty, within 32 bits and at most {MAX_RANGE:#x} bytes"
            )));
        }
        if e.kind == SigKind::Struct && e.layout.is_none() {
            errs.push(at("kind struct requires a layout"));
        }
        if let Some(l) = &e.layout {
            if l.fields.len() > MAX_FIELDS || l.size == 0 || l.size > MAX_RANGE as u32 {
                errs.push(at("layout size/field count out of range"));
            }
            for fl in &l.fields {
                check_text(&at("layout field name"), &fl.name, 64, &mut errs);
                check_text(&at("layout field type"), &fl.ty, 16, &mut errs);
                if fl.size == 0
                    || fl
                        .offset
                        .checked_add(fl.size)
                        .map(|x| x > l.size)
                        .unwrap_or(true)
                {
                    errs.push(at(&format!("field {} exceeds the struct size", fl.name)));
                }
            }
        }
        if e.resolution.params.len() > MAX_PARAMS {
            errs.push(at("too many resolution params"));
        }
        for (k, v) in &e.resolution.params {
            check_text(&at("param key"), k, 32, &mut errs);
            check_text(&at(&format!("param {k}")), v, MAX_PARAM_VALUE, &mut errs);
        }
        match (e.resolution.action, ["target", "recipe", "value"]) {
            (ResolutionAction::Hook, _) if !e.resolution.params.contains_key("target") => {
                errs.push(at("hook needs params.target"))
            }
            (ResolutionAction::Rewrite, _) if !e.resolution.params.contains_key("recipe") => {
                errs.push(at("rewrite needs params.recipe"))
            }
            (ResolutionAction::Stub, _) if !e.resolution.params.contains_key("value") => {
                errs.push(at("stub needs params.value"))
            }
            _ => {}
        }
    }
    errs
}

/// Full path used for files from disk/PRs: size cap, strict parse (unknown fields rejected), then rules.
pub fn validate_json(text: &str) -> Result<SignatureFile, Vec<String>> {
    if text.len() > MAX_FILE_BYTES {
        return Err(vec![format!("file larger than {MAX_FILE_BYTES} bytes")]);
    }
    let f: SignatureFile =
        serde_json::from_str(text).map_err(|e| vec![format!("parse error: {e}")])?;
    let errs = validate(&f);
    if errs.is_empty() { Ok(f) } else { Err(errs) }
}

#[cfg(test)]
pub(crate) mod fixtures {
    use super::*;
    use std::collections::BTreeMap;

    pub fn sample_pattern() -> Pattern {
        let w = [
            0xA9BF7BFDu32,
            0x9100_03FD,
            0xAA0003F3,
            0x9400_0001,
            0xD65F03C0,
        ];
        Pattern::from_a64(&w.iter().flat_map(|x| x.to_le_bytes()).collect::<Vec<_>>()).unwrap()
    }

    pub fn sample_sig(p: &Pattern) -> Signature {
        Signature {
            kind: SigKind::Function,
            module: "main".into(),
            offset_start: 0x1000,
            offset_end: 0x1040,
            pattern: p.bytes_hex(),
            mask: p.mask_hex(),
            name: Some("nn::os::Example".into()),
            layout: None,
            resolution: Resolution {
                action: ResolutionAction::Annotate,
                params: BTreeMap::new(),
            },
            provenance: Provenance::Local,
            confidence: Confidence::Medium,
        }
    }

    pub fn sample_file() -> SignatureFile {
        let p = sample_pattern();
        let mut f = SignatureFile::new(
            &TitleId("0100AAAA00000000".into()),
            &BuildId("ab".repeat(16)),
        );
        f.signatures.insert(p.hash(), sample_sig(&p));
        f
    }
}

#[cfg(test)]
mod tests {
    use super::fixtures::*;
    use super::*;

    #[test]
    fn committed_examples_validate() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../knownSignatures/_examples");
        let mut n = 0;
        let visible = |e: &std::io::Result<std::fs::DirEntry>| {
            !e.as_ref()
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with('.')
        };
        for t in std::fs::read_dir(dir).unwrap().filter(visible) {
            for f in std::fs::read_dir(t.unwrap().path())
                .unwrap()
                .filter(visible)
            {
                let text = std::fs::read_to_string(f.unwrap().path()).unwrap();
                let f = validate_json(&text).unwrap_or_else(|e| panic!("{e:?}"));
                assert!(f.signatures.len() >= 3);
                n += 1;
            }
        }
        assert!(n >= 1);
    }

    #[test]
    fn good_file_passes_and_round_trips() {
        let f = sample_file();
        assert!(validate(&f).is_empty(), "{:?}", validate(&f));
        assert_eq!(validate_json(&f.to_json()).unwrap(), f);
    }

    fn bad(mutate: impl FnOnce(&mut SignatureFile)) -> Vec<String> {
        let mut f = sample_file();
        mutate(&mut f);
        validate(&f)
    }
    fn first_mut(f: &mut SignatureFile) -> &mut Signature {
        f.signatures.values_mut().next().unwrap()
    }

    #[test]
    fn rejects_over_long_pattern() {
        let e = bad(|f| {
            let key = f.signatures.keys().next().unwrap().clone();
            let mut s = f.signatures.remove(&key).unwrap();
            let code: Vec<u8> = (0..32u32)
                .flat_map(|i| (0xD503201Fu32 ^ (i << 8)).to_le_bytes())
                .collect();
            let p = Pattern::from_a64(&code).unwrap();
            s.pattern = p.bytes_hex();
            s.mask = p.mask_hex();
            f.signatures.insert(p.hash(), s);
        });
        assert!(e.iter().any(|m| m.contains("exceeds")), "{e:?}");
    }

    #[test]
    fn rejects_wrong_key_and_noncanonical_and_weak_patterns() {
        let e = bad(|f| {
            let s = f.signatures.values().next().unwrap().clone();
            f.signatures.clear();
            f.signatures.insert("0".repeat(32), s);
        });
        assert!(e.iter().any(|m| m.contains("pattern hash")));
        let e = bad(|f| {
            let s = first_mut(f);
            s.mask = "00".repeat(20);
        });
        assert!(
            e.iter()
                .any(|m| m.contains("fully pinned") || m.contains("wildcard")),
            "{e:?}"
        );
    }

    #[test]
    fn rejects_ascii_string_dump_in_pattern() {
        let e = bad(|f| {
            let s = first_mut(f);
            let text = b"https://example.invalid/secret/path/AAAA";
            s.kind = SigKind::Pointer;
            s.pattern = hex(&text[..40]);
            s.mask = "ff".repeat(40);
        });
        assert!(e.iter().any(|m| m.contains("ASCII")), "{e:?}");
    }

    #[test]
    fn rejects_blobs_strings_paths_and_unknown_fields() {
        let e = bad(|f| first_mut(f).name = Some("deadbeef".repeat(8)));
        assert!(e.iter().any(|m| m.contains("hex run")), "{e:?}");
        let e = bad(|f| first_mut(f).name = Some("aZ9xQ2mP7vK4tR8wL1nB6cY3hJ5dF0gS".into()));
        assert!(e.iter().any(|m| m.contains("entropy")), "{e:?}");
        let e = bad(|f| first_mut(f).name = Some("/Users/neto/game".into()));
        assert!(!e.is_empty());
        let e = bad(|f| first_mut(f).name = Some("some prose with spaces".into()));
        assert!(e.iter().any(|m| m.contains("characters")));
        let mut v: serde_json::Value = serde_json::from_str(&sample_file().to_json()).unwrap();
        v["signatures"]
            .as_object_mut()
            .unwrap()
            .values_mut()
            .next()
            .unwrap()["raw_dump"] = "AAAA".into();
        assert!(validate_json(&v.to_string()).unwrap_err()[0].contains("unknown field"));
    }

    #[test]
    fn rejects_bad_ranges_ids_layouts_and_resolution() {
        assert!(!bad(|f| first_mut(f).offset_end = 0x1000).is_empty());
        assert!(!bad(|f| first_mut(f).offset_end = 0x1000 + MAX_RANGE + 1).is_empty());
        assert!(!bad(|f| f.build_id = "xyz".into()).is_empty());
        assert!(!bad(|f| f.title_id = "0100aaaa00000000".into()).is_empty());
        assert!(!bad(|f| first_mut(f).kind = SigKind::Struct).is_empty());
        assert!(!bad(|f| first_mut(f).resolution.action = ResolutionAction::Hook).is_empty());
        assert!(!bad(|f| f.schema_version = 2).is_empty());
        let big = "x".repeat(MAX_FILE_BYTES + 1);
        assert!(validate_json(&big).is_err());
    }
}
