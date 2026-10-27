//! Applying patches to a module's segments, before the AOT rewrite.
//!
//! Hook for `napple-build` (call site proposal in docs/mods/authoring.md and the change note):
//! ```ignore
//! let mut seg = ModuleBytes::from_segments([
//!     (nso.text.mem_offset, std::mem::take(&mut nso.text.data)),
//!     (nso.rodata.mem_offset, std::mem::take(&mut nso.rodata.data)),
//!     (nso.data.mem_offset, std::mem::take(&mut nso.data.data)),
//! ]);
//! let report = napple_mods::apply_mods_to_module(&nso.build_id, &mut seg, &resolved_mods);
//! [nso.text.data, nso.rodata.data, nso.data.data] = seg.into_segments();
//! ```
use crate::model::{PatchKind, ResolvedMod};
use crate::util::{build_id_hex, sha256_hex};
use serde::Serialize;

/// The NSO file header that precedes the segments; patch offsets are file offsets and include it.
pub const NSO_HEADER: u64 = 0x100;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum SegmentKind {
    Text,
    Rodata,
    Data,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    pub kind: SegmentKind,
    pub mem_offset: u64,
    pub data: Vec<u8>,
}

/// The decompressed segments of one module (text, rodata, data in this order).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModuleBytes {
    pub segments: Vec<Segment>,
}

impl ModuleBytes {
    /// `[(mem_offset, bytes); 3]` = text, rodata, data (like `napple_core::infrastructure::nso::Nso`).
    pub fn from_segments(s: [(u32, Vec<u8>); 3]) -> Self {
        let kinds = [SegmentKind::Text, SegmentKind::Rodata, SegmentKind::Data];
        Self {
            segments: s
                .into_iter()
                .zip(kinds)
                .map(|((o, d), kind)| Segment {
                    kind,
                    mem_offset: o as u64,
                    data: d,
                })
                .collect(),
        }
    }

    /// One flat image: text `[0, text_end)`, rodata `[text_end, ro_end)`, data the rest, all at their memory offsets.
    pub fn from_flat(image: &[u8], text_end: usize, ro_end: usize) -> Self {
        let (t, r) = (text_end.min(image.len()), ro_end.min(image.len()));
        let r = r.max(t);
        Self::from_segments([
            (0, image[..t].to_vec()),
            (t as u32, image[t..r].to_vec()),
            (r as u32, image[r..].to_vec()),
        ])
    }

    pub fn into_segments(self) -> [Vec<u8>; 3] {
        let mut it = self.segments.into_iter().map(|s| s.data);
        [
            it.next().unwrap_or_default(),
            it.next().unwrap_or_default(),
            it.next().unwrap_or_default(),
        ]
    }

    pub fn flatten(&self) -> Vec<u8> {
        let end = self
            .segments
            .iter()
            .map(|s| s.mem_offset as usize + s.data.len())
            .max()
            .unwrap_or(0);
        let mut v = vec![0u8; end];
        for s in &self.segments {
            v[s.mem_offset as usize..s.mem_offset as usize + s.data.len()].copy_from_slice(&s.data);
        }
        v
    }

    pub fn sha256(&self) -> String {
        sha256_hex(&self.flatten())
    }

    fn locate(&self, off: u64, len: u64) -> Result<(usize, usize), MissReason> {
        let end = off.checked_add(len).ok_or(MissReason::OutOfRange)?;
        for (i, s) in self.segments.iter().enumerate() {
            let (a, b) = (s.mem_offset, s.mem_offset + s.data.len() as u64);
            if off >= a && off < b {
                return if end <= b {
                    Ok((i, (off - a) as usize))
                } else {
                    Err(MissReason::CrossesSegment)
                };
            }
        }
        Err(MissReason::OutOfRange)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum MissReason {
    /// The record is outside every segment (header, padding or .bss).
    OutOfRange,
    CrossesSegment,
    /// The module-relative offset would be negative (record inside the NSO header).
    InHeader,
    TruncateRejected,
}

#[derive(Debug, Clone, Serialize)]
pub struct Hit {
    pub source: String,
    pub file_offset: u64,
    pub module_offset: u64,
    pub len: usize,
    pub segment: SegmentKind,
}

#[derive(Debug, Clone, Serialize)]
pub struct Miss {
    pub source: String,
    pub file_offset: u64,
    pub len: usize,
    pub reason: MissReason,
}

#[derive(Debug, Clone, Serialize)]
pub struct Overlap {
    pub first: String,
    pub second: String,
    pub module_offset: u64,
    pub len: u64,
    /// Both write the same bytes (harmless) or different bytes (the later one wins).
    pub identical: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct Skipped {
    pub source: String,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Default)]
pub struct ApplyReport {
    pub build_id: String,
    pub dry_run: bool,
    pub hits: Vec<Hit>,
    pub misses: Vec<Miss>,
    pub overlaps: Vec<Overlap>,
    pub skipped: Vec<Skipped>,
    pub bytes_patched: u64,
    pub sha256_before: String,
    pub sha256_after: String,
}

impl ApplyReport {
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).unwrap_or_default()
    }
}

struct Job {
    source: String,
    file_offset: u64,
    data: Vec<u8>,
    truncate: bool,
}

fn jobs_for(build_id: &[u8; 32], mods: &[ResolvedMod], rep: &mut ApplyReport) -> Vec<Job> {
    let mut jobs = Vec::new();
    for m in mods {
        let tag = |p: &str, extra: &str| format!("{}@{}:{}{}", m.id, m.version, p, extra);
        if !m.selection.enabled {
            rep.skipped.push(Skipped {
                source: m.id.clone(),
                reason: "mod not enabled".into(),
            });
            continue;
        }
        for pf in &m.patches {
            let file_key = pf.path.clone();
            let file_on = m.selection.overrides.get(&file_key).copied();
            if !(m.selection.code_patches || file_on == Some(true)) {
                rep.skipped.push(Skipped {
                    source: tag(&pf.path, ""),
                    reason: "code patches not enabled for this mod".into(),
                });
                continue;
            }
            if file_on == Some(false) {
                rep.skipped.push(Skipped {
                    source: tag(&pf.path, ""),
                    reason: "disabled by the user".into(),
                });
                continue;
            }
            match &pf.kind {
                PatchKind::Ips {
                    build_id: bid,
                    patch,
                } => {
                    if bid != build_id {
                        rep.skipped.push(Skipped {
                            source: tag(&pf.path, ""),
                            reason: format!("build id {} is another module", build_id_hex(bid)),
                        });
                        continue;
                    }
                    for r in &patch.records {
                        jobs.push(Job {
                            source: tag(&pf.path, ""),
                            file_offset: r.offset,
                            data: r.data.clone(),
                            truncate: false,
                        });
                    }
                    if let Some(t) = patch.truncate {
                        jobs.push(Job {
                            source: tag(&pf.path, " (truncate)"),
                            file_offset: t,
                            data: vec![],
                            truncate: true,
                        });
                    }
                }
                PatchKind::Pchtxt(p) => {
                    for b in p.builds.iter().filter(|b| &b.build_id == build_id) {
                        for pa in &b.patches {
                            let key = format!("{}#{}", pf.path, pa.name);
                            let on = m
                                .selection
                                .overrides
                                .get(&key)
                                .copied()
                                .unwrap_or(pa.enabled);
                            if !on {
                                rep.skipped.push(Skipped {
                                    source: tag(&pf.path, &format!("#{}", pa.name)),
                                    reason: if pa.enabled {
                                        "disabled by the user"
                                    } else {
                                        "@disabled by default"
                                    }
                                    .into(),
                                });
                                continue;
                            }
                            for r in &pa.records {
                                jobs.push(Job {
                                    source: tag(&pf.path, &format!("#{}", pa.name)),
                                    file_offset: r.offset,
                                    data: r.data.clone(),
                                    truncate: false,
                                });
                            }
                        }
                    }
                }
            }
        }
    }
    jobs
}

fn run(
    build_id: &[u8; 32],
    module: &mut ModuleBytes,
    mods: &[ResolvedMod],
    write: bool,
) -> ApplyReport {
    let mut rep = ApplyReport {
        build_id: build_id_hex(build_id),
        dry_run: !write,
        sha256_before: module.sha256(),
        ..Default::default()
    };
    let jobs = jobs_for(build_id, mods, &mut rep);
    // (start, end, source index into `done`, data) of writes so far, for overlap detection
    let mut done: Vec<(u64, u64, String, Vec<u8>)> = Vec::new();
    for j in jobs {
        if j.truncate {
            rep.misses.push(Miss {
                source: j.source,
                file_offset: j.file_offset,
                len: 0,
                reason: MissReason::TruncateRejected,
            });
            continue;
        }
        let len = j.data.len();
        let Some(off) = j.file_offset.checked_sub(NSO_HEADER) else {
            rep.misses.push(Miss {
                source: j.source,
                file_offset: j.file_offset,
                len,
                reason: MissReason::InHeader,
            });
            continue;
        };
        match module.locate(off, len as u64) {
            Err(reason) => rep.misses.push(Miss {
                source: j.source,
                file_offset: j.file_offset,
                len,
                reason,
            }),
            Ok((si, so)) => {
                let end = off + len as u64;
                for (a, b, src, old) in &done {
                    let (lo, hi) = (off.max(*a), end.min(*b));
                    if lo < hi {
                        let mine = &j.data[(lo - off) as usize..(hi - off) as usize];
                        let theirs = &old[(lo - a) as usize..(hi - a) as usize];
                        rep.overlaps.push(Overlap {
                            first: src.clone(),
                            second: j.source.clone(),
                            module_offset: lo,
                            len: hi - lo,
                            identical: mine == theirs,
                        });
                    }
                }
                let seg = &mut module.segments[si];
                if write {
                    seg.data[so..so + len].copy_from_slice(&j.data);
                }
                rep.bytes_patched += len as u64;
                rep.hits.push(Hit {
                    source: j.source.clone(),
                    file_offset: j.file_offset,
                    module_offset: off,
                    len,
                    segment: seg.kind,
                });
                done.push((off, end, j.source, j.data));
            }
        }
    }
    rep.sha256_after = module.sha256();
    rep
}

/// Applies every enabled patch of `mods` that targets `build_id` (the module's own 32-byte build id: main, subsdk0.. and rtld
/// each have their own) to `module`. Call before the AOT rewrite. Nothing is written for a miss; a report lists hits, misses,
/// overlaps and skipped patches. Never panics on any patch content.
pub fn apply_mods_to_module(
    build_id: &[u8; 32],
    module: &mut ModuleBytes,
    mods: &[ResolvedMod],
) -> ApplyReport {
    run(build_id, module, mods, true)
}

/// Same report as [`apply_mods_to_module`] without changing `module`.
pub fn dry_run_mods_for_module(
    build_id: &[u8; 32],
    module: &ModuleBytes,
    mods: &[ResolvedMod],
) -> ApplyReport {
    let mut copy = module.clone();
    let mut r = run(build_id, &mut copy, mods, true);
    r.dry_run = true; // sha256_after is the hash the module would have
    r
}
