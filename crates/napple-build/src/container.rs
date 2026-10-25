//! Raw access to the NCAs and tickets of a user-supplied container (NSP / XCI).
//! napple-core's `Pack` keeps these private; packaging needs the raw sections (BKTR) and the Control/Data NCAs.
use crate::error::{BuildError, Result};
use napple_core::infrastructure::io::{read_vec, FileSource, Src};
use napple_core::infrastructure::nca::TitleKeys;
use napple_core::infrastructure::ncz;
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::{ticket::parse_ticket, xci::parse_xci};
use std::collections::HashMap;
use std::path::Path;

pub struct Container {
    pub ncas: HashMap<String, Src>,
    pub tks: TitleKeys,
}

impl Container {
    pub fn load(path: &Path) -> Result<Self> {
        let src = FileSource::open(path).map_err(|e| BuildError::new("input", format!("{}: {e}", path.display())))?;
        let magic = read_vec(&*src, 0, 4.min(src.size() as usize))?;
        let mut parts = Vec::new();
        if magic == b"PFS0" {
            parts.push(parse_pfs0(src)?);
        } else {
            parts.extend(parse_xci(src)?.into_iter().filter(|(n, _)| n != "logo").map(|(_, f)| f));
        }
        let (mut ncas, mut tks) = (HashMap::new(), TitleKeys::new());
        for fs in &parts {
            for en in &fs.entries {
                if en.name.ends_with(".nca") {
                    ncas.insert(en.name.clone(), fs.open(&en.name)?);
                } else if en.name.ends_with(".ncz") {
                    ncas.insert(ncz::nca_name(&en.name), ncz::NczSource::open(fs.open(&en.name)?)?);
                } else if en.name.ends_with(".tik") {
                    let t = parse_ticket(&fs.read(&en.name)?)?;
                    if t.key_type == 0 {
                        tks.insert(t.rights_id, t.encrypted_title_key);
                    }
                }
            }
        }
        Ok(Self { ncas, tks })
    }
}

/// Caches one `Container` per input file.
#[derive(Default)]
pub struct Containers(HashMap<std::path::PathBuf, std::rc::Rc<Container>>);

impl Containers {
    pub fn get(&mut self, path: &Path) -> Result<std::rc::Rc<Container>> {
        if let Some(c) = self.0.get(path) {
            return Ok(c.clone());
        }
        let c = std::rc::Rc::new(Container::load(path)?);
        self.0.insert(path.to_path_buf(), c.clone());
        Ok(c)
    }
}
