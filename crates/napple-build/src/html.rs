//! Document NCAs whose RomFS the runtime's offline web applet serves: HtmlDocument (CNMT content type 4: manual / offline HTML) is
//! written to `<version dir>/html/`, LegalInformation (content type 5: the title's legal / IP notices, WebDocumentKind
//! ApplicationLegalInformation) to `<version dir>/legal/`, both next to `romfs.map`.
use crate::container::Container;
use crate::error::{BuildError, Result};
use napple_core::application::package::Pack;
use napple_core::domain::error::Error as CoreError;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::read_vec;
use napple_core::infrastructure::nca::{self, FsType, Nca};
use napple_core::infrastructure::romfs::RomFs;
use std::path::Path;

/// Content type of the HtmlDocument NCA in a CNMT record.
pub const CTYPE_HTML: u8 = 4;
/// Content type of the LegalInformation NCA in a CNMT record.
pub const CTYPE_LEGAL: u8 = 5;
const MAX_FILE: u64 = 256 << 20;
const MAX_TOTAL: u64 = 1 << 30;
const MAX_FILES: usize = 200_000;

/// Relative path made of plain components only (no `..`, no absolute paths, no empty parts).
pub fn safe_rel(p: &str) -> Option<String> {
    let parts: Vec<&str> = p
        .split('/')
        .filter(|c| !c.is_empty() && *c != ".")
        .collect();
    if parts.is_empty()
        || parts
            .iter()
            .any(|c| *c == ".." || c.contains('\\') || c.contains('\0'))
    {
        return None;
    }
    Some(parts.join("/"))
}

/// Relative path and contents of each HtmlDocument file.
pub type HtmlFiles = Vec<(String, Vec<u8>)>;

/// Reads every file of the pack's HtmlDocument RomFS (`None` when the pack has none or it cannot be decrypted).
pub fn read_html(cont: &Container, pack: &Pack, keys: &KeySet) -> Result<Option<HtmlFiles>> {
    read_document(cont, pack, keys, CTYPE_HTML)
}

/// Reads every file of the pack's LegalInformation RomFS (`None` when the pack has none or it cannot be decrypted).
pub fn read_legal(cont: &Container, pack: &Pack, keys: &KeySet) -> Result<Option<HtmlFiles>> {
    read_document(cont, pack, keys, CTYPE_LEGAL)
}

/// Every file of the RomFS of the pack's first content of type `ctype` (`None` when there is none or it cannot be decrypted).
pub fn read_document(cont: &Container, pack: &Pack, keys: &KeySet, ctype: u8) -> Result<Option<HtmlFiles>> {
    let Some(rec) = pack.meta.contents.iter().find(|c| c.ctype == ctype) else {
        return Ok(None);
    };
    let Some(src) = cont.ncas.get(&rec.nca_name()) else {
        return Ok(None);
    };
    let n = match Nca::open(src.clone(), keys, &cont.tks) {
        Ok(n) => n,
        Err(CoreError::Unsupported(_)) => return Ok(None),
        Err(e) => return Err(e.into()),
    };
    for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs) {
        let Ok(s) = n.section(sec.index) else {
            continue;
        };
        let fs = RomFs::parse(nca::romfs_data(&s, sec)?)?;
        let mut out = Vec::new();
        let mut total = 0u64;
        for f in fs.files.iter().take(MAX_FILES) {
            let Some(rel) = safe_rel(&f.path) else {
                continue;
            };
            total = total.saturating_add(f.size);
            if f.size > MAX_FILE || total > MAX_TOTAL {
                continue;
            }
            out.push((rel, read_vec(&*fs.open(f)?, 0, f.size as usize)?));
        }
        if !out.is_empty() {
            return Ok(Some(out));
        }
    }
    Ok(None)
}

/// Writes the files under `dir/html/`.
pub fn write_html(dir: &Path, files: &[(String, Vec<u8>)]) -> Result<()> {
    write_document(dir, "html", files)
}

/// Writes the files under `dir/legal/`.
pub fn write_legal(dir: &Path, files: &[(String, Vec<u8>)]) -> Result<()> {
    write_document(dir, "legal", files)
}

/// Writes the files under `dir/<sub>/` (paths already confined by `safe_rel`).
pub fn write_document(dir: &Path, sub: &str, files: &[(String, Vec<u8>)]) -> Result<()> {
    for (rel, data) in files {
        let dst = dir.join(sub).join(rel);
        if let Some(parent) = dst.parent() {
            std::fs::create_dir_all(parent).map_err(|e| BuildError::io("create document dir", e))?;
        }
        std::fs::write(&dst, data).map_err(|e| BuildError::io("write document file", e))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rel_paths_are_confined() {
        assert_eq!(safe_rel("/a//b/./c.html").as_deref(), Some("a/b/c.html"));
        assert_eq!(safe_rel("../x"), None);
        assert_eq!(safe_rel("a/../../x"), None);
        assert_eq!(safe_rel("a\\b"), None);
        assert_eq!(safe_rel("/"), None);
    }

    #[test]
    fn write_html_creates_nested_dirs_under_html() {
        let dir = std::env::temp_dir().join(format!("napple-html-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let files = vec![
            ("index.html".to_string(), b"i".to_vec()),
            ("img/ui/a.png".to_string(), b"p".to_vec()),
        ];
        write_html(&dir, &files).unwrap();
        assert_eq!(std::fs::read(dir.join("html/img/ui/a.png")).unwrap(), b"p");
        assert_eq!(std::fs::read(dir.join("html/index.html")).unwrap(), b"i");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn write_legal_goes_under_legal() {
        let dir = std::env::temp_dir().join(format!("napple-legal-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let files = vec![("ipnotices.htdocs/index.html".to_string(), b"n".to_vec())];
        write_legal(&dir, &files).unwrap();
        assert_eq!(std::fs::read(dir.join("legal/ipnotices.htdocs/index.html")).unwrap(), b"n");
        assert!(!dir.join("html").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
