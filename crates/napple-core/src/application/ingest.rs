//! Use case: open a user-supplied game container and surface the Program (ExeFS) and data NCAs.
use crate::domain::error::{Error, Result};
use crate::domain::keys::KeySet;
use crate::infrastructure::io::{FileSource, Src};
use crate::infrastructure::nca::{self, ContentType, FsType, Nca, TitleKeys};
use crate::infrastructure::pfs::{parse_pfs0, Fs};
use crate::infrastructure::romfs::RomFs;
use crate::infrastructure::{ticket::parse_ticket, xci::parse_xci};
use std::path::Path;

pub struct NcaInfo {
    pub name: String,
    pub content_type: ContentType,
    pub program_id: u64,
}

pub struct Ingested {
    pub ncas: Vec<NcaInfo>,
    /// ExeFS files of the Program NCA (main, rtld, sdk, subsdk*, main.npdm).
    pub exefs: Vec<(String, Vec<u8>)>,
    pub romfs: Option<RomFs>,
    pub title_id: Option<u64>,
    /// Set when the RomFS could not be opened yet (e.g. BKTR update patch awaiting milestone 1b).
    pub romfs_note: Option<String>,
}

/// Collects NCAs and tickets from NSP (PFS0) or XCI (HFS0) containers.
pub(crate) fn collect(src: Src) -> Result<(Vec<(String, Src)>, TitleKeys)> {
    let mut parts: Vec<Fs> = Vec::new();
    let magic = crate::infrastructure::io::read_vec(&*src, 0, 4.min(src.size() as usize))?;
    if magic == b"PFS0" {
        parts.push(parse_pfs0(src)?);
    } else {
        // XCI: the secure partition holds the game; update/normal are read too if present.
        parts.extend(parse_xci(src)?.into_iter().filter(|(n, _)| n != "logo").map(|(_, f)| f));
    }
    let (mut ncas, mut tks) = (Vec::new(), TitleKeys::new());
    for fs in &parts {
        for e in &fs.entries {
            if e.name.ends_with(".nca") {
                ncas.push((e.name.clone(), fs.open(&e.name)?));
            } else if e.name.ends_with(".tik") {
                let t = parse_ticket(&fs.read(&e.name)?)?;
                if t.key_type == 0 {
                    tks.insert(t.rights_id, t.encrypted_title_key);
                }
            }
        }
    }
    Ok((ncas, tks))
}

pub fn ingest(path: &Path, keys: &KeySet) -> Result<Ingested> {
    let (ncas, tks) = collect(FileSource::open(path)?)?;
    ingest_sources(ncas, tks, keys)
}

pub fn ingest_sources(ncas: Vec<(String, Src)>, tks: TitleKeys, keys: &KeySet) -> Result<Ingested> {
    let (mut infos, mut exefs, mut romfs, mut title_id, mut romfs_note) = (Vec::new(), Vec::new(), None, None, None);
    for (name, src) in ncas {
        let n = match Nca::open(src, keys, &tks) {
            Ok(n) => n,
            // Meta NCAs / other generations we cannot open must not abort the whole ingest,
            // but a missing key is the user's problem and is reported.
            Err(e @ (Error::MissingKey(_) | Error::KeyFile(_))) => return Err(e),
            Err(Error::Unsupported(_)) => continue,
            Err(e) => return Err(e),
        };
        infos.push(NcaInfo { name: name.clone(), content_type: n.content_type, program_id: n.program_id });
        if n.content_type != ContentType::Program || !exefs.is_empty() {
            continue;
        }
        title_id = Some(n.program_id);
        for sec in &n.sections {
            match sec.fs_type {
                FsType::Pfs0 if exefs.is_empty() => {
                    let s = n.section(sec.index)?;
                    let fs = parse_pfs0(nca::pfs0_data(&s, sec)?)?;
                    for e in &fs.entries {
                        exefs.push((e.name.clone(), fs.read(&e.name)?));
                    }
                }
                FsType::RomFs if romfs.is_none() => {
                    match n.section(sec.index) {
                        Ok(s) => romfs = Some(RomFs::parse(nca::romfs_data(&s, sec)?)?),
                        Err(Error::Unsupported(m)) => romfs_note = Some(m),
                        Err(e) => return Err(e),
                    }
                }
                _ => {}
            }
        }
    }
    if exefs.is_empty() {
        return Err(Error::Container("no Program NCA with ExeFS found (update-only or unsupported input?)".into()));
    }
    Ok(Ingested { ncas: infos, exefs, romfs, title_id, romfs_note })
}
