//! What a cloud save version contains, and how it is put back on disk.
//!
//! A title's host save tree is `<save root>/<TITLE>/save` (runtime services/host_info.c host_save_dir); fs.c keeps each save as a live
//! directory plus `<dir>.committed` (the last committed snapshot, what a crash rolls back to), `<dir>.napple-journal` and `<dir>.napple-meta`.
//! A version holds the committed snapshots (the live directory when a save has none yet) of:
//! - `account/`: the user's account save, `space1/account/<first 4 uid bytes, hex>` (fs.c save_root)
//! - `device/`:  the title's device save, `space1/device` (not per user; titles such as ACNH keep their data there)
//! Installing a version replaces both the snapshot and the live directory of each component it contains, before the game opens them:
//! fs.c's journal recovery at the first open then finds a consistent state.
use crate::tarball::{self, Entry};
use crate::{Error, Result};
use std::fs;
use std::path::{Path, PathBuf};

pub fn account_root(save_dir: &Path, uid_hex: &str) -> PathBuf {
    save_dir.join("space1/account").join(&uid_hex[..8.min(uid_hex.len())])
}

pub fn device_root(save_dir: &Path) -> PathBuf {
    save_dir.join("space1/device")
}

fn with_suffix(p: &Path, suffix: &str) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(suffix);
    PathBuf::from(s)
}

pub fn components(save_dir: &Path, uid_hex: &str) -> [(&'static str, PathBuf); 2] {
    [("account", account_root(save_dir, uid_hex)), ("device", device_root(save_dir))]
}

/// The committed state of a save directory (the live directory when nothing was committed through the journal yet).
fn snapshot(root: &Path) -> Option<PathBuf> {
    let c = with_suffix(root, ".committed");
    if c.is_dir() {
        Some(c)
    } else if root.is_dir() && !with_suffix(root, ".napple-journal").exists() {
        Some(root.to_path_buf())
    } else {
        None
    }
}

/// The deterministic tar of this user's save for the title, or None when the title has no save on this device yet.
pub fn local_tar(save_dir: &Path, uid_hex: &str) -> Result<Option<Vec<u8>>> {
    let mut entries = Vec::new();
    for (prefix, root) in components(save_dir, uid_hex) {
        if let Some(src) = snapshot(&root) {
            tarball::collect(&src, prefix, &mut entries)?;
        }
    }
    if entries.is_empty() { Ok(None) } else { Ok(Some(tarball::build(entries)?)) }
}

fn copy_tree(src: &Path, dst: &Path) -> Result<()> {
    fs::create_dir_all(dst)?;
    for e in fs::read_dir(src)? {
        let e = e?;
        let ft = e.file_type()?;
        let to = dst.join(e.file_name());
        if ft.is_dir() {
            copy_tree(&e.path(), &to)?;
        } else if ft.is_file() {
            fs::copy(e.path(), &to)?;
        }
    }
    Ok(())
}

fn rm(p: &Path) {
    if p.is_dir() {
        let _ = fs::remove_dir_all(p);
    } else {
        let _ = fs::remove_file(p);
    }
}

/// Puts the components present in `entries` in place (both the committed snapshot and the live directory). Each component is extracted
/// beside its directory first, so a failure leaves the previous save untouched.
pub fn install(save_dir: &Path, uid_hex: &str, entries: &[Entry]) -> Result<()> {
    for (prefix, root) in components(save_dir, uid_hex) {
        let lead = format!("{prefix}/");
        if !entries.iter().any(|e| e.path.starts_with(&lead)) {
            continue;
        }
        let parent = root.parent().ok_or_else(|| Error::Io("save path has no parent".into()))?;
        fs::create_dir_all(parent)?;
        let incoming = with_suffix(&root, ".cloud-incoming");
        let old = with_suffix(&root, ".cloud-old");
        let committed = with_suffix(&root, ".committed");
        rm(&incoming);
        rm(&old);
        tarball::extract(entries, prefix, &incoming)?;
        if committed.exists() {
            fs::rename(&committed, &old)?;
        }
        if let Err(e) = fs::rename(&incoming, &committed) {
            let _ = fs::rename(&old, &committed);
            return Err(e.into());
        }
        rm(&root);
        copy_tree(&committed, &root)?;
        fs::write(with_suffix(&root, ".napple-journal"), b"")?;
        rm(&old);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_tar_and_install() {
        let d = std::env::temp_dir().join(format!("napple-cloud-savedir-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let uid = "4e6170706c6555736572000000000001";
        assert!(local_tar(&d, uid).unwrap().is_none());
        // a journaled account save: the committed snapshot counts, not uncommitted live changes
        let acc = account_root(&d, uid);
        fs::create_dir_all(&acc).unwrap();
        fs::write(acc.join("progress.bin"), b"uncommitted").unwrap();
        fs::create_dir_all(with_suffix(&acc, ".committed")).unwrap();
        fs::write(with_suffix(&acc, ".committed").join("progress.bin"), b"committed").unwrap();
        fs::write(with_suffix(&acc, ".napple-journal"), b"").unwrap();
        let t = local_tar(&d, uid).unwrap().unwrap();
        let e = tarball::parse(&t).unwrap();
        assert_eq!(e.iter().find(|e| e.path == "account/progress.bin").unwrap().data.as_deref(), Some(&b"committed"[..]));
        // install a version with an account and a device component on a second "device"
        let d2 = d.join("other");
        let mut v = e.clone();
        v.push(Entry { path: "device/".into(), data: None });
        v.push(Entry { path: "device/island.dat".into(), data: Some(b"isle".to_vec()) });
        install(&d2, uid, &v).unwrap();
        let acc2 = account_root(&d2, uid);
        assert_eq!(fs::read(acc2.join("progress.bin")).unwrap(), b"committed");
        assert_eq!(fs::read(with_suffix(&acc2, ".committed").join("progress.bin")).unwrap(), b"committed");
        assert!(with_suffix(&acc2, ".napple-journal").exists());
        assert_eq!(fs::read(device_root(&d2).join("island.dat")).unwrap(), b"isle");
        let t2 = local_tar(&d2, uid).unwrap().unwrap();
        assert_eq!(tarball::parse(&t2).unwrap().len(), v.len());
        // installing over an existing save replaces it entirely
        let v2 = vec![Entry { path: "account/".into(), data: None }, Entry { path: "account/new.bin".into(), data: Some(vec![1]) }];
        install(&d2, uid, &v2).unwrap();
        assert!(!acc2.join("progress.bin").exists());
        assert!(acc2.join("new.bin").exists());
        assert!(device_root(&d2).join("island.dat").exists(), "components absent from the version are left alone");
        let _ = fs::remove_dir_all(&d);
    }
}
