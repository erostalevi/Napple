//! Raw access to the NCAs and tickets of a user-supplied container (NSP / XCI).
//! napple-core's `Pack` keeps these private; packaging needs the raw sections (BKTR) and the Control/Data NCAs.
use crate::error::{BuildError, Result};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{FileSource, Src, read_vec};
use napple_core::infrastructure::nca::TitleKeys;
use napple_core::infrastructure::ncz;
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::{
    ticket::{encrypted_title_key, parse_ticket},
    xci::parse_xci,
};
use std::collections::HashMap;
use std::path::Path;

pub struct Container {
    pub ncas: HashMap<String, Src>,
    pub tks: TitleKeys,
}

impl Container {
    /// Common tickets only. Personalized (RSA) tickets are skipped; use [`Container::load_with_keys`].
    pub fn load(path: &Path) -> Result<Self> {
        Self::load_with_keys(path, None)
    }

    /// With the user's keys, personalized tickets are resolved too (their title key is RSA-OAEP
    /// encrypted to the console's `eticket_rsa_keypair`); without that key they are skipped and the
    /// NCA then reports the missing ticket.
    pub fn load_with_keys(path: &Path, keys: Option<&KeySet>) -> Result<Self> {
        let src = FileSource::open(path)
            .map_err(|e| BuildError::new("input", format!("{}: {e}", path.display())))?;
        let magic = read_vec(&*src, 0, 4.min(src.size() as usize))?;
        let mut parts = Vec::new();
        if magic == b"PFS0" {
            parts.push(parse_pfs0(src)?);
        } else {
            parts.extend(
                parse_xci(src)?
                    .into_iter()
                    .filter(|(n, _)| n != "logo")
                    .map(|(_, f)| f),
            );
        }
        let (mut ncas, mut tks) = (HashMap::new(), TitleKeys::new());
        for fs in &parts {
            for en in &fs.entries {
                if en.name.ends_with(".nca") {
                    ncas.insert(en.name.clone(), fs.open(&en.name)?);
                } else if en.name.ends_with(".ncz") {
                    ncas.insert(
                        ncz::nca_name(&en.name),
                        ncz::NczSource::open(fs.open(&en.name)?)?,
                    );
                } else if en.name.ends_with(".tik") {
                    let t = parse_ticket(&fs.read(&en.name)?)?;
                    let key = match (t.key_type, keys) {
                        (0, _) => Some(t.encrypted_title_key),
                        (_, Some(k)) => encrypted_title_key(&t, k)?,
                        _ => None,
                    };
                    if let Some(k) = key {
                        tks.insert(t.rights_id, k);
                    }
                }
            }
        }
        Ok(Self { ncas, tks })
    }
}

/// Caches one `Container` per input file. `Containers::default()` resolves common tickets only;
/// `Containers::with_keys` also resolves personalized ones.
#[derive(Default)]
pub struct Containers<'k> {
    cache: HashMap<std::path::PathBuf, std::rc::Rc<Container>>,
    keys: Option<&'k KeySet>,
}

impl<'k> Containers<'k> {
    pub fn with_keys(keys: &'k KeySet) -> Self {
        Self {
            cache: HashMap::new(),
            keys: Some(keys),
        }
    }

    pub fn get(&mut self, path: &Path) -> Result<std::rc::Rc<Container>> {
        if let Some(c) = self.cache.get(path) {
            return Ok(c.clone());
        }
        let c = std::rc::Rc::new(Container::load_with_keys(path, self.keys)?);
        self.cache.insert(path.to_path_buf(), c.clone());
        Ok(c)
    }
}
