//! Groups user-supplied packs into a build plan: one base game, its updates and its DLC.
use super::ingest::{Ingested, collect, ingest_sources};
use crate::domain::error::{Error, Result};
use crate::domain::keys::KeySet;
use crate::infrastructure::cnmt::{Cnmt, TitleKind, parse_cnmt};
use crate::infrastructure::io::{FileSource, Src};
use crate::infrastructure::nacp::{Nacp, parse_nacp};
use crate::infrastructure::nca::{self, ContentType, FsType, Nca, TitleKeys};
use crate::infrastructure::pfs::parse_pfs0;
use crate::infrastructure::romfs::RomFs;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub struct Pack {
    pub path: PathBuf,
    pub meta: Cnmt,
    ncas: HashMap<String, Src>,
    tks: TitleKeys,
}

impl Pack {
    pub fn size(&self) -> u64 {
        self.meta.contents.iter().map(|c| c.size).sum()
    }

    /// Reads ExeFS (and RomFS when openable) from this pack's Program NCA.
    pub fn program(&self, keys: &KeySet) -> Result<Ingested> {
        let rec = self
            .meta
            .contents
            .iter()
            .find(|c| c.ctype == 1)
            .ok_or_else(|| Error::Container("pack has no Program content".into()))?;
        let src = self
            .ncas
            .get(&rec.nca_name())
            .ok_or_else(|| Error::Container("Program NCA missing from pack".into()))?;
        ingest_sources(vec![(rec.nca_name(), src.clone())], self.tks.clone(), keys)
    }
}

impl Pack {
    /// Reads `control.nacp` from this pack's Control NCA (None when the pack has no readable control data).
    pub fn control(&self, keys: &KeySet) -> Result<Option<Nacp>> {
        let Some(rec) = self.meta.contents.iter().find(|c| c.ctype == 3) else {
            return Ok(None);
        };
        let Some(src) = self.ncas.get(&rec.nca_name()) else {
            return Ok(None);
        };
        let n = match Nca::open(src.clone(), keys, &self.tks) {
            Ok(n) => n,
            Err(Error::Unsupported(_)) => return Ok(None),
            Err(e) => return Err(e),
        };
        for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs) {
            let Ok(s) = n.section(sec.index) else {
                continue;
            };
            let fs = RomFs::parse(nca::romfs_data(&s, sec)?)?;
            if let Some(f) = fs.files.iter().find(|f| f.path == "control.nacp") {
                if f.size > 0x10000 {
                    return Err(Error::Container("implausible control.nacp size".into()));
                }
                let b = crate::infrastructure::io::read_vec(&*fs.open(f)?, 0, f.size as usize)?;
                return parse_nacp(&b).map(Some);
            }
        }
        Ok(None)
    }
}

/// Returns one Pack per content meta found in the file (a combined NSP/XCI can hold several titles).
pub fn scan(path: &Path, keys: &KeySet) -> Result<Vec<Pack>> {
    let (ncas, tks) = collect(FileSource::open(path)?, keys)?;
    let map: HashMap<String, Src> = ncas.iter().cloned().collect();
    let mut packs = Vec::new();
    for (_, src) in &ncas {
        let n = match Nca::open(src.clone(), keys, &tks) {
            Ok(n) => n,
            Err(Error::Unsupported(_)) => continue,
            Err(e) => return Err(e),
        };
        if n.content_type != ContentType::Meta {
            continue;
        }
        for sec in n.sections.iter().filter(|s| s.fs_type == FsType::Pfs0) {
            let fs = parse_pfs0(nca::pfs0_data(&n.section(sec.index)?, sec)?)?;
            for e in fs.entries.iter().filter(|e| e.name.ends_with(".cnmt")) {
                packs.push(Pack {
                    path: path.to_path_buf(),
                    meta: parse_cnmt(&fs.read(&e.name)?)?,
                    ncas: map.clone(),
                    tks: tks.clone(),
                });
            }
        }
    }
    Ok(packs)
}

pub fn version_label(v: u32) -> String {
    format!("v{v} (release {})", v >> 16)
}

#[derive(Debug, Clone)]
pub enum Choice {
    All,
    None,
    Latest,
    Only(Vec<u32>),
}

pub struct BuildPlan {
    pub packs: Vec<Pack>,
    pub base: Option<usize>,
    pub updates: Vec<usize>, // oldest -> newest
    pub dlcs: Vec<usize>,
    pub warnings: Vec<String>,
}

impl BuildPlan {
    pub fn from_packs(packs: Vec<Pack>) -> Self {
        let mut warnings = Vec::new();
        let (mut base, mut updates, mut dlcs) = (None::<usize>, Vec::new(), Vec::new());
        let mut seen = std::collections::HashSet::new();
        for (i, p) in packs.iter().enumerate() {
            if !seen.insert((p.meta.title_id, p.meta.version)) {
                warnings.push(format!(
                    "duplicate pack {:016X} {} ignored",
                    p.meta.title_id,
                    version_label(p.meta.version)
                ));
                continue;
            }
            match p.meta.kind {
                TitleKind::Application if base.is_none() => base = Some(i),
                TitleKind::Application => {
                    warnings.push(format!("second base game {:016X} ignored", p.meta.title_id))
                }
                TitleKind::Patch => updates.push(i),
                TitleKind::AddOn => dlcs.push(i),
                _ => warnings.push(format!(
                    "unsupported content type for {:016X} ignored",
                    p.meta.title_id
                )),
            }
        }
        match base {
            Some(b) => {
                let app = packs[b].meta.title_id;
                let (mut u2, mut d2) = (Vec::new(), Vec::new());
                for (list, out, what) in [(updates, &mut u2, "update"), (dlcs, &mut d2, "DLC")] {
                    for i in list {
                        if packs[i].meta.application_id == app {
                            out.push(i);
                        } else {
                            warnings.push(format!(
                                "{what} {:016X} belongs to another game and was ignored",
                                packs[i].meta.title_id
                            ));
                        }
                    }
                }
                updates = u2;
                dlcs = d2;
            }
            None => warnings.push("no base game found among the supplied files".into()),
        }
        updates.sort_by_key(|&i| packs[i].meta.version);
        dlcs.sort_by_key(|&i| (packs[i].meta.title_id, packs[i].meta.version));
        Self {
            packs,
            base,
            updates,
            dlcs,
            warnings,
        }
    }

    pub fn pick_updates(&self, c: &Choice) -> Vec<usize> {
        match c {
            Choice::All => self.updates.clone(),
            Choice::None => vec![],
            Choice::Latest => self.updates.last().copied().into_iter().collect(),
            Choice::Only(v) => self
                .updates
                .iter()
                .copied()
                .filter(|&i| v.contains(&self.packs[i].meta.version))
                .collect(),
        }
    }

    pub fn pick_dlcs(&self, c: &DlcChoice) -> Vec<usize> {
        match c {
            DlcChoice::All => self.dlcs.clone(),
            DlcChoice::None => vec![],
            DlcChoice::Only(ids) => self
                .dlcs
                .iter()
                .copied()
                .filter(|&i| ids.contains(&self.packs[i].meta.title_id))
                .collect(),
        }
    }
}

#[derive(Debug, Clone)]
pub enum DlcChoice {
    All,
    None,
    Only(Vec<u64>),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack(kind: TitleKind, title: u64, app: u64, version: u32) -> Pack {
        let meta = Cnmt {
            title_id: title,
            version,
            kind,
            application_id: app,
            required_system_version: 0,
            required_application_version: 0,
            contents: vec![],
        };
        Pack {
            path: "x".into(),
            meta,
            ncas: HashMap::new(),
            tks: TitleKeys::new(),
        }
    }

    #[test]
    fn groups_sorts_and_filters_foreign_packs() {
        let (app, other) = (0x0100_0000_0000_0000u64, 0x0100_0000_0001_0000u64);
        let plan = BuildPlan::from_packs(vec![
            pack(TitleKind::Patch, app + 0x800, app, 131072),
            pack(TitleKind::Application, app, app, 0),
            pack(TitleKind::Patch, app + 0x800, app, 65536),
            pack(TitleKind::AddOn, app + 0x1001, app, 0),
            pack(TitleKind::AddOn, other + 0x1001, other, 0),
            pack(TitleKind::Patch, app + 0x800, app, 65536), // duplicate
        ]);
        assert_eq!(plan.base, Some(1));
        let vers: Vec<u32> = plan
            .updates
            .iter()
            .map(|&i| plan.packs[i].meta.version)
            .collect();
        assert_eq!(vers, [65536, 131072]);
        assert_eq!(plan.dlcs.len(), 1);
        assert_eq!(plan.warnings.len(), 2);
        assert_eq!(plan.pick_updates(&Choice::Latest).len(), 1);
        assert_eq!(
            plan.packs[plan.pick_updates(&Choice::Latest)[0]]
                .meta
                .version,
            131072
        );
        assert_eq!(plan.pick_updates(&Choice::Only(vec![65536])).len(), 1);
        assert!(plan.pick_dlcs(&DlcChoice::None).is_empty());
    }

    #[test]
    fn no_base_warns() {
        let plan = BuildPlan::from_packs(vec![pack(TitleKind::Patch, 0x800, 0, 1)]);
        assert!(plan.base.is_none() && !plan.warnings.is_empty());
    }
}
