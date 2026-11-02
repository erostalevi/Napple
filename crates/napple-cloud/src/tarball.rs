//! Deterministic ustar archives of save directories.
//!
//! Same directory contents -> same bytes: entries sorted by path (byte order), mode 0755 (dirs) / 0644 (files), uid/gid 0, mtime 0, empty
//! user/group names, no pax/GNU extensions. Only directories and regular files are archived (a save tree has nothing else; symlinks and
//! special files are skipped). The reader accepts only that subset and rejects absolute paths and `..` so a hostile blob cannot write
//! outside the directory it is extracted into.
use crate::{Error, Result};
use std::fs;
use std::path::Path;

const BLOCK: usize = 512;
/// Upper bound for an archive's total file data (a save version is at most 64 MiB of ciphertext; saves compress well, so allow 4x).
pub const MAX_TOTAL: u64 = 256 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// Relative path with `/` separators; directories end with `/`.
    pub path: String,
    /// None for a directory.
    pub data: Option<Vec<u8>>,
}

/// Adds `dir`'s contents under `prefix/` (the prefix directory itself included).
pub fn collect(dir: &Path, prefix: &str, out: &mut Vec<Entry>) -> Result<()> {
    out.push(Entry { path: format!("{prefix}/"), data: None });
    walk(dir, prefix, out)
}

fn walk(dir: &Path, rel: &str, out: &mut Vec<Entry>) -> Result<()> {
    for e in fs::read_dir(dir)? {
        let e = e?;
        let name = e.file_name();
        let Some(name) = name.to_str() else { continue };
        let ft = e.file_type()?;
        let path = format!("{rel}/{name}");
        if ft.is_dir() {
            out.push(Entry { path: format!("{path}/"), data: None });
            walk(&e.path(), &path, out)?;
        } else if ft.is_file() {
            out.push(Entry { path, data: Some(fs::read(e.path())?) });
        }
    }
    Ok(())
}

fn octal(field: &mut [u8], v: u64) {
    let n = field.len() - 1;
    let s = format!("{:0w$o}", v, w = n);
    field[..n].copy_from_slice(s.as_bytes());
    field[n] = 0;
}

fn split_name(path: &str) -> Result<(&str, &str)> {
    if path.len() <= 100 {
        return Ok(("", path));
    }
    let b = path.as_bytes();
    // the last '/' that leaves a name of at most 100 bytes and a prefix of at most 155 (a trailing '/' of a directory stays in the name)
    for i in (0..b.len().saturating_sub(1)).rev() {
        if b[i] == b'/' && b.len() - i - 1 <= 100 && i <= 155 {
            return Ok((&path[..i], &path[i + 1..]));
        }
    }
    Err(Error::Format(format!("path too long for ustar: {path}")))
}

fn header(path: &str, size: u64, dir: bool) -> Result<[u8; BLOCK]> {
    let mut h = [0u8; BLOCK];
    let (prefix, name) = split_name(path)?;
    h[..name.len()].copy_from_slice(name.as_bytes());
    octal(&mut h[100..108], if dir { 0o755 } else { 0o644 });
    octal(&mut h[108..116], 0);
    octal(&mut h[116..124], 0);
    octal(&mut h[124..136], size);
    octal(&mut h[136..148], 0);
    h[156] = if dir { b'5' } else { b'0' };
    h[257..263].copy_from_slice(b"ustar\0");
    h[263..265].copy_from_slice(b"00");
    h[345..345 + prefix.len()].copy_from_slice(prefix.as_bytes());
    h[148..156].copy_from_slice(b"        ");
    let sum: u32 = h.iter().map(|&b| b as u32).sum();
    let s = format!("{sum:06o}\0 ");
    h[148..156].copy_from_slice(s.as_bytes());
    Ok(h)
}

/// The archive bytes for `entries` (sorted here; duplicates are an error).
pub fn build(mut entries: Vec<Entry>) -> Result<Vec<u8>> {
    entries.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    if entries.windows(2).any(|w| w[0].path == w[1].path) {
        return Err(Error::Format("duplicate path in archive".into()));
    }
    let mut out = Vec::new();
    for e in &entries {
        check_path(&e.path)?;
        let size = e.data.as_ref().map_or(0, |d| d.len() as u64);
        out.extend_from_slice(&header(&e.path, size, e.data.is_none())?);
        if let Some(d) = &e.data {
            out.extend_from_slice(d);
            out.resize(out.len().div_ceil(BLOCK) * BLOCK, 0);
        }
    }
    out.resize(out.len() + 2 * BLOCK, 0);
    Ok(out)
}

fn check_path(p: &str) -> Result<()> {
    let body = p.strip_suffix('/').unwrap_or(p);
    let bad = body.is_empty()
        || body.starts_with('/')
        || body.contains('\\')
        || body.contains('\0')
        || body.split('/').any(|c| c.is_empty() || c == "." || c == "..");
    if bad { Err(Error::Format(format!("unsafe path in archive: {p:?}"))) } else { Ok(()) }
}

fn field_str(f: &[u8]) -> Result<&str> {
    let end = f.iter().position(|&b| b == 0).unwrap_or(f.len());
    std::str::from_utf8(&f[..end]).map_err(|_| Error::Format("non-UTF-8 name in archive".into()))
}

fn field_octal(f: &[u8]) -> Result<u64> {
    let s = field_str(f)?.trim_matches(|c| c == ' ' || c == '\0');
    if s.is_empty() {
        return Ok(0);
    }
    u64::from_str_radix(s, 8).map_err(|_| Error::Format("bad octal field in archive".into()))
}

/// Parses an archive written by `build` (or any plain ustar with only files and directories).
pub fn parse(data: &[u8]) -> Result<Vec<Entry>> {
    let mut out = Vec::new();
    let mut off = 0usize;
    let mut total = 0u64;
    loop {
        if off + BLOCK > data.len() {
            return Err(Error::Format("truncated archive".into()));
        }
        let h = &data[off..off + BLOCK];
        if h.iter().all(|&b| b == 0) {
            break;
        }
        let stored = field_octal(&h[148..156])?;
        let sum: u64 = h.iter().enumerate().map(|(i, &b)| if (148..156).contains(&i) { 32 } else { b as u64 }).sum();
        if sum != stored {
            return Err(Error::Format("archive header checksum mismatch".into()));
        }
        if &h[257..262] != b"ustar" {
            return Err(Error::Format("not a ustar archive".into()));
        }
        let name = field_str(&h[0..100])?;
        let prefix = field_str(&h[345..500])?;
        let mut path = if prefix.is_empty() { name.to_string() } else { format!("{prefix}/{name}") };
        let size = field_octal(&h[124..136])?;
        let kind = h[156];
        off += BLOCK;
        match kind {
            b'5' => {
                if !path.ends_with('/') {
                    path.push('/');
                }
                check_path(&path)?;
                out.push(Entry { path, data: None });
            }
            b'0' | 0 => {
                check_path(&path)?;
                if path.ends_with('/') {
                    return Err(Error::Format("file entry with a directory name".into()));
                }
                total += size;
                if total > MAX_TOTAL {
                    return Err(Error::Format("archive too large".into()));
                }
                let n = size as usize;
                if off + n > data.len() {
                    return Err(Error::Format("truncated archive data".into()));
                }
                out.push(Entry { path, data: Some(data[off..off + n].to_vec()) });
                off += n.div_ceil(BLOCK) * BLOCK;
            }
            other => return Err(Error::Format(format!("unsupported archive entry type {:?}", other as char))),
        }
    }
    Ok(out)
}

/// Writes the entries under `prefix/` into `dest` (created; must not exist yet or be empty). Other prefixes are ignored.
pub fn extract(entries: &[Entry], prefix: &str, dest: &Path) -> Result<bool> {
    let lead = format!("{prefix}/");
    let mut any = false;
    fs::create_dir_all(dest)?;
    for e in entries {
        let Some(rel) = e.path.strip_prefix(&lead) else { continue };
        any = true;
        if rel.is_empty() {
            continue;
        }
        let p = dest.join(rel.trim_end_matches('/'));
        match &e.data {
            None => fs::create_dir_all(&p)?,
            Some(d) => {
                if let Some(parent) = p.parent() {
                    fs::create_dir_all(parent)?;
                }
                fs::write(&p, d)?;
            }
        }
    }
    Ok(any)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("napple-cloud-tar-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn deterministic_and_round_trip() {
        let a = tmp("a");
        fs::create_dir_all(a.join("sub/deeper")).unwrap();
        fs::write(a.join("zz.bin"), b"last").unwrap();
        fs::write(a.join("sub/deeper/f.dat"), vec![7u8; 1000]).unwrap();
        fs::write(a.join("a.txt"), b"").unwrap();
        let mut e1 = Vec::new();
        collect(&a, "account", &mut e1).unwrap();
        let t1 = build(e1.clone()).unwrap();
        // same contents written in another order, other times, other permissions -> same bytes
        let b = tmp("b");
        fs::write(b.join("a.txt"), b"").unwrap();
        fs::create_dir_all(b.join("sub/deeper")).unwrap();
        fs::write(b.join("sub/deeper/f.dat"), vec![7u8; 1000]).unwrap();
        fs::write(b.join("zz.bin"), b"last").unwrap();
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(b.join("zz.bin"), fs::Permissions::from_mode(0o600)).unwrap();
        let mut e2 = Vec::new();
        collect(&b, "account", &mut e2).unwrap();
        e2.reverse();
        let t2 = build(e2).unwrap();
        assert_eq!(t1, t2);
        assert_eq!(t1.len() % 512, 0);
        let parsed = parse(&t1).unwrap();
        let mut sorted = e1.clone();
        sorted.sort_by(|a, b| a.path.cmp(&b.path));
        assert_eq!(parsed, sorted);
        let c = tmp("c");
        assert!(extract(&parsed, "account", &c.join("out")).unwrap());
        assert_eq!(fs::read(c.join("out/sub/deeper/f.dat")).unwrap(), vec![7u8; 1000]);
        assert_eq!(fs::read(c.join("out/zz.bin")).unwrap(), b"last");
        assert!(!extract(&parsed, "device", &c.join("dev")).unwrap());
        for d in [a, b, c] {
            let _ = fs::remove_dir_all(d);
        }
    }

    #[test]
    fn readable_by_system_tar() {
        let mut e = vec![Entry { path: "account/".into(), data: None }];
        let long = format!("account/{}/{}", "d".repeat(90), "f".repeat(60));
        e.push(Entry { path: format!("{}/", &long[..long.rfind('/').unwrap()]), data: None });
        e.push(Entry { path: long.clone(), data: Some(b"hello".to_vec()) });
        let t = build(e).unwrap();
        let d = tmp("sys");
        fs::write(d.join("x.tar"), &t).unwrap();
        let out = std::process::Command::new("tar").arg("-tf").arg(d.join("x.tar")).output();
        if let Ok(out) = out {
            let s = String::from_utf8_lossy(&out.stdout);
            assert!(s.contains(&long), "system tar listing: {s}");
        }
        assert_eq!(parse(&t).unwrap().last().unwrap().path, long);
        let _ = fs::remove_dir_all(d);
    }

    #[test]
    fn rejects_hostile_paths() {
        for p in ["/etc/passwd", "account/../../x", "account//x", "../x"] {
            let mut t = build(vec![Entry { path: "ok".into(), data: Some(vec![1]) }]).unwrap();
            // patch the name field and the checksum by hand
            let mut h = [0u8; 512];
            h.copy_from_slice(&t[..512]);
            h[..100].fill(0);
            h[..p.len()].copy_from_slice(p.as_bytes());
            h[148..156].copy_from_slice(b"        ");
            let sum: u32 = h.iter().map(|&b| b as u32).sum();
            h[148..156].copy_from_slice(format!("{sum:06o}\0 ").as_bytes());
            t[..512].copy_from_slice(&h);
            assert!(parse(&t).is_err(), "{p} accepted");
        }
        assert!(build(vec![Entry { path: "a/../b".into(), data: Some(vec![]) }]).is_err());
    }

    #[test]
    fn rejects_corruption() {
        let t = build(vec![Entry { path: "a".into(), data: Some(vec![1, 2, 3]) }]).unwrap();
        let mut bad = t.clone();
        bad[0] ^= 1;
        assert!(parse(&bad).is_err());
        assert!(parse(&t[..600]).is_err());
    }
}
