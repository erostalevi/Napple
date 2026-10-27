//! RomFS overlay: `files/<romfs path>` shadows the game's file; `files/<romfs path>.delete` hides it.
use crate::model::LoadedMod;
use crate::util::{Result, err, safe_rel_path};
use std::collections::BTreeMap;

pub const OVERLAY_ROOT: &str = "files/";
pub const DELETE_SUFFIX: &str = ".delete";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Entry {
    /// Served from `entry` (an archive path) of the mod at `mod_index` in the list given to [`build_overlay`].
    File {
        mod_index: usize,
        entry: String,
        size: u64,
    },
    /// A tombstone: the path (and everything below it) does not exist.
    Deleted { mod_index: usize },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Conflict {
    pub path: String,
    pub winner: usize,
    pub loser: usize,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OverlayMap {
    /// Keys are absolute romfs paths with a leading `/`.
    pub entries: BTreeMap<String, Entry>,
    pub conflicts: Vec<Conflict>,
}

pub enum Lookup<'a> {
    /// Not touched by any mod: use the game's own file.
    Original,
    Replaced(&'a Entry),
    Deleted,
}

impl OverlayMap {
    pub fn lookup(&self, path: &str) -> Lookup<'_> {
        match self.entries.get(path) {
            Some(e @ Entry::File { .. }) => return Lookup::Replaced(e),
            Some(Entry::Deleted { .. }) => return Lookup::Deleted,
            None => {}
        }
        let mut p = path;
        while let Some(i) = p.rfind('/') {
            p = &p[..i];
            if matches!(self.entries.get(p), Some(Entry::Deleted { .. })) {
                return Lookup::Deleted;
            }
        }
        Lookup::Original
    }
}

/// Maps one archive path to `(romfs path, is_delete)`.
pub fn overlay_path(archive_path: &str) -> Result<Option<(String, bool)>> {
    let Some(rel) = archive_path.strip_prefix(OVERLAY_ROOT) else {
        return Ok(None);
    };
    safe_rel_path(rel)?;
    let (rel, del) = match rel.strip_suffix(DELETE_SUFFIX) {
        Some(r) if !r.is_empty() && !r.ends_with('/') => (r, true),
        _ => (rel, false),
    };
    Ok(Some((format!("/{rel}"), del)))
}

/// Later mods in `mods` win over earlier ones. Mods that are not enabled should not be passed in.
pub fn build_overlay(mods: &[&LoadedMod]) -> Result<OverlayMap> {
    let mut m = OverlayMap::default();
    for (i, lm) in mods.iter().enumerate() {
        let mut here: BTreeMap<String, Entry> = BTreeMap::new();
        for (p, size) in &lm.listing.entries {
            let Some((path, del)) = overlay_path(p)? else {
                continue;
            };
            if del {
                here.insert(path, Entry::Deleted { mod_index: i });
            } else {
                // a delete marker in the same mod wins over a file of the same name
                here.entry(path).or_insert(Entry::File {
                    mod_index: i,
                    entry: p.clone(),
                    size: *size,
                });
            }
        }
        for (path, e) in here {
            if let Entry::Deleted { .. } = e {
                let doomed: Vec<String> = m
                    .entries
                    .range(path.clone()..)
                    .take_while(|(k, _)| **k == path || k.starts_with(&format!("{path}/")))
                    .map(|(k, _)| k.clone())
                    .collect();
                for k in doomed {
                    m.entries.remove(&k);
                }
            } else if let Some(Entry::File { mod_index, .. }) = m.entries.get(&path) {
                m.conflicts.push(Conflict {
                    path: path.clone(),
                    winner: i,
                    loser: *mod_index,
                });
            }
            m.entries.insert(path, e);
        }
    }
    Ok(m)
}

/// Reads the bytes of an overlay entry.
pub fn read_entry(mods: &[&LoadedMod], e: &Entry, limit: u64) -> Result<Vec<u8>> {
    match e {
        Entry::File {
            mod_index, entry, ..
        } => match mods.get(*mod_index) {
            Some(m) => m.files.read(entry, limit),
            None => err("overlay entry refers to a missing mod"),
        },
        Entry::Deleted { .. } => err("entry is deleted"),
    }
}
