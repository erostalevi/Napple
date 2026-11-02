//! Secret storage: refresh/access tokens, the device X25519 private key, the unwrapped account save key.
//!
//! macOS and iOS: generic-password Keychain items, service `lat.napple.account.<namespace>`, account = the secret's name. Items are
//! device-only (never iCloud-synchronized).
//! - iOS: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly in the app's keychain.
//! - macOS: the file-based login keychain (unsigned / ad-hoc development binaries cannot use the data-protection keychain). The item's
//!   decrypt ACL allows any application of this user, so the napple CLI, Napple Studio and every built game (different binaries that all
//!   act as this Mac's one device) read it without a keychain prompt. User interaction is disabled: a locked keychain gives an error,
//!   never a dialog. `NAPPLE_ACCOUNT_KEYCHAIN=<path>` (+ `NAPPLE_ACCOUNT_KEYCHAIN_PASSWORD`) uses a separate keychain file instead (created
//!   on first use): two of them are two "devices" on one Mac for tests.
//! Elsewhere, or with `NAPPLE_ACCOUNT_STORE=file`: 0600 files in `<state dir>/secrets/`.
use crate::{Error, Result};
use std::path::{Path, PathBuf};

pub trait SecretStore: Send + Sync {
    fn get(&self, name: &str) -> Result<Option<Vec<u8>>>;
    fn set(&self, name: &str, value: &[u8]) -> Result<()>;
    fn delete(&self, name: &str) -> Result<()>;
    fn describe(&self) -> String;
}

pub const REFRESH_TOKEN: &str = "refresh-token";
pub const ACCESS_TOKEN: &str = "access-token";
pub const DEVICE_KEY: &str = "device-key";
pub const ACCOUNT_KEY: &str = "account-key";
pub const ALL: [&str; 4] = [REFRESH_TOKEN, ACCESS_TOKEN, DEVICE_KEY, ACCOUNT_KEY];

static FORCE_FILE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Tests: use the file store whatever the environment says.
pub fn force_file_store(on: bool) {
    FORCE_FILE.store(on, std::sync::atomic::Ordering::Relaxed);
}

pub fn open(namespace: &str, state_dir: &Path) -> Result<Box<dyn SecretStore>> {
    let mut kind = std::env::var("NAPPLE_ACCOUNT_STORE").unwrap_or_default();
    if FORCE_FILE.load(std::sync::atomic::Ordering::Relaxed) {
        kind = "file".into();
    }
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    if kind != "file" {
        return Ok(Box::new(keychain::Keychain::new(namespace)?));
    }
    let _ = (namespace, kind);
    Ok(Box::new(FileStore { dir: state_dir.join("secrets") }))
}

pub struct FileStore {
    pub dir: PathBuf,
}

impl SecretStore for FileStore {
    fn get(&self, name: &str) -> Result<Option<Vec<u8>>> {
        match std::fs::read(self.dir.join(name)) {
            Ok(v) => Ok(Some(v)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e.into()),
        }
    }
    fn set(&self, name: &str, value: &[u8]) -> Result<()> {
        use std::io::Write;
        use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&self.dir)?;
        let tmp = self.dir.join(format!(".{name}.tmp"));
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        f.write_all(value)?;
        f.sync_all()?;
        std::fs::rename(&tmp, self.dir.join(name))?;
        Ok(())
    }
    fn delete(&self, name: &str) -> Result<()> {
        match std::fs::remove_file(self.dir.join(name)) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.into()),
            _ => Ok(()),
        }
    }
    fn describe(&self) -> String {
        format!("files in {}", self.dir.display())
    }
}

#[cfg(any(target_os = "macos", target_os = "ios"))]
pub mod keychain {
    use super::SecretStore;
    use crate::{Error, Result};
    use core_foundation::base::{CFType, CFTypeRef, OSStatus, TCFType};
    use core_foundation::boolean::CFBoolean;
    use core_foundation::data::CFData;
    use core_foundation::dictionary::CFDictionary;
    use core_foundation::string::{CFString, CFStringRef};
    use core_foundation_sys::dictionary::CFDictionaryRef;
    use std::ffi::c_void;

    #[link(name = "Security", kind = "framework")]
    unsafe extern "C" {
        static kSecClass: CFStringRef;
        static kSecClassGenericPassword: CFStringRef;
        static kSecAttrService: CFStringRef;
        static kSecAttrAccount: CFStringRef;
        static kSecAttrLabel: CFStringRef;
        static kSecValueData: CFStringRef;
        static kSecReturnData: CFStringRef;
        static kSecMatchLimit: CFStringRef;
        static kSecMatchLimitOne: CFStringRef;
        static kSecAttrSynchronizable: CFStringRef;
        fn SecItemAdd(attributes: CFDictionaryRef, result: *mut CFTypeRef) -> OSStatus;
        fn SecItemCopyMatching(query: CFDictionaryRef, result: *mut CFTypeRef) -> OSStatus;
        fn SecItemDelete(query: CFDictionaryRef) -> OSStatus;
    }
    #[cfg(target_os = "ios")]
    #[link(name = "Security", kind = "framework")]
    unsafe extern "C" {
        static kSecAttrAccessible: CFStringRef;
        static kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly: CFStringRef;
    }
    #[cfg(target_os = "macos")]
    #[link(name = "Security", kind = "framework")]
    unsafe extern "C" {
        static kSecAttrAccess: CFStringRef;
        static kSecUseKeychain: CFStringRef;
        static kSecMatchSearchList: CFStringRef;
        static kSecACLAuthorizationDecrypt: CFStringRef;
        fn SecKeychainSetUserInteractionAllowed(state: u8) -> OSStatus;
        fn SecKeychainCreate(path: *const i8, pwlen: u32, pw: *const c_void, prompt: u8, access: *const c_void, kc: *mut CFTypeRef) -> OSStatus;
        fn SecKeychainOpen(path: *const i8, kc: *mut CFTypeRef) -> OSStatus;
        fn SecKeychainUnlock(kc: CFTypeRef, pwlen: u32, pw: *const c_void, use_pw: u8) -> OSStatus;
        fn SecAccessCreate(descriptor: CFStringRef, trusted: CFTypeRef, access: *mut CFTypeRef) -> OSStatus;
        fn SecAccessCopyMatchingACLList(access: CFTypeRef, authorization: CFTypeRef) -> CFTypeRef;
        fn SecACLSetContents(acl: CFTypeRef, apps: CFTypeRef, description: CFStringRef, prompt: u16) -> OSStatus;
    }

    const NOT_FOUND: OSStatus = -25300;
    const DUPLICATE: OSStatus = -25299;

    pub struct Keychain {
        service: String,
        #[cfg(target_os = "macos")]
        file: Option<(String, CFType)>,
    }
    // CFType of a SecKeychainRef: Security serializes access to it internally.
    unsafe impl Send for Keychain {}
    unsafe impl Sync for Keychain {}

    fn s(r: CFStringRef) -> CFType {
        unsafe { CFString::wrap_under_get_rule(r) }.as_CFType()
    }
    fn k(r: CFStringRef) -> CFString {
        unsafe { CFString::wrap_under_get_rule(r) }
    }
    fn err(what: &str, st: OSStatus) -> Error {
        let hint = match st {
            -25308 => " (the keychain is locked or needs a confirmation; unlock it and retry)",
            -34018 => " (missing keychain entitlement)",
            _ => "",
        };
        Error::Io(format!("keychain {what}: OSStatus {st}{hint}"))
    }

    impl Keychain {
        /// A keychain item set under an exact service name (the shared Napple Online wrap key: `lat.napple.online`).
        pub fn with_service(service: &str) -> Result<Self> {
            let mut k = Self::new("")?;
            k.service = service.to_string();
            Ok(k)
        }

        pub fn new(namespace: &str) -> Result<Self> {
            #[cfg(target_os = "macos")]
            {
                unsafe { SecKeychainSetUserInteractionAllowed(0) };
                let file = match std::env::var("NAPPLE_ACCOUNT_KEYCHAIN") {
                    Ok(p) if !p.is_empty() => Some((p.clone(), open_file_keychain(&p)?)),
                    _ => None,
                };
                Ok(Keychain { service: format!("lat.napple.account.{namespace}"), file })
            }
            #[cfg(target_os = "ios")]
            Ok(Keychain { service: format!("lat.napple.account.{namespace}") })
        }

        fn base(&self, name: &str) -> Vec<(CFString, CFType)> {
            vec![
                (k(unsafe { kSecClass }), s(unsafe { kSecClassGenericPassword })),
                (k(unsafe { kSecAttrService }), CFString::new(&self.service).as_CFType()),
                (k(unsafe { kSecAttrAccount }), CFString::new(name).as_CFType()),
            ]
        }
        fn search_scope(&self, q: &mut Vec<(CFString, CFType)>) {
            #[cfg(target_os = "macos")]
            if let Some((_, kc)) = &self.file {
                let list = core_foundation::array::CFArray::from_CFTypes(&[kc.clone()]);
                q.push((k(unsafe { kSecMatchSearchList }), list.as_CFType()));
            }
            let _ = q;
        }
    }

    #[cfg(target_os = "macos")]
    fn open_file_keychain(path: &str) -> Result<CFType> {
        let pw = std::env::var("NAPPLE_ACCOUNT_KEYCHAIN_PASSWORD").unwrap_or_else(|_| "napple".into());
        let cpath = std::ffi::CString::new(path).map_err(|_| Error::Io("keychain path".into()))?;
        let mut kc: CFTypeRef = std::ptr::null();
        let st = if std::path::Path::new(path).exists() {
            let st = unsafe { SecKeychainOpen(cpath.as_ptr(), &mut kc) };
            if st == 0 {
                let u = unsafe { SecKeychainUnlock(kc, pw.len() as u32, pw.as_ptr() as *const c_void, 1) };
                if u != 0 {
                    unsafe { core_foundation_sys::base::CFRelease(kc) };
                    return Err(err("unlock", u));
                }
            }
            st
        } else {
            unsafe { SecKeychainCreate(cpath.as_ptr(), pw.len() as u32, pw.as_ptr() as *const c_void, 0, std::ptr::null(), &mut kc) }
        };
        if st != 0 {
            return Err(err("open", st));
        }
        Ok(unsafe { CFType::wrap_under_create_rule(kc) })
    }

    /// An access object whose decrypt ACL trusts every application (no prompt for the CLI, Studio or a game reading the item).
    #[cfg(target_os = "macos")]
    fn any_app_access(label: &str) -> Result<CFType> {
        let mut access: CFTypeRef = std::ptr::null();
        let desc = CFString::new(label);
        let st = unsafe { SecAccessCreate(desc.as_concrete_TypeRef(), std::ptr::null(), &mut access) };
        if st != 0 {
            return Err(err("access", st));
        }
        let access = unsafe { CFType::wrap_under_create_rule(access) };
        let list = unsafe { SecAccessCopyMatchingACLList(access.as_CFTypeRef(), kSecACLAuthorizationDecrypt as CFTypeRef) };
        if !list.is_null() {
            let list: core_foundation::array::CFArray<CFType> = unsafe { core_foundation::array::CFArray::wrap_under_create_rule(list as _) };
            for acl in list.iter() {
                let st = unsafe { SecACLSetContents(acl.as_CFTypeRef(), std::ptr::null(), desc.as_concrete_TypeRef(), 0) };
                if st != 0 {
                    return Err(err("acl", st));
                }
            }
        }
        Ok(access)
    }

    impl SecretStore for Keychain {
        fn get(&self, name: &str) -> Result<Option<Vec<u8>>> {
            let mut q = self.base(name);
            q.push((k(unsafe { kSecReturnData }), CFBoolean::true_value().as_CFType()));
            q.push((k(unsafe { kSecMatchLimit }), s(unsafe { kSecMatchLimitOne })));
            self.search_scope(&mut q);
            let d = CFDictionary::from_CFType_pairs(&q);
            let mut out: CFTypeRef = std::ptr::null();
            let st = unsafe { SecItemCopyMatching(d.as_concrete_TypeRef(), &mut out) };
            match st {
                0 => {
                    let data = unsafe { CFData::wrap_under_create_rule(out as _) };
                    Ok(Some(data.bytes().to_vec()))
                }
                NOT_FOUND => Ok(None),
                _ => Err(err("read", st)),
            }
        }
        fn set(&self, name: &str, value: &[u8]) -> Result<()> {
            self.delete(name)?;
            let label = format!("Napple Account ({name})");
            let mut a = self.base(name);
            a.push((k(unsafe { kSecValueData }), CFData::from_buffer(value).as_CFType()));
            a.push((k(unsafe { kSecAttrLabel }), CFString::new(&label).as_CFType()));
            a.push((k(unsafe { kSecAttrSynchronizable }), CFBoolean::false_value().as_CFType()));
            #[cfg(target_os = "ios")]
            a.push((k(unsafe { kSecAttrAccessible }), s(unsafe { kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly })));
            #[cfg(target_os = "macos")]
            {
                a.push((k(unsafe { kSecAttrAccess }), any_app_access(&label)?));
                if let Some((_, kc)) = &self.file {
                    a.push((k(unsafe { kSecUseKeychain }), kc.clone()));
                }
            }
            let d = CFDictionary::from_CFType_pairs(&a);
            let st = unsafe { SecItemAdd(d.as_concrete_TypeRef(), std::ptr::null_mut()) };
            if st == 0 || st == DUPLICATE { Ok(()) } else { Err(err("write", st)) }
        }
        fn delete(&self, name: &str) -> Result<()> {
            let mut q = self.base(name);
            self.search_scope(&mut q);
            let d = CFDictionary::from_CFType_pairs(&q);
            match unsafe { SecItemDelete(d.as_concrete_TypeRef()) } {
                0 | NOT_FOUND => Ok(()),
                st => Err(err("delete", st)),
            }
        }
        fn describe(&self) -> String {
            #[cfg(target_os = "macos")]
            if let Some((p, _)) = &self.file {
                return format!("keychain {p}, service {}", self.service);
            }
            format!("keychain, service {}", self.service)
        }
    }
}

/// Removes every secret of this namespace (sign-out).
pub fn wipe(store: &dyn SecretStore) -> Result<()> {
    let mut last = Ok(());
    for n in ALL {
        if let Err(e) = store.delete(n) {
            last = Err(e);
        }
    }
    last.map_err(|e: Error| e)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn exercise(st: &dyn SecretStore) {
        st.delete("unit-test").unwrap();
        assert_eq!(st.get("unit-test").unwrap(), None);
        st.set("unit-test", b"first").unwrap();
        assert_eq!(st.get("unit-test").unwrap().as_deref(), Some(&b"first"[..]));
        st.set("unit-test", &[0, 1, 2, 255]).unwrap();
        assert_eq!(st.get("unit-test").unwrap().as_deref(), Some(&[0u8, 1, 2, 255][..]));
        st.delete("unit-test").unwrap();
        assert_eq!(st.get("unit-test").unwrap(), None);
    }

    #[test]
    fn file_store() {
        let d = std::env::temp_dir().join(format!("napple-cloud-store-{}", std::process::id()));
        let st = FileStore { dir: d.clone() };
        exercise(&st);
        use std::os::unix::fs::PermissionsExt;
        st.set("x", b"1").unwrap();
        assert_eq!(std::fs::metadata(d.join("x")).unwrap().permissions().mode() & 0o777, 0o600);
        let _ = std::fs::remove_dir_all(d);
    }

    /// The macOS Keychain wrapper against a throwaway keychain file (never the login keychain), including a second handle on the
    /// same file reading what the first wrote (another process of this user does the same through the any-application ACL).
    #[cfg(target_os = "macos")]
    #[test]
    fn macos_keychain_file() {
        let d = std::env::temp_dir().join(format!("napple-cloud-kc-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let path = d.join("test.keychain-db");
        // SAFETY of env mutation in tests: this is the only test touching these variables.
        unsafe {
            std::env::set_var("NAPPLE_ACCOUNT_KEYCHAIN", &path);
            std::env::set_var("NAPPLE_ACCOUNT_KEYCHAIN_PASSWORD", "unit-test");
        }
        let a = keychain::Keychain::new("unit-a").unwrap();
        exercise(&a);
        a.set(DEVICE_KEY, &[7u8; 32]).unwrap();
        let again = keychain::Keychain::new("unit-a").unwrap();
        assert_eq!(again.get(DEVICE_KEY).unwrap().as_deref(), Some(&[7u8; 32][..]));
        let other_ns = keychain::Keychain::new("unit-b").unwrap();
        assert_eq!(other_ns.get(DEVICE_KEY).unwrap(), None, "namespaces are separate");
        wipe(&again).unwrap();
        assert_eq!(a.get(DEVICE_KEY).unwrap(), None);
        unsafe {
            std::env::remove_var("NAPPLE_ACCOUNT_KEYCHAIN");
            std::env::remove_var("NAPPLE_ACCOUNT_KEYCHAIN_PASSWORD");
        }
        let _ = std::fs::remove_dir_all(d);
    }
}
