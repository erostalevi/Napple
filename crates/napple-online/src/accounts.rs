//! Napple accounts and the tokens the server issues for them.
//!
//! An account is created for a local user of a Napple instance on first use (the runtime sends the user's id) and gets a NEX principal id (`pid`)
//! and a secret the instance keeps. With pid + secret the instance asks for an ID token; the token is what the title hands to the game server
//! (AuthenticationInfo.token), and the game server verifies it with the same server key. Tokens are compact JWS (HS256), so a title that looks
//! at its token sees the usual three-part shape. Nothing here is a Nintendo identity or credential.

use crate::crypto::{base64, base64_decode, ct_eq, hex, hmac_sha256, random_bytes, sha256, unhex};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::collections::HashMap;
use std::sync::Mutex;

/// How long an ID token is valid.
pub const TOKEN_LIFETIME_SECS: u64 = 15 * 60;
/// First principal id handed out ("NA" in the top bytes keeps Napple pids recognisable and far from small reserved ids).
pub const FIRST_PID: u64 = 0x4e41_0000_0000_1000;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Account {
    pub pid: u64,
    /// The local user id the instance registered (32 hex digits).
    pub uid: String,
    /// SHA-256 of the secret, hex. The secret itself is only known to the instance.
    pub secret_sha256: String,
    pub created: u64,
}

#[derive(Default, Serialize, Deserialize)]
struct Store {
    next_pid: u64,
    accounts: Vec<Account>,
}

/// The claims of a verified token.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Claims {
    pub iss: String,
    pub sub: String,
    pub pid: u64,
    pub tid: String,
    pub iat: u64,
    pub exp: u64,
}

pub fn now_secs() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Accounts plus the server key. `dir` = None keeps everything in memory (tests).
pub struct Accounts {
    dir: Option<PathBuf>,
    key: Vec<u8>,
    store: Mutex<Store>,
    /// Each account's friends (their pids) as its client last reported them (POST /napple/v1/friends; the client learns them from
    /// napple-social). Kept in memory: a client reports again after every restart of either side.
    friends: Mutex<HashMap<u64, Vec<u64>>>,
}

#[derive(Debug, PartialEq)]
pub enum AccountError {
    BadRequest,
    Unknown,
    BadSecret,
    Io(String),
}

impl Accounts {
    /// Opens (or creates) the account store in `dir`: `server.key` (32 random bytes, hex) and `accounts.json`.
    pub fn open(dir: Option<&Path>) -> Result<Accounts, AccountError> {
        let mut key = random_bytes(32);
        let mut store = Store { next_pid: FIRST_PID, accounts: vec![] };
        if let Some(d) = dir {
            std::fs::create_dir_all(d).map_err(|e| AccountError::Io(e.to_string()))?;
            let kp = d.join("server.key");
            match std::fs::read_to_string(&kp) {
                Ok(s) => key = unhex(s.trim()).filter(|k| k.len() >= 16).ok_or(AccountError::Io("server.key is not hex".into()))?,
                Err(_) => {
                    std::fs::write(&kp, hex(&key)).map_err(|e| AccountError::Io(e.to_string()))?;
                    #[cfg(unix)]
                    {
                        use std::os::unix::fs::PermissionsExt;
                        let _ = std::fs::set_permissions(&kp, std::fs::Permissions::from_mode(0o600));
                    }
                }
            }
            if let Ok(s) = std::fs::read_to_string(d.join("accounts.json")) {
                store = serde_json::from_str(&s).map_err(|e| AccountError::Io(e.to_string()))?;
            }
        }
        Ok(Accounts { dir: dir.map(|d| d.to_path_buf()), key, store: Mutex::new(store), friends: Mutex::new(HashMap::new()) })
    }

    /// The server's secret key (tokens, and the key the secure server's tickets are sealed with).
    pub fn server_key(&self) -> &[u8] {
        &self.key
    }

    fn save(&self, store: &Store) -> Result<(), AccountError> {
        if let Some(d) = &self.dir {
            let tmp = d.join("accounts.json.tmp");
            std::fs::write(&tmp, serde_json::to_string_pretty(store).unwrap()).map_err(|e| AccountError::Io(e.to_string()))?;
            std::fs::rename(&tmp, d.join("accounts.json")).map_err(|e| AccountError::Io(e.to_string()))?;
        }
        Ok(())
    }

    /// Creates an account for local user `uid` and returns it with its secret (shown once). A uid may register again (a reinstalled instance
    /// that lost its credentials): it gets a new account.
    pub fn create(&self, uid: &str) -> Result<(Account, String), AccountError> {
        if uid.len() != 32 || unhex(uid).is_none() {
            return Err(AccountError::BadRequest);
        }
        let secret = hex(&random_bytes(32));
        let mut st = self.store.lock().unwrap();
        if st.next_pid < FIRST_PID {
            st.next_pid = FIRST_PID;
        }
        let a = Account { pid: st.next_pid, uid: uid.to_ascii_lowercase(), secret_sha256: hex(&sha256(secret.as_bytes())), created: now_secs() };
        st.next_pid += 1;
        st.accounts.push(a.clone());
        self.save(&st)?;
        Ok((a, secret))
    }

    pub fn get(&self, pid: u64) -> Option<Account> {
        self.store.lock().unwrap().accounts.iter().find(|a| a.pid == pid).cloned()
    }

    /// Replaces the friend list `pid` reported (at most 300 pids, the account itself left out).
    pub fn set_friends(&self, pid: u64, mut friends: Vec<u64>) {
        friends.retain(|&f| f != pid && f != 0);
        friends.sort_unstable();
        friends.dedup();
        friends.truncate(300);
        self.friends.lock().unwrap().insert(pid, friends);
    }

    /// The friends `pid` reported (empty when it never did).
    pub fn friends(&self, pid: u64) -> Vec<u64> {
        self.friends.lock().unwrap().get(&pid).cloned().unwrap_or_default()
    }

    /// Checks pid + secret.
    pub fn authenticate(&self, pid: u64, secret: &str) -> Result<Account, AccountError> {
        let a = self.get(pid).ok_or(AccountError::Unknown)?;
        if !ct_eq(hex(&sha256(secret.as_bytes())).as_bytes(), a.secret_sha256.as_bytes()) {
            return Err(AccountError::BadSecret);
        }
        Ok(a)
    }

    /// Issues an ID token for `pid` and title `tid` (16 hex digits), valid TOKEN_LIFETIME_SECS from `now`.
    pub fn issue_token(&self, pid: u64, tid: &str, now: u64) -> String {
        let header = base64(br#"{"alg":"HS256","typ":"JWT","kid":"napple"}"#, true);
        let claims = Claims { iss: "napple-online".into(), sub: format!("{pid:016x}"), pid, tid: tid.to_ascii_lowercase(), iat: now, exp: now + TOKEN_LIFETIME_SECS };
        let payload = base64(serde_json::to_string(&claims).unwrap().as_bytes(), true);
        let signing = format!("{header}.{payload}");
        let sig = base64(&hmac_sha256(&self.key, signing.as_bytes()), true);
        format!("{signing}.{sig}")
    }

    /// Verifies a token's signature and lifetime. The account must still exist.
    pub fn verify_token(&self, token: &str, now: u64) -> Option<Claims> {
        let mut parts = token.trim_end_matches('\0').split('.');
        let (h, p, s) = (parts.next()?, parts.next()?, parts.next()?);
        if parts.next().is_some() {
            return None;
        }
        let expect = hmac_sha256(&self.key, format!("{h}.{p}").as_bytes());
        if !ct_eq(&base64_decode(s)?, &expect) {
            return None;
        }
        let claims: Claims = serde_json::from_slice(&base64_decode(p)?).ok()?;
        if claims.iss != "napple-online" || now >= claims.exp || claims.iat > now + 300 {
            return None;
        }
        self.get(claims.pid)?;
        Some(claims)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const UID: &str = "4e6170706c6555736572000000000001";

    #[test]
    fn create_authenticate_token() {
        let acc = Accounts::open(None).unwrap();
        let (a, secret) = acc.create(UID).unwrap();
        assert_eq!(a.pid, FIRST_PID);
        assert_eq!(acc.authenticate(a.pid, &secret).unwrap(), a);
        assert_eq!(acc.authenticate(a.pid, "00"), Err(AccountError::BadSecret));
        assert_eq!(acc.authenticate(a.pid + 1, &secret), Err(AccountError::Unknown));
        let (b, _) = acc.create(UID).unwrap();
        assert_eq!(b.pid, FIRST_PID + 1);
        assert_eq!(acc.create("xyz"), Err(AccountError::BadRequest));

        let t = acc.issue_token(a.pid, "0100152000022000", 1000);
        assert_eq!(t.split('.').count(), 3);
        let c = acc.verify_token(&t, 1001).unwrap();
        assert_eq!(c.pid, a.pid);
        assert_eq!(c.tid, "0100152000022000");
        // expiry, tampering, another server's key
        assert!(acc.verify_token(&t, 1000 + TOKEN_LIFETIME_SECS).is_none());
        let mut bad = t.clone();
        bad.replace_range(bad.len() - 3.., "AAA");
        assert!(acc.verify_token(&bad, 1001).is_none());
        let other = Accounts::open(None).unwrap();
        other.create(UID).unwrap();
        assert!(other.verify_token(&t, 1001).is_none());
        // a token for an account that does not exist
        assert!(acc.verify_token(&acc.issue_token(42, "0100152000022000", 1000), 1001).is_none());
        // the NEX String carries a NUL
        assert!(acc.verify_token(&format!("{t}\0"), 1001).is_some());
    }

    #[test]
    fn persistent_store() {
        let dir = std::env::temp_dir().join(format!("napple-online-test-{}", hex(&random_bytes(6))));
        let (pid, secret, token) = {
            let acc = Accounts::open(Some(&dir)).unwrap();
            let (a, s) = acc.create(UID).unwrap();
            (a.pid, s, acc.issue_token(a.pid, "0100152000022000", now_secs()))
        };
        let acc = Accounts::open(Some(&dir)).unwrap();
        assert!(acc.authenticate(pid, &secret).is_ok());
        assert!(acc.verify_token(&token, now_secs()).is_some(), "the server key survives a restart");
        assert_eq!(acc.create(UID).unwrap().0.pid, pid + 1);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
