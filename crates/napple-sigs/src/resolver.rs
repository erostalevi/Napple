//! Matching of known signatures against a module, and the resolver pipeline:
//! known signature (by pattern hash) -> miss -> Reverser -> validate -> store as pending-share.
use crate::domain::*;
use crate::repo::SignatureRepository;
use crate::reverser::{ReverseRequest, Reverser};
use crate::validate;
use napple_core::infrastructure::nso::Nso;
use std::collections::BTreeMap;
use std::path::Path;

pub struct Segment {
    pub name: &'static str,
    pub base: u64,
    pub bytes: Vec<u8>,
}

/// A loaded module image (offsets are relative to the module base).
pub struct Module {
    pub name: String,
    pub build_id: Option<BuildId>,
    pub segments: Vec<Segment>,
}

impl Module {
    pub fn from_nso(name: &str, file: &[u8]) -> Result<Self, String> {
        let n = Nso::parse(file).map_err(|e| format!("NSO: {e}"))?;
        let build = BuildId::parse(&n.build_id_hex())?;
        let seg = |name, s: &napple_core::infrastructure::nso::Segment| Segment {
            name,
            base: s.mem_offset as u64,
            bytes: s.data.clone(),
        };
        Ok(Self {
            name: name.into(),
            build_id: Some(build),
            segments: vec![
                seg("text", &n.text),
                seg("rodata", &n.rodata),
                seg("data", &n.data),
            ],
        })
    }

    /// Raw code (e.g. a patched `.bin`): one text segment at base 0.
    pub fn from_raw(name: &str, bytes: Vec<u8>, build_id: Option<BuildId>) -> Self {
        Self {
            name: name.into(),
            build_id,
            segments: vec![Segment {
                name: "text",
                base: 0,
                bytes,
            }],
        }
    }

    pub fn text(&self) -> Result<&Segment, String> {
        self.segments
            .iter()
            .find(|s| s.name == "text")
            .ok_or_else(|| "module has no text segment".into())
    }
}

/// Module name from a file name: `main`, `sdk`, `subsdk0` (extension stripped; sanitised).
pub fn module_name_from_path(p: &Path) -> String {
    let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("main");
    let clean: String = stem
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '_')
        .take(32)
        .collect();
    if clean.is_empty() {
        "main".into()
    } else {
        clean.to_ascii_lowercase()
    }
}

#[derive(Debug, Clone)]
pub struct Hit {
    pub hash: String,
    pub name: Option<String>,
    pub kind: SigKind,
    pub offsets: Vec<u64>,
    /// The (single) hit lies inside the signature's recorded offset range.
    pub in_range: bool,
}

#[derive(Debug, Default)]
pub struct MatchReport {
    pub hits: Vec<Hit>,
    /// Signatures for this module that matched nowhere (build drifted or wrong build).
    pub unknown: Vec<String>,
    /// Signatures that belong to other modules of the build and were not scanned.
    pub skipped_other_module: usize,
}

const MAX_HITS: usize = 8;

pub fn match_module(file: &SignatureFile, m: &Module) -> Result<MatchReport, String> {
    let mut rep = MatchReport::default();
    for (hash, s) in &file.signatures {
        if s.module != m.name {
            rep.skipped_other_module += 1;
            continue;
        }
        let p = s.pattern()?;
        let mut offsets = Vec::new();
        for seg in &m.segments {
            if s.kind.is_code() && seg.name != "text" {
                continue;
            }
            let step = if s.kind.is_code() { 4 } else { 1 };
            for o in p.find_all(&seg.bytes, step, MAX_HITS) {
                offsets.push(seg.base + o as u64);
            }
        }
        offsets.truncate(MAX_HITS);
        if offsets.is_empty() {
            rep.unknown.push(hash.clone());
        } else {
            let in_range =
                offsets.len() == 1 && (s.offset_start..s.offset_end).contains(&offsets[0]);
            rep.hits.push(Hit {
                hash: hash.clone(),
                name: s.name.clone(),
                kind: s.kind,
                offsets,
                in_range,
            });
        }
    }
    Ok(rep)
}

/// Shortest masked pattern at `offset` (module-relative, inside the text segment) that occurs exactly once in the text.
pub fn derive_pattern(text: &Segment, offset: u64) -> Result<Pattern, String> {
    let at = offset
        .checked_sub(text.base)
        .filter(|o| (*o as usize) < text.bytes.len())
        .ok_or("offset is outside the text segment")? as usize;
    if !at.is_multiple_of(4) {
        return Err("offset is not 4-byte aligned".into());
    }
    let avail = (text.bytes.len() - at).min(MAX_PATTERN_BYTES);
    let mut n = MIN_PATTERN_BYTES;
    while n <= avail {
        let p = Pattern::from_a64(&text.bytes[at..at + n])?;
        if p.fixed_bytes() >= MIN_FIXED_BYTES {
            let hits = p.find_all(&text.bytes, 4, 2);
            if hits == [at] {
                return Ok(p);
            }
        }
        n += 4;
    }
    Err(format!(
        "no unique pattern of up to {MAX_PATTERN_BYTES} bytes at this offset (duplicated or too generic code)"
    ))
}

#[derive(Clone, Debug)]
pub struct Item {
    pub offset: u64,
    pub kind: SigKind,
    pub name: Option<String>,
    /// Size supplied by the user; used for a local signature when no Reverser result exists.
    pub size: Option<u64>,
}

#[derive(Debug)]
pub enum Outcome {
    Known { hash: String },
    Carried { hash: String, from_build: BuildId },
    Learned { hash: String, via: String },
    Manual { hash: String, why: String },
    Failed { reason: String },
}

pub struct Context<'a> {
    pub title: &'a TitleId,
    pub build: &'a BuildId,
    /// Known + pending + shared + cache, merged.
    pub known: &'a dyn SignatureRepository,
    /// Where new signatures go (pending-share store).
    pub pending: &'a dyn SignatureRepository,
    pub reverser: Option<&'a dyn Reverser>,
    /// Directory (inside work/) where REA image slices are written.
    pub scratch: &'a Path,
}

const SLICE_BYTES: usize = 64 << 10;

// A signature is built from exactly these facts; a parameter struct would only restate them.
#[allow(clippy::too_many_arguments)]
fn new_sig(
    item: &Item,
    m: &Module,
    p: &Pattern,
    start: u64,
    end: u64,
    name: Option<String>,
    prov: Provenance,
    conf: Confidence,
) -> Signature {
    Signature {
        kind: item.kind,
        module: m.name.clone(),
        offset_start: start,
        offset_end: end,
        pattern: p.bytes_hex(),
        mask: p.mask_hex(),
        name,
        layout: None,
        resolution: Resolution {
            action: ResolutionAction::Annotate,
            params: BTreeMap::new(),
        },
        provenance: prov,
        confidence: conf,
    }
}

fn store(ctx: &Context, hash: &str, sig: Signature) -> Result<(), String> {
    let mut f = SignatureFile::new(ctx.title, ctx.build);
    f.signatures.insert(hash.to_string(), sig);
    let errs = validate::validate(&f);
    if !errs.is_empty() {
        return Err(format!("validation failed: {}", errs.join("; ")));
    }
    ctx.pending.save(&f)
}

pub fn resolve_item(ctx: &Context, m: &Module, item: &Item) -> Outcome {
    let fail = |r: String| Outcome::Failed { reason: r };
    if !item.kind.is_code() {
        return fail(format!(
            "kind {:?} cannot be derived from code; add it by hand or via a patch table",
            item.kind
        ));
    }
    let text = match m.text() {
        Ok(t) => t,
        Err(e) => return fail(e),
    };
    let pattern = match derive_pattern(text, item.offset) {
        Ok(p) => p,
        Err(e) => return fail(e),
    };
    let hash = pattern.hash();
    // 1. known by pattern hash in this build
    match ctx.known.lookup(ctx.title, ctx.build, &hash) {
        Ok(Some(_)) => return Outcome::Known { hash },
        Ok(None) => {}
        Err(e) => return fail(e),
    }
    // 2. same pattern in another build of the title: carry it over (still validated + pending)
    if let Ok(found) = ctx.known.find_by_hash(ctx.title, &hash)
        && let Some((b, s)) = found.into_iter().find(|(b, _)| b != ctx.build)
    {
        let len = s.offset_end - s.offset_start;
        let mut ns = new_sig(
            item,
            m,
            &pattern,
            item.offset,
            item.offset + len,
            s.name.clone(),
            Provenance::Local,
            s.confidence.min(Confidence::Medium),
        );
        ns.layout = s.layout.clone();
        ns.resolution = s.resolution.clone();
        return match store(ctx, &hash, ns) {
            Ok(()) => Outcome::Carried {
                hash,
                from_build: b,
            },
            Err(e) => fail(e),
        };
    }
    // 3. Reverser
    let mut why = String::from("no reverser configured");
    if let Some(r) = ctx.reverser {
        match reverse(ctx, r, m, text, item) {
            Ok((start, end, name, tool)) => {
                let name = name.or_else(|| item.name.clone());
                let sig = new_sig(
                    item,
                    m,
                    &pattern,
                    start,
                    end,
                    name,
                    Provenance::Rea,
                    Confidence::Medium,
                );
                return match store(ctx, &hash, sig) {
                    Ok(()) => Outcome::Learned { hash, via: tool },
                    Err(e) => fail(e),
                };
            }
            Err(e) => why = format!("reverser failed: {e}"),
        }
    }
    // 4. user-supplied size: local, low confidence
    match item.size {
        Some(sz) if sz > 0 && sz <= validate::MAX_RANGE => {
            let sig = new_sig(
                item,
                m,
                &pattern,
                item.offset,
                item.offset + sz,
                item.name.clone(),
                Provenance::Local,
                Confidence::Low,
            );
            match store(ctx, &hash, sig) {
                Ok(()) => Outcome::Manual { hash, why },
                Err(e) => fail(e),
            }
        }
        _ => fail(format!(
            "{why}; give a size (OFFSET:KIND:NAME:SIZE) to record it locally"
        )),
    }
}

fn reverse(
    ctx: &Context,
    r: &dyn Reverser,
    m: &Module,
    text: &Segment,
    item: &Item,
) -> Result<(u64, u64, Option<String>, String), String> {
    let at = (item.offset - text.base) as usize;
    let end = (at + SLICE_BYTES).min(text.bytes.len());
    std::fs::create_dir_all(ctx.scratch).map_err(|e| e.to_string())?;
    let file = ctx.scratch.join(format!(
        "{}-{}-{:x}.bin",
        &ctx.build.0[..16],
        m.name,
        item.offset
    ));
    {
        use std::io::Write;
        use std::os::unix::fs::OpenOptionsExt;
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&file)
            .map_err(|e| e.to_string())?;
        f.write_all(&text.bytes[at..end])
            .map_err(|e| e.to_string())?;
    }
    let res = r.analyze(&ReverseRequest {
        module: m.name.clone(),
        image: file.clone(),
        load_address: item.offset,
        kind: item.kind,
    });
    let _ = std::fs::remove_file(&file); // game bytes never linger in scratch
    let f = res?;
    // Validate: the finding must start at the item and stay in the text segment.
    if f.start != item.offset || f.end <= f.start || f.end > text.base + text.bytes.len() as u64 {
        return Err("reverser result is inconsistent with the module".into());
    }
    Ok((f.start, f.end, f.name, f.tool))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::repo::DirRepository;
    use crate::reverser::{ReverseFinding, ReverseRequest};
    use std::cell::Cell;

    fn le(w: &[u32]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    /// Synthetic module: padding, then two distinct functions.
    fn module() -> Module {
        let mut code = le(&[0xD503201F; 8]);
        code.extend(le(&[
            0xA9BF7BFD, 0x910003FD, 0xAA0003F3, 0x94000010, 0x2A0003E0, 0xA8C17BFD, 0xD65F03C0,
        ])); // @0x20
        code.extend(le(&[0xD503201F; 4]));
        code.extend(le(&[
            0xA9BF7BFD, 0x910003FD, 0xAA0103F3, 0x94000FF0, 0x2A0103E0, 0xA8C17BFD, 0xD65F03C0,
        ])); // @0x4c, differs in 3rd/5th word
        Module::from_raw("main", code, Some(BuildId("ab".repeat(16))))
    }

    struct Fake(Cell<u32>, Result<ReverseFinding, String>);
    impl Reverser for Fake {
        fn label(&self) -> String {
            "fake".into()
        }
        fn analyze(&self, req: &ReverseRequest) -> Result<ReverseFinding, String> {
            self.0.set(self.0.get() + 1);
            assert!(req.image.exists());
            self.1.clone().map(|mut f| {
                f.start = req.load_address;
                f.end = req.load_address + 0x1c;
                f
            })
        }
    }
    fn finding() -> ReverseFinding {
        ReverseFinding {
            start: 0,
            end: 0,
            name: Some("nn::Thing".into()),
            evidence_id: None,
            tool: "fake".into(),
        }
    }

    fn setup() -> (tempfile::TempDir, TitleId, BuildId) {
        (
            tempfile::tempdir().unwrap(),
            TitleId("0100AAAA00000000".into()),
            BuildId("ab".repeat(16)),
        )
    }

    #[test]
    fn derive_pattern_is_shortest_unique() {
        let m = module();
        let t = m.text().unwrap();
        let p = derive_pattern(t, 0x20).unwrap();
        // 3rd word differs between the two functions => 12 bytes are not unique, need 12 (words 0..2)? first 2 words equal, third differs
        assert!(p.len() >= 12 && p.len() <= 20, "{}", p.len());
        assert_eq!(p.find_all(&t.bytes, 4, 5), vec![0x20]);
        assert!(derive_pattern(t, 0x22).is_err()); // unaligned
        assert!(derive_pattern(t, 0x1000).is_err()); // out of range
    }

    #[test]
    fn miss_then_reverser_then_pending_then_known() {
        let (d, t, b) = setup();
        let known = DirRepository::new(d.path().join("known"), "known", false);
        let pending = DirRepository::new(d.path().join("pending"), "pending", true);
        let fake = Fake(Cell::new(0), Ok(finding()));
        let ctx = Context {
            title: &t,
            build: &b,
            known: &known,
            pending: &pending,
            reverser: Some(&fake),
            scratch: &d.path().join("scratch"),
        };
        let m = module();
        let item = Item {
            offset: 0x20,
            kind: SigKind::Function,
            name: None,
            size: None,
        };
        let Outcome::Learned { hash, .. } = resolve_item(&ctx, &m, &item) else {
            panic!()
        };
        assert_eq!(fake.0.get(), 1);
        let f = pending.load(&t, &b).unwrap().unwrap();
        let s = &f.signatures[&hash];
        assert_eq!(
            (s.provenance, s.confidence, s.offset_end),
            (Provenance::Rea, Confidence::Medium, 0x3c)
        );
        assert_eq!(s.name.as_deref(), Some("nn::Thing"));
        // scratch image removed
        assert_eq!(
            std::fs::read_dir(d.path().join("scratch")).unwrap().count(),
            0
        );
        // once pending is part of the known view the reverser is no longer consulted
        let ctx2 = Context {
            known: &pending,
            ..ctx
        };
        assert!(matches!(
            resolve_item(&ctx2, &m, &item),
            Outcome::Known { .. }
        ));
        assert_eq!(fake.0.get(), 1);
    }

    #[test]
    fn reverser_failure_falls_back_to_manual_or_fails() {
        let (d, t, b) = setup();
        let known = DirRepository::new(d.path().join("known"), "known", false);
        let pending = DirRepository::new(d.path().join("pending"), "pending", true);
        let fake = Fake(Cell::new(0), Err("boom".into()));
        let ctx = Context {
            title: &t,
            build: &b,
            known: &known,
            pending: &pending,
            reverser: Some(&fake),
            scratch: &d.path().join("s"),
        };
        let m = module();
        let no_size = Item {
            offset: 0x20,
            kind: SigKind::Function,
            name: None,
            size: None,
        };
        let Outcome::Failed { reason } = resolve_item(&ctx, &m, &no_size) else {
            panic!()
        };
        assert!(reason.contains("boom"));
        assert!(pending.load(&t, &b).unwrap().is_none());
        let with_size = Item {
            size: Some(0x1c),
            name: Some("my_fn".into()),
            ..no_size
        };
        let Outcome::Manual { hash, .. } = resolve_item(&ctx, &m, &with_size) else {
            panic!()
        };
        let s = pending.load(&t, &b).unwrap().unwrap().signatures[&hash].clone();
        assert_eq!(
            (s.provenance, s.confidence),
            (Provenance::Local, Confidence::Low)
        );
    }

    #[test]
    fn pattern_known_in_other_build_is_carried_over() {
        let (d, t, b) = setup();
        let m = module();
        let p = derive_pattern(m.text().unwrap(), 0x20).unwrap();
        let other = BuildId("cd".repeat(16));
        let known = DirRepository::new(d.path().join("known"), "known", true);
        let mut f = SignatureFile::new(&t, &other);
        let mut s = new_sig(
            &Item {
                offset: 0x500,
                kind: SigKind::Function,
                name: None,
                size: None,
            },
            &m,
            &p,
            0x500,
            0x520,
            Some("nn::Old".into()),
            Provenance::Community,
            Confidence::High,
        );
        s.resolution = Resolution {
            action: ResolutionAction::Hook,
            params: [("target".to_string(), "hle_old".to_string())].into(),
        };
        f.signatures.insert(p.hash(), s);
        known.save(&f).unwrap();
        let pending = DirRepository::new(d.path().join("pending"), "pending", true);
        let ctx = Context {
            title: &t,
            build: &b,
            known: &known,
            pending: &pending,
            reverser: None,
            scratch: &d.path().join("s"),
        };
        let Outcome::Carried { hash, from_build } = resolve_item(
            &ctx,
            &m,
            &Item {
                offset: 0x20,
                kind: SigKind::Function,
                name: None,
                size: None,
            },
        ) else {
            panic!()
        };
        assert_eq!(from_build, other);
        let n = &pending.load(&t, &b).unwrap().unwrap().signatures[&hash];
        assert_eq!(
            (n.offset_start, n.offset_end, n.confidence, n.provenance),
            (0x20, 0x40, Confidence::Medium, Provenance::Local)
        );
        assert_eq!(n.resolution.action, ResolutionAction::Hook);
    }

    #[test]
    fn non_code_kind_is_refused() {
        let (d, t, b) = setup();
        let known = DirRepository::new(d.path().join("k"), "k", false);
        let pending = DirRepository::new(d.path().join("p"), "p", true);
        let ctx = Context {
            title: &t,
            build: &b,
            known: &known,
            pending: &pending,
            reverser: None,
            scratch: d.path(),
        };
        assert!(matches!(
            resolve_item(
                &ctx,
                &module(),
                &Item {
                    offset: 0x20,
                    kind: SigKind::Struct,
                    name: None,
                    size: None
                }
            ),
            Outcome::Failed { .. }
        ));
    }

    #[test]
    fn match_reports_hits_unknowns_and_range() {
        let m = module();
        let (t, b) = (TitleId("0100AAAA00000000".into()), BuildId("ab".repeat(16)));
        let mut f = SignatureFile::new(&t, &b);
        let item = Item {
            offset: 0x20,
            kind: SigKind::Function,
            name: None,
            size: None,
        };
        let p = derive_pattern(m.text().unwrap(), 0x20).unwrap();
        f.signatures.insert(
            p.hash(),
            new_sig(
                &item,
                &m,
                &p,
                0x20,
                0x3c,
                Some("a".into()),
                Provenance::Local,
                Confidence::Low,
            ),
        );
        // second signature: moved (range wrong) but still found
        let p2 = derive_pattern(m.text().unwrap(), 0x4c).unwrap();
        f.signatures.insert(
            p2.hash(),
            new_sig(
                &item,
                &m,
                &p2,
                0x900,
                0x920,
                None,
                Provenance::Local,
                Confidence::Low,
            ),
        );
        // third: not in the module
        let p3 = Pattern::from_a64(&le(&[0x12345678, 0x9ABCDEF0, 0x0F1E2D3C])).unwrap();
        f.signatures.insert(
            p3.hash(),
            new_sig(
                &item,
                &m,
                &p3,
                0,
                0x10,
                None,
                Provenance::Local,
                Confidence::Low,
            ),
        );
        // fourth: another module
        let mut s4 = f.signatures[&p.hash()].clone();
        s4.module = "sdk".into();
        f.signatures.insert("1".repeat(32), s4);
        let r = match_module(&f, &m).unwrap();
        assert_eq!(r.hits.len(), 2);
        let h1 = r.hits.iter().find(|h| h.hash == p.hash()).unwrap();
        assert_eq!((h1.offsets.clone(), h1.in_range), (vec![0x20], true));
        let h2 = r.hits.iter().find(|h| h.hash == p2.hash()).unwrap();
        assert_eq!((h2.offsets.clone(), h2.in_range), (vec![0x4c], false));
        assert_eq!(r.unknown, vec![p3.hash()]);
        assert_eq!(r.skipped_other_module, 1);
    }

    #[test]
    fn module_names() {
        assert_eq!(module_name_from_path(Path::new("/x/subsdk0")), "subsdk0");
        assert_eq!(module_name_from_path(Path::new("/x/Main.nso")), "main");
    }
}
