//! Progress lines, cancellation, free-space checks, APFS clones and temp-dir hygiene.
use crate::error::{BuildError, Result};
use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

static CANCELLED: AtomicBool = AtomicBool::new(false);
static LAST_PCT_X10: AtomicU64 = AtomicU64::new(0);

extern "C" fn on_signal(_: libc::c_int) {
    CANCELLED.store(true, Ordering::SeqCst);
}

/// SIGINT/SIGTERM set a flag that every long loop polls, so temp files are removed before exiting.
pub fn install_cancel_handlers() {
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
    }
}

pub fn check_cancel() -> Result<()> {
    if CANCELLED.load(Ordering::SeqCst) { Err(BuildError::cancelled()) } else { Ok(()) }
}

/// `PROGRESS <stage> <percent 0-100> <message>`; the percentage is the overall build progress and never goes backwards.
pub fn progress(stage: &str, pct: f64, msg: &str) {
    let x10 = (pct.clamp(0.0, 100.0) * 10.0).round() as u64;
    let x10 = LAST_PCT_X10.fetch_max(x10, Ordering::SeqCst).max(x10);
    let msg: String = msg.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    println!("PROGRESS {stage} {}.{} {msg}", x10 / 10, x10 % 10);
}

pub fn reset_progress() {
    LAST_PCT_X10.store(0, Ordering::SeqCst);
}

/// Loads prod.keys with a path-aware error (key values never appear in messages).
pub fn load_keys(path: &Path) -> Result<napple_core::domain::keys::KeySet> {
    match std::fs::metadata(path) {
        Ok(m) if m.is_file() => {}
        Ok(_) => return Err(BuildError::new("keys", format!("{} is not a file", path.display()))),
        Err(e) => return Err(BuildError::new("keys", format!("cannot read the key file {}: {e}", path.display()))),
    }
    Ok(napple_core::domain::keys::KeySet::load(path)?)
}

pub fn gb(n: u64) -> String {
    format!("{:.2} GB", n as f64 / 1e9)
}

/// Free bytes available to unprivileged users on the volume holding `p`.
pub fn free_space(p: &Path) -> Result<u64> {
    let c = CString::new(p.as_os_str().to_string_lossy().as_bytes()).map_err(|_| BuildError::usage("path contains NUL"))?;
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut s) } != 0 {
        return Err(BuildError::io("statvfs", std::io::Error::last_os_error()));
    }
    Ok(s.f_bavail as u64 * s.f_frsize as u64)
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn clonefile(src: *const libc::c_char, dst: *const libc::c_char, flags: u32) -> libc::c_int;
}

/// No clonefile(2) off macOS: always fall back to a plain copy (lets the crate build and test on Linux CI).
#[cfg(not(target_os = "macos"))]
unsafe fn clonefile(_: *const libc::c_char, _: *const libc::c_char, _: u32) -> libc::c_int {
    -1
}

/// Copy-on-write clone when the volume supports it (APFS), else a plain copy. Returns true when cloned.
pub fn clone_or_copy(src: &Path, dst: &Path) -> Result<bool> {
    if let Some(d) = dst.parent() {
        std::fs::create_dir_all(d).map_err(|e| BuildError::io("create dir", e))?;
    }
    let _ = std::fs::remove_file(dst);
    let (cs, cd) = (CString::new(src.as_os_str().to_string_lossy().as_bytes()).unwrap(), CString::new(dst.as_os_str().to_string_lossy().as_bytes()).unwrap());
    if unsafe { clonefile(cs.as_ptr(), cd.as_ptr(), 0) } == 0 {
        return Ok(true);
    }
    std::fs::copy(src, dst).map_err(|e| BuildError::io(&format!("copy {}", src.display()), e))?;
    Ok(false)
}

/// Removes the directory on drop unless `keep()` was called (build staging area).
pub struct TempDir {
    pub path: PathBuf,
    keep: bool,
}

impl TempDir {
    pub fn create(path: PathBuf) -> Result<Self> {
        std::fs::create_dir_all(&path).map_err(|e| BuildError::io(&format!("create {}", path.display()), e))?;
        Ok(Self { path, keep: false })
    }
    pub fn keep(&mut self) {
        self.keep = true;
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        if !self.keep {
            let _ = std::fs::remove_dir_all(&self.path);
        }
    }
}

fn dev_of(p: &Path) -> Option<u64> {
    use std::os::unix::fs::MetadataExt;
    std::fs::metadata(p).ok().map(|m| m.dev())
}

/// Where to stage the bundle and keep the RomFS cache. Folders managed by iCloud Drive / File Provider (Desktop,
/// Documents) re-add Finder metadata to `.app` directories, which breaks `codesign`, and would sync the multi-GB files;
/// so when the temporary directory is on the same volume as `out` (clones and the final rename stay cheap/atomic) it is
/// used; otherwise the output directory itself. `NAPPLE_TMPDIR` overrides.
pub fn stage_root(out: &Path) -> PathBuf {
    let cand = std::env::var_os("NAPPLE_TMPDIR").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
    if cand.is_dir() && dev_of(&cand).is_some() && dev_of(&cand) == dev_of(out) && free_space(&cand).is_ok() {
        return cand;
    }
    out.to_path_buf()
}

/// Unique staging directory name for this build: `.napple-build-<pid>-<seq>`.
pub fn staging_name() -> String {
    static SEQ: AtomicU64 = AtomicU64::new(0);
    format!(".napple-build-{}-{}", std::process::id(), SEQ.fetch_add(1, Ordering::SeqCst))
}

pub fn pid_alive(pid: i32) -> bool {
    let alive = unsafe { libc::kill(pid, 0) } == 0;
    alive || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

/// Deletes `.napple-build-<pid>` directories left by crashed builds whose process no longer exists.
pub fn clean_stale_staging(out: &Path) -> usize {
    let mut n = 0;
    if let Ok(rd) = std::fs::read_dir(out) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            let rest = name.strip_prefix(".napple-build-").or_else(|| name.strip_prefix(".napple-old-"));
            if let Some(pid) = rest.and_then(|p| p.split('-').next()).and_then(|p| p.parse::<i32>().ok()) {
                if pid != std::process::id() as i32 && !pid_alive(pid) && std::fs::remove_dir_all(e.path()).is_ok() {
                    n += 1;
                }
            }
        }
    }
    n
}

/// File-name-safe app name: no path separators/control characters, trimmed, at most 80 chars.
pub fn sanitize_name(s: &str) -> String {
    let cleaned: String = s.chars().map(|c| if c.is_control() || matches!(c, '/' | '\\' | ':' | '\0' | '"' | '*' | '?' | '<' | '>' | '|') { ' ' } else { c }).collect();
    let t = cleaned.split_whitespace().collect::<Vec<_>>().join(" ");
    let t: String = t.chars().take(80).collect();
    t.trim_start_matches(|c: char| c == '.' || c.is_whitespace()).trim_end_matches(|c: char| c == '.' || c.is_whitespace()).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_sanitized() {
        assert_eq!(sanitize_name("Super Mario Odyssey™"), "Super Mario Odyssey™");
        assert_eq!(sanitize_name("../../etc/passwd"), "etc passwd");
        assert_eq!(sanitize_name("  a:b\u{0}c  "), "a b c");
        assert_eq!(sanitize_name("..."), "");
        assert_eq!(sanitize_name(&"x".repeat(200)).len(), 80);
    }

    #[test]
    fn progress_is_monotonic_and_single_line() {
        reset_progress();
        progress("a", 10.0, "x\ny");
        progress("b", 5.0, "back");
        assert_eq!(LAST_PCT_X10.load(Ordering::SeqCst), 100);
    }

    #[test]
    fn clone_or_copy_and_free_space() {
        let d = std::env::temp_dir().join(format!("napple-util-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join("a"), b"hello").unwrap();
        clone_or_copy(&d.join("a"), &d.join("sub/b")).unwrap();
        assert_eq!(std::fs::read(d.join("sub/b")).unwrap(), b"hello");
        clone_or_copy(&d.join("a"), &d.join("sub/b")).unwrap(); // overwrite
        assert!(free_space(&d).unwrap() > 0);
        assert!(free_space(&d.join("missing")).is_err());
        assert_eq!(clean_stale_staging(&d), 0);
        std::fs::create_dir_all(d.join(".napple-build-2147483646-0")).unwrap();
        assert_eq!(clean_stale_staging(&d), 1);
        let _ = std::fs::remove_dir_all(d);
    }
}
