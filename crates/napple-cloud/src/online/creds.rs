//! `<NAPPLE_HOME>/online/credentials.json`: the account sign-in shared by every Napple game on the Mac.
//!
//! Mode 0600. The refresh token, the device key and the account save key are wrapped with AES-256-GCM under a key kept in the macOS Keychain
//! (service `lat.napple.online`, account `wrap-key`); without a Keychain a 0600 `wrap.key` next to the file does the same job. Several games
//! (processes) may refresh the token at once: every change is a read-modify-write under an exclusive `flock` on `credentials.lock` and lands by
//! writing a temporary file and renaming it, so a reader never sees half a file. Access tokens are never written.
use crate::blob::{aes_gcm_open, aes_gcm_seal};
use crate::util::{b64, random, unb64};
use crate::{Error, Result};
use serde::{Deserialize, Serialize};
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};

const AAD: &[u8] = b"napple-online-credentials-v1";
pub const KEYCHAIN_SERVICE: &str = "lat.napple.online";
pub const KEYCHAIN_ACCOUNT: &str = "wrap-key";

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Credentials {
    pub server: String,
    pub account_id: String,
    pub device_id: String,
    pub refresh_token_enc: String,
    pub device_key_enc: String,
    /// The account save key, wrapped like the others (empty until this device has it).
    #[serde(default)]
    pub account_key_enc: String,
    #[serde(default)]
    pub email: String,
}

pub struct CredStore {
    dir: PathBuf,
    keychain: bool,
}

pub fn home() -> PathBuf {
    match std::env::var("NAPPLE_HOME") {
        Ok(h) if !h.is_empty() => PathBuf::from(h),
        _ => PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join("Library/Application Support/Napple"),
    }
}

impl CredStore {
    /// `<home>/online`. The Keychain wraps the key unless NAPPLE_ACCOUNT_STORE=file (tests, other platforms).
    pub fn open(home: &Path) -> CredStore {
        let keychain = cfg!(any(target_os = "macos", target_os = "ios")) && std::env::var("NAPPLE_ACCOUNT_STORE").as_deref() != Ok("file");
        CredStore { dir: home.join("online"), keychain }
    }

    pub fn with_file_key(home: &Path) -> CredStore {
        CredStore { dir: home.join("online"), keychain: false }
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    fn path(&self) -> PathBuf {
        self.dir.join("credentials.json")
    }

    fn mkdir(&self) -> Result<()> {
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&self.dir)?;
        Ok(())
    }

    fn write_private(&self, name: &str, data: &[u8]) -> Result<()> {
        self.mkdir()?;
        let tmp = self.dir.join(format!(".{name}.{}.tmp", std::process::id()));
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
        std::fs::rename(&tmp, self.dir.join(name))?;
        Ok(())
    }

    /// The wrapping key: Keychain item, else the key file. Made on first use (under the caller's lock).
    fn wrap_key(&self) -> Result<[u8; 32]> {
        #[cfg(any(target_os = "macos", target_os = "ios"))]
        if self.keychain {
            use crate::store::SecretStore;
            if let Ok(kc) = crate::store::keychain::Keychain::with_service(KEYCHAIN_SERVICE) {
                match kc.get(KEYCHAIN_ACCOUNT) {
                    Ok(Some(k)) => {
                        if let Ok(k) = <[u8; 32]>::try_from(k.as_slice()) {
                            return Ok(k);
                        }
                    }
                    Ok(None) => {
                        let k: [u8; 32] = random();
                        if kc.set(KEYCHAIN_ACCOUNT, &k).is_ok() {
                            return Ok(k);
                        }
                    }
                    Err(_) => {}
                }
            }
        }
        let p = self.dir.join("wrap.key");
        match std::fs::read(&p) {
            Ok(k) => <[u8; 32]>::try_from(k.as_slice()).map_err(|_| Error::Crypto("wrap.key is not 32 bytes".into())),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let k: [u8; 32] = random();
                self.write_private("wrap.key", &k)?;
                Ok(k)
            }
            Err(e) => Err(e.into()),
        }
    }

    pub fn seal(&self, plain: &[u8]) -> Result<String> {
        let key = self.wrap_key()?;
        let nonce: [u8; 12] = random();
        let mut out = nonce.to_vec();
        out.extend(aes_gcm_seal(&key, &nonce, plain, AAD)?);
        Ok(b64(&out))
    }

    pub fn open_sealed(&self, enc: &str) -> Result<Vec<u8>> {
        let raw = unb64(enc).filter(|r| r.len() > 12).ok_or_else(|| Error::Crypto("wrapped credential is malformed".into()))?;
        let key = self.wrap_key()?;
        aes_gcm_open(&key, raw[..12].try_into().unwrap(), &raw[12..], AAD)
    }

    fn lock(&self) -> Result<std::fs::File> {
        self.mkdir()?;
        let f = std::fs::OpenOptions::new().write(true).create(true).truncate(false).mode(0o600).open(self.dir.join("credentials.lock"))?;
        if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX) } != 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        Ok(f) // closing the file releases the lock
    }

    fn read(&self) -> Result<Option<Credentials>> {
        match std::fs::read(self.path()) {
            Ok(b) => Ok(Some(serde_json::from_slice(&b)?)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e.into()),
        }
    }

    pub fn load(&self) -> Result<Option<Credentials>> {
        self.read()
    }

    /// Read-modify-write under the cross-process lock. `f` sees the current file (None = signed out) and may replace or clear it; its result
    /// is returned. Not re-entrant: do not call `update` / `seal` helpers that take the lock from inside `f` (seal / open_sealed do not).
    pub fn update<T>(&self, f: impl FnOnce(&mut Option<Credentials>) -> Result<T>) -> Result<T> {
        let _lock = self.lock()?;
        let mut cur = self.read()?;
        let before = cur.clone();
        let r = f(&mut cur)?;
        if cur != before {
            match &cur {
                Some(c) => self.write_private("credentials.json", serde_json::to_string_pretty(c)?.as_bytes())?,
                None => {
                    let _ = std::fs::remove_file(self.path());
                }
            }
        }
        Ok(r)
    }

    /// The refresh token of the stored credentials, decrypted.
    pub fn refresh_token(&self) -> Result<Option<String>> {
        match self.read()? {
            Some(c) if !c.refresh_token_enc.is_empty() => Ok(Some(String::from_utf8_lossy(&self.open_sealed(&c.refresh_token_enc)?).into_owned())),
            _ => Ok(None),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn tmp(n: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("napple-creds-{n}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        d
    }

    fn sample(s: &CredStore, rt: &str) -> Credentials {
        Credentials {
            server: "http://x".into(),
            account_id: "a".into(),
            device_id: "d".into(),
            refresh_token_enc: s.seal(rt.as_bytes()).unwrap(),
            device_key_enc: s.seal(b"devkey").unwrap(),
            ..Default::default()
        }
    }

    #[test]
    fn file_is_private_and_wrapped() {
        let h = tmp("mode");
        let s = CredStore::with_file_key(&h);
        s.update(|c| {
            *c = Some(sample(&s, "refresh-secret"));
            Ok(())
        })
        .unwrap();
        let p = h.join("online/credentials.json");
        assert_eq!(std::fs::metadata(&p).unwrap().permissions().mode() & 0o777, 0o600);
        assert_eq!(std::fs::metadata(h.join("online/wrap.key")).unwrap().permissions().mode() & 0o777, 0o600);
        let raw = std::fs::read_to_string(&p).unwrap();
        assert!(!raw.contains("refresh-secret"));
        assert_eq!(s.refresh_token().unwrap().as_deref(), Some("refresh-secret"));
        // a second store object (another game) reads the same file with the same key
        assert_eq!(CredStore::with_file_key(&h).refresh_token().unwrap().as_deref(), Some("refresh-secret"));
        let _ = std::fs::remove_dir_all(&h);
    }

    #[test]
    fn tampered_blob_is_refused() {
        let h = tmp("tamper");
        let s = CredStore::with_file_key(&h);
        let mut enc = s.seal(b"x").unwrap();
        enc.push('A');
        assert!(s.open_sealed(&enc).is_err());
        let _ = std::fs::remove_dir_all(&h);
    }

    #[test]
    fn concurrent_updates_never_lose_or_corrupt() {
        let h = tmp("conc");
        let s = std::sync::Arc::new(CredStore::with_file_key(&h));
        s.update(|c| {
            *c = Some(sample(&s, "0"));
            Ok(())
        })
        .unwrap();
        let mut ts = vec![];
        for _ in 0..4 {
            let s = s.clone();
            ts.push(std::thread::spawn(move || {
                // each thread opens its own store object so flock (per open file description) really contends
                let mine = CredStore::with_file_key(s.dir().parent().unwrap());
                for _ in 0..15 {
                    mine.update(|c| {
                        let cur = c.as_mut().unwrap();
                        let n: u32 = String::from_utf8(mine.open_sealed(&cur.refresh_token_enc)?).unwrap().parse().unwrap();
                        cur.refresh_token_enc = mine.seal((n + 1).to_string().as_bytes())?;
                        Ok(())
                    })
                    .unwrap();
                    // a plain reader never sees a torn file
                    assert!(mine.load().unwrap().is_some());
                }
            }));
        }
        for t in ts {
            t.join().unwrap();
        }
        assert_eq!(s.refresh_token().unwrap().as_deref(), Some("60"));
        let _ = std::fs::remove_dir_all(&h);
    }

    #[test]
    fn clear_removes_the_file() {
        let h = tmp("clear");
        let s = CredStore::with_file_key(&h);
        s.update(|c| {
            *c = Some(sample(&s, "r"));
            Ok(())
        })
        .unwrap();
        s.update(|c| {
            *c = None;
            Ok(())
        })
        .unwrap();
        assert!(s.load().unwrap().is_none());
        let _ = std::fs::remove_dir_all(&h);
    }
}
