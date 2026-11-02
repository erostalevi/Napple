//! Environment-only configuration. Every knob has a default except where noted; see docs/guides/napple-social.md.

use std::net::SocketAddr;
use std::path::PathBuf;

/// Friends per profile.
pub const FRIEND_LIMIT: i64 = 300;
/// Outgoing pending friend requests per profile.
pub const OUTGOING_REQUEST_LIMIT: i64 = 100;
/// Incoming pending friend requests per profile (beyond this, senders get `target_inbox_full`).
pub const INCOMING_REQUEST_LIMIT: i64 = 300;
/// Blocked profiles per profile.
pub const BLOCK_LIMIT: i64 = 1000;
/// Outgoing live invites per profile.
pub const OUTGOING_INVITE_LIMIT: i64 = 100;
/// Invite lifetime.
pub const INVITE_TTL_MS: i64 = 10 * 60 * 1000;
/// A profile is offline after this many heartbeat intervals without a heartbeat.
pub const MISSED_HEARTBEATS: i64 = 3;
/// Clock skew tolerated on `exp` / `nbf`.
pub const TOKEN_LEEWAY_SECS: i64 = 30;
/// Profile-ownership cache lifetime (the contract says 60 s).
pub const PROFILE_CACHE_MS: i64 = 60_000;
/// A profile missing from a cached list causes a re-fetch only if the cached list is older than this.
pub const PROFILE_MISS_REFETCH_MS: i64 = 5_000;
/// JWKS keys are re-fetched after this age (a stale key is still used if the account service is unreachable).
pub const JWKS_TTL_MS: i64 = 10 * 60 * 1000;
/// An unknown `kid` triggers at most one JWKS fetch per this interval.
pub const JWKS_MIN_REFRESH_MS: i64 = 10_000;
/// Field limits.
pub const MAX_TITLE_NAME: usize = 128;
pub const MAX_MODE: usize = 128;
pub const MAX_JOIN_TOKEN: usize = 1024;
pub const MAX_INVITE_MESSAGE: usize = 140;

#[derive(Clone, Debug)]
pub struct Config {
    /// `NAPPLE_SOCIAL_BIND`, default `127.0.0.1:8794`.
    pub bind: SocketAddr,
    /// `NAPPLE_ACCOUNT_URL`, default `https://pretend.napple.lat` (no trailing slash).
    pub account_url: String,
    /// `NAPPLE_SOCIAL_DATA_DIR`, default `./napple-social-data`. Holds `social.sqlite3`.
    pub data_dir: PathBuf,
    /// `NAPPLE_SOCIAL_HEARTBEAT_SECS`, default 60.
    pub heartbeat_secs: i64,
    /// `NAPPLE_SOCIAL_RATE_PER_MIN`: authenticated requests per account per minute, default 240 (burst = half of it).
    pub rate_per_min: u32,
    /// `NAPPLE_SOCIAL_IP_RATE_PER_MIN`: requests per client IP per minute before authentication, default 600.
    pub ip_rate_per_min: u32,
    /// `NAPPLE_SOCIAL_FRIEND_REQUESTS_PER_HOUR` per profile, default 30.
    pub friend_requests_per_hour: u32,
    /// `NAPPLE_SOCIAL_INVITES_PER_HOUR` per profile, default 120.
    pub invites_per_hour: u32,
    /// `NAPPLE_SOCIAL_TRUST_PROXY=1`: take the client IP from the last `X-Forwarded-For` entry (set it behind a load balancer).
    pub trust_proxy: bool,
    /// Expected `aud` claim. Fixed to "napple" by the contract; kept here so tests can see it.
    pub audience: String,
}

impl Config {
    pub fn from_env() -> Result<Self, String> {
        Self::from_lookup(|k| std::env::var(k).ok())
    }

    pub fn from_lookup(get: impl Fn(&str) -> Option<String>) -> Result<Self, String> {
        let get = |k: &str| {
            get(k)
                .map(|v| v.trim().to_string())
                .filter(|v| !v.is_empty())
        };
        let bind = get("NAPPLE_SOCIAL_BIND").unwrap_or_else(|| "127.0.0.1:8794".into());
        let bind: SocketAddr = bind
            .parse()
            .map_err(|e| format!("NAPPLE_SOCIAL_BIND={bind}: {e}"))?;
        let account_url =
            get("NAPPLE_ACCOUNT_URL").unwrap_or_else(|| "https://pretend.napple.lat".into());
        if !(account_url.starts_with("http://") || account_url.starts_with("https://")) {
            return Err(format!(
                "NAPPLE_ACCOUNT_URL={account_url}: must start with http:// or https://"
            ));
        }
        let account_url = account_url.trim_end_matches('/').to_string();
        let data_dir = PathBuf::from(
            get("NAPPLE_SOCIAL_DATA_DIR").unwrap_or_else(|| "./napple-social-data".into()),
        );
        fn num<T: std::str::FromStr + PartialOrd + Copy>(
            v: Option<String>,
            name: &str,
            default: T,
            min: T,
        ) -> Result<T, String> {
            match v {
                None => Ok(default),
                Some(s) => {
                    let n: T = s.parse().map_err(|_| format!("{name}={s}: not a number"))?;
                    if n < min {
                        return Err(format!("{name}={s}: too small"));
                    }
                    Ok(n)
                }
            }
        }
        let flag = |k: &str| matches!(get(k).as_deref(), Some("1" | "true" | "yes" | "on"));
        Ok(Config {
            bind,
            account_url,
            data_dir,
            heartbeat_secs: num(
                get("NAPPLE_SOCIAL_HEARTBEAT_SECS"),
                "NAPPLE_SOCIAL_HEARTBEAT_SECS",
                60,
                5,
            )?,
            rate_per_min: num(
                get("NAPPLE_SOCIAL_RATE_PER_MIN"),
                "NAPPLE_SOCIAL_RATE_PER_MIN",
                240,
                2,
            )?,
            ip_rate_per_min: num(
                get("NAPPLE_SOCIAL_IP_RATE_PER_MIN"),
                "NAPPLE_SOCIAL_IP_RATE_PER_MIN",
                600,
                2,
            )?,
            friend_requests_per_hour: num(
                get("NAPPLE_SOCIAL_FRIEND_REQUESTS_PER_HOUR"),
                "NAPPLE_SOCIAL_FRIEND_REQUESTS_PER_HOUR",
                30,
                1,
            )?,
            invites_per_hour: num(
                get("NAPPLE_SOCIAL_INVITES_PER_HOUR"),
                "NAPPLE_SOCIAL_INVITES_PER_HOUR",
                120,
                1,
            )?,
            trust_proxy: flag("NAPPLE_SOCIAL_TRUST_PROXY"),
            audience: "napple".into(),
        })
    }

    /// Presence timeout: offline after 3 missed heartbeats.
    pub fn presence_timeout_ms(&self) -> i64 {
        self.heartbeat_secs * 1000 * MISSED_HEARTBEATS
    }

    pub fn jwks_url(&self) -> String {
        format!("{}/account/v1/.well-known/jwks.json", self.account_url)
    }

    pub fn profiles_url(&self) -> String {
        format!("{}/account/v1/profiles", self.account_url)
    }

    pub fn db_path(&self) -> PathBuf {
        self.data_dir.join("social.sqlite3")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn defaults_and_overrides() {
        let c = Config::from_lookup(|_| None).unwrap();
        assert_eq!(c.bind.to_string(), "127.0.0.1:8794");
        assert_eq!(c.account_url, "https://pretend.napple.lat");
        assert_eq!(c.presence_timeout_ms(), 180_000);
        let m: HashMap<&str, &str> = [
            ("NAPPLE_SOCIAL_BIND", "0.0.0.0:9000"),
            ("NAPPLE_ACCOUNT_URL", "http://acct.local:8080/"),
            ("NAPPLE_SOCIAL_HEARTBEAT_SECS", "30"),
            ("NAPPLE_SOCIAL_TRUST_PROXY", "1"),
        ]
        .into();
        let c = Config::from_lookup(|k| m.get(k).map(|s| s.to_string())).unwrap();
        assert_eq!(c.bind.port(), 9000);
        assert_eq!(
            c.jwks_url(),
            "http://acct.local:8080/account/v1/.well-known/jwks.json"
        );
        assert_eq!(
            c.profiles_url(),
            "http://acct.local:8080/account/v1/profiles"
        );
        assert_eq!(c.presence_timeout_ms(), 90_000);
        assert!(c.trust_proxy);
        assert!(
            Config::from_lookup(|k| (k == "NAPPLE_ACCOUNT_URL").then(|| "ftp://x".to_string()))
                .is_err()
        );
        assert!(
            Config::from_lookup(|k| (k == "NAPPLE_SOCIAL_HEARTBEAT_SECS").then(|| "1".to_string()))
                .is_err()
        );
    }
}
