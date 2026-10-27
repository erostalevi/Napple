//! Where a mod's files come from: a `.napmod` zip, or a directory (the authoring layout).
use crate::util::{ModError, Result, err, safe_rel_path};
use sha2::{Digest, Sha256};
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::io::{Read, Seek};
use std::path::{Path, PathBuf};

pub const MAX_ENTRIES: usize = 50_000;
pub const MAX_TOTAL_BYTES: u64 = 8 << 30;
pub const MAX_RATIO: u64 = 2000;

pub trait ModFiles {
    /// Reads one file, failing if it is larger than `limit` bytes.
    fn read(&self, name: &str, limit: u64) -> Result<Vec<u8>>;
}

/// Everything listed for a mod, plus the entries refused while listing.
pub struct Listing {
    /// Safe entries: path -> uncompressed size.
    pub entries: BTreeMap<String, u64>,
    /// Entries with unsafe names, symlinks, encryption, duplicates or absurd compression ratios.
    pub bad: Vec<String>,
}

pub struct DirFiles(pub PathBuf);

impl ModFiles for DirFiles {
    fn read(&self, name: &str, limit: u64) -> Result<Vec<u8>> {
        safe_rel_path(name)?;
        let f = std::fs::File::open(self.0.join(name))?;
        let mut v = Vec::new();
        f.take(limit + 1).read_to_end(&mut v)?;
        if v.len() as u64 > limit {
            return err(format!("{name}: larger than {limit} bytes"));
        }
        Ok(v)
    }
}

pub fn list_dir(root: &Path) -> Result<Listing> {
    let mut l = Listing {
        entries: BTreeMap::new(),
        bad: vec![],
    };
    fn walk(root: &Path, dir: &Path, l: &mut Listing) -> Result<()> {
        let mut names: Vec<_> = std::fs::read_dir(dir)?.collect::<std::io::Result<Vec<_>>>()?;
        names.sort_by_key(|e| e.file_name());
        for e in names {
            let ft = e.file_type()?;
            let rel = e
                .path()
                .strip_prefix(root)
                .map_err(|_| ModError("path".into()))?
                .to_string_lossy()
                .replace('\\', "/");
            if ft.is_symlink() {
                l.bad.push(format!("{rel}: symbolic link"));
            } else if ft.is_dir() {
                walk(root, &e.path(), l)?;
            } else if ft.is_file() {
                if let Err(x) = safe_rel_path(&rel) {
                    l.bad.push(x.to_string());
                } else if l.entries.len() >= MAX_ENTRIES {
                    return err("too many files");
                } else {
                    l.entries.insert(rel, e.metadata()?.len());
                }
            }
        }
        Ok(())
    }
    walk(root, root, &mut l)?;
    Ok(l)
}

pub struct ZipFiles<R: Read + Seek>(RefCell<zip::ZipArchive<R>>);

impl<R: Read + Seek> ZipFiles<R> {
    pub fn open(r: R) -> Result<(Self, Listing)> {
        let mut z = zip::ZipArchive::new(r)?;
        if z.len() > MAX_ENTRIES {
            return err(format!(
                "archive has {} entries (limit {MAX_ENTRIES})",
                z.len()
            ));
        }
        let mut l = Listing {
            entries: BTreeMap::new(),
            bad: vec![],
        };
        let mut total = 0u64;
        for i in 0..z.len() {
            let f = z.by_index_raw(i)?;
            let name = f.name().to_string();
            if f.is_dir() {
                continue;
            }
            if let Err(x) = safe_rel_path(&name) {
                l.bad.push(x.to_string());
                continue;
            }
            if f.encrypted() {
                l.bad.push(format!("{name}: encrypted entry"));
                continue;
            }
            if f.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
                l.bad.push(format!("{name}: symbolic link"));
                continue;
            }
            let (size, csize) = (f.size(), f.compressed_size());
            if size > 64 && size / csize.max(1) > MAX_RATIO {
                l.bad
                    .push(format!("{name}: compression ratio above {MAX_RATIO}:1"));
                continue;
            }
            total = total.saturating_add(size);
            if l.entries.insert(name.clone(), size).is_some() {
                l.bad.push(format!("{name}: duplicate entry"));
            }
        }
        if total > MAX_TOTAL_BYTES {
            return err("archive expands to more than 8 GiB");
        }
        Ok((Self(RefCell::new(z)), l))
    }
}

impl<R: Read + Seek> ModFiles for ZipFiles<R> {
    fn read(&self, name: &str, limit: u64) -> Result<Vec<u8>> {
        safe_rel_path(name)?;
        let mut z = self.0.borrow_mut();
        let f = z.by_name(name)?;
        let mut v = Vec::new();
        f.take(limit + 1).read_to_end(&mut v)?;
        if v.len() as u64 > limit {
            return err(format!("{name}: larger than {limit} bytes"));
        }
        Ok(v)
    }
}

/// SHA-256 of a whole file, streamed.
pub fn hash_file(p: &Path) -> Result<String> {
    let mut f = std::fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(crate::util::hex_encode(&h.finalize()))
}
