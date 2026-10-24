//! Firmware archive access (zip or directory of NCAs): system titles and their data archives.
use crate::error::{BuildError, Result};
use crate::zipread::ZipArchive;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::cnmt::{parse_cnmt, Cnmt};
use napple_core::infrastructure::io::{FileSource, Src};
use napple_core::infrastructure::nca::{self, ContentType, FsType, Nca, TitleKeys};
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::romfs::RomFs;
use std::path::Path;

pub struct Firmware {
    ncas: Vec<(String, Src)>,
}

pub struct SystemTitle {
    pub meta: Cnmt,
}

impl Firmware {
    pub fn open(path: &Path) -> Result<Self> {
        let md = std::fs::metadata(path).map_err(|e| BuildError::new("firmware", format!("{}: {e}", path.display())))?;
        let mut ncas = Vec::new();
        if md.is_dir() {
            let mut stack = vec![(path.to_path_buf(), 0)];
            while let Some((d, depth)) = stack.pop() {
                for e in std::fs::read_dir(&d)?.flatten() {
                    let p = e.path();
                    let name = e.file_name().to_string_lossy().to_string();
                    if p.is_dir() && depth < 2 {
                        stack.push((p, depth + 1));
                    } else if name.ends_with(".nca") {
                        ncas.push((name, FileSource::open(&p)?));
                    }
                }
            }
        } else {
            let z = ZipArchive::open(path)?;
            for e in &z.entries {
                let base = e.name.rsplit('/').next().unwrap_or(&e.name);
                if base.ends_with(".nca") {
                    ncas.push((base.to_string(), z.open_entry(e)?));
                }
            }
        }
        if ncas.is_empty() {
            return Err(BuildError::new("firmware", format!("{}: no NCA files found (expected a firmware zip or a directory of .nca files)", path.display())));
        }
        ncas.sort_by(|a, b| a.0.cmp(&b.0));
        Ok(Self { ncas })
    }

    pub fn nca_count(&self) -> usize {
        self.ncas.len()
    }

    /// Content metas of every system title in the archive (ordered by title id, then version).
    pub fn titles(&self, keys: &KeySet) -> Result<Vec<SystemTitle>> {
        let tks = TitleKeys::new();
        let mut out = Vec::new();
        for (name, src) in self.ncas.iter().filter(|(n, _)| n.ends_with(".cnmt.nca")) {
            let n = match Nca::open(src.clone(), keys, &tks) {
                Ok(n) => n,
                Err(napple_core::domain::error::Error::Unsupported(_)) => continue,
                Err(e) => return Err(BuildError::new("firmware", format!("{name}: {e}"))),
            };
            if n.content_type != ContentType::Meta {
                continue;
            }
            for sec in n.sections.iter().filter(|s| s.fs_type == FsType::Pfs0) {
                let fs = parse_pfs0(nca::pfs0_data(&n.section(sec.index)?, sec)?)?;
                for e in fs.entries.iter().filter(|e| e.name.ends_with(".cnmt")) {
                    out.push(SystemTitle { meta: parse_cnmt(&fs.read(&e.name)?)? });
                }
            }
        }
        out.sort_by_key(|t| (t.meta.title_id, t.meta.version));
        Ok(out)
    }

    /// RomFS of the first Data content (type 2) of `title` that has one.
    pub fn data_romfs(&self, title: &SystemTitle, keys: &KeySet) -> Result<Option<RomFs>> {
        let tks = TitleKeys::new();
        for rec in title.meta.contents.iter().filter(|c| c.ctype == 2) {
            let Some((_, src)) = self.ncas.iter().find(|(n, _)| *n == rec.nca_name()) else { continue };
            let n = Nca::open(src.clone(), keys, &tks)?;
            for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs) {
                return Ok(Some(RomFs::parse(nca::romfs_data(&n.section(sec.index)?, sec)?)?));
            }
        }
        Ok(None)
    }
}
