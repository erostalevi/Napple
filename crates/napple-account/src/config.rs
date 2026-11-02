//! Env-only configuration. Every knob has a default that works for a local dev run; production sets the AWS/Stripe ones.

use std::collections::HashMap;
use std::path::PathBuf;

const MIB: u64 = 1024 * 1024;
const GIB: u64 = 1024 * MIB;

#[derive(Clone, Debug)]
pub struct PlanLimits {
    /// 0 = unlimited.
    pub synced_titles: u32,
    pub versions_per_title: u32,
    pub quota_bytes: u64,
}

#[derive(Clone, Debug)]
pub struct StripeConfig {
    pub secret_key: String,
    pub webhook_secret: String,
    pub price_monthly: String,
    pub price_yearly: String,
    pub api_base: String,
}

#[derive(Clone, Debug)]
pub enum BlobBackend {
    /// Local directory; the server itself serves the "presigned" URLs under `/account/v1/dev-blobs/`.
    Fs(PathBuf),
    S3(S3Config),
}

#[derive(Clone, Debug)]
pub struct S3Config {
    pub bucket: String,
    pub region: String,
    /// Override for S3-compatible endpoints and test mocks (path-style addressing). None = AWS virtual-hosted style.
    pub endpoint: Option<String>,
}

#[derive(Clone, Debug)]
pub enum MailerKind {
    /// Prints the code and link to stdout. Refused when `NAPPLE_ACCOUNT_ENV=production`.
    Dev,
    Ses {
        region: String,
        from: String,
    },
}

#[derive(Clone, Debug)]
pub struct Config {
    pub listen: String,
    pub data_dir: PathBuf,
    pub production: bool,
    /// Public origin + prefix, e.g. `https://pretend.napple.lat/account/v1`; used in links, JWT `iss`, presign-dev URLs.
    pub public_base: String,
    pub issuer: String,
    pub link_base: String,
    pub rp_id: String,
    pub rp_name: String,
    pub webauthn_origins: Vec<String>,
    pub cors_origins: Vec<String>,
    pub trust_proxy: bool,
    pub access_ttl: i64,
    pub refresh_ttl: i64,
    pub email_code_ttl: i64,
    pub email_code_attempts: u32,
    pub max_version_bytes: u64,
    pub upload_url_ttl: i64,
    pub pending_upload_ttl: i64,
    pub free: PlanLimits,
    pub plus: PlanLimits,
    pub title_switch_days: i64,
    pub readonly_days: i64,
    pub deletion_warning_days: i64,
    pub max_profiles: u32,
    pub stripe: Option<StripeConfig>,
    pub blobs: BlobBackend,
    pub mailer: MailerKind,
    /// Optional base64 32-byte Ed25519 seed; otherwise a key file in `data_dir` is created on first start.
    pub jwt_seed: Option<String>,
    pub auth_ip_per_min: u32,
    pub email_per_window: u32,
}

impl Config {
    pub fn from_env() -> Result<Config, String> {
        Config::from_vars(std::env::vars().collect())
    }

    pub fn from_vars(v: HashMap<String, String>) -> Result<Config, String> {
        let get = |k: &str| {
            v.get(k)
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
        };
        let num = |k: &str, d: u64| -> Result<u64, String> {
            match get(k) {
                Some(s) => parse_size(&s).ok_or_else(|| format!("{k}: not a number/size: {s}")),
                None => Ok(d),
            }
        };
        let list = |k: &str, d: &str| -> Vec<String> {
            get(k)
                .unwrap_or_else(|| d.to_string())
                .split(',')
                .map(|s| s.trim().trim_end_matches('/').to_string())
                .filter(|s| !s.is_empty())
                .collect()
        };
        let production = get("NAPPLE_ACCOUNT_ENV").as_deref() == Some("production");
        let public_base = get("NAPPLE_ACCOUNT_PUBLIC_BASE")
            .unwrap_or_else(|| "https://pretend.napple.lat/account/v1".into())
            .trim_end_matches('/')
            .to_string();

        let stripe = match (get("STRIPE_SECRET_KEY"), get("STRIPE_WEBHOOK_SECRET"), get("STRIPE_PRICE_MONTHLY"), get("STRIPE_PRICE_YEARLY")) {
            (Some(secret_key), Some(webhook_secret), Some(price_monthly), Some(price_yearly)) => Some(StripeConfig {
                secret_key,
                webhook_secret,
                price_monthly,
                price_yearly,
                api_base: get("STRIPE_API_BASE").unwrap_or_else(|| "https://api.stripe.com".into()).trim_end_matches('/').to_string(),
            }),
            (None, None, None, None) => None,
            _ => return Err("Stripe needs all of STRIPE_SECRET_KEY, STRIPE_WEBHOOK_SECRET, STRIPE_PRICE_MONTHLY, STRIPE_PRICE_YEARLY (or none)".into()),
        };

        let data_dir = PathBuf::from(
            get("NAPPLE_ACCOUNT_DATA").unwrap_or_else(|| "./napple-account-data".into()),
        );
        let blobs = match get("NAPPLE_ACCOUNT_S3_BUCKET") {
            Some(bucket) => BlobBackend::S3(S3Config {
                bucket,
                region: get("NAPPLE_ACCOUNT_S3_REGION")
                    .or_else(|| get("AWS_REGION"))
                    .unwrap_or_else(|| "us-east-1".into()),
                endpoint: get("NAPPLE_ACCOUNT_S3_ENDPOINT")
                    .map(|s| s.trim_end_matches('/').to_string()),
            }),
            None => BlobBackend::Fs(
                get("NAPPLE_ACCOUNT_BLOB_DIR")
                    .map(PathBuf::from)
                    .unwrap_or_else(|| data_dir.join("blobs")),
            ),
        };
        let mailer = match get("NAPPLE_ACCOUNT_MAILER").as_deref() {
            Some("ses") => MailerKind::Ses {
                region: get("NAPPLE_ACCOUNT_SES_REGION")
                    .or_else(|| get("AWS_REGION"))
                    .unwrap_or_else(|| "us-east-1".into()),
                from: get("NAPPLE_ACCOUNT_MAIL_FROM")
                    .unwrap_or_else(|| "Napple <no-reply@napple.lat>".into()),
            },
            Some("dev") | None => MailerKind::Dev,
            Some(other) => {
                return Err(format!(
                    "NAPPLE_ACCOUNT_MAILER: unknown mailer {other} (dev|ses)"
                ));
            }
        };
        if production && matches!(mailer, MailerKind::Dev) {
            return Err("NAPPLE_ACCOUNT_ENV=production needs NAPPLE_ACCOUNT_MAILER=ses (the dev mailer prints sign-in codes)".into());
        }
        if production && matches!(blobs, BlobBackend::Fs(_)) {
            return Err("NAPPLE_ACCOUNT_ENV=production needs NAPPLE_ACCOUNT_S3_BUCKET".into());
        }

        Ok(Config {
            listen: get("NAPPLE_ACCOUNT_HTTP").unwrap_or_else(|| "127.0.0.1:8082".into()),
            data_dir,
            production,
            issuer: get("NAPPLE_ACCOUNT_ISSUER").unwrap_or_else(|| public_base.clone()),
            link_base: get("NAPPLE_ACCOUNT_LINK_BASE")
                .unwrap_or_else(|| format!("{public_base}/auth/email/link")),
            public_base,
            rp_id: get("NAPPLE_ACCOUNT_RP_ID").unwrap_or_else(|| "napple.lat".into()),
            rp_name: get("NAPPLE_ACCOUNT_RP_NAME").unwrap_or_else(|| "Napple".into()),
            webauthn_origins: list(
                "NAPPLE_ACCOUNT_WEBAUTHN_ORIGINS",
                "https://pretend.napple.lat,https://napple.lat",
            ),
            cors_origins: list(
                "NAPPLE_ACCOUNT_CORS_ORIGINS",
                "https://napple.lat,https://pretend.napple.lat,https://www.napple.lat",
            ),
            trust_proxy: get("NAPPLE_ACCOUNT_TRUST_PROXY")
                .as_deref()
                .is_some_and(|s| s == "1" || s == "true"),
            access_ttl: num("NAPPLE_ACCOUNT_ACCESS_TTL", 15 * 60)? as i64,
            refresh_ttl: num("NAPPLE_ACCOUNT_REFRESH_TTL", 30 * 86_400)? as i64,
            email_code_ttl: num("NAPPLE_ACCOUNT_EMAIL_CODE_TTL", 10 * 60)? as i64,
            email_code_attempts: num("NAPPLE_ACCOUNT_EMAIL_CODE_ATTEMPTS", 5)? as u32,
            max_version_bytes: num("NAPPLE_ACCOUNT_MAX_VERSION_BYTES", 64 * MIB)?,
            upload_url_ttl: num("NAPPLE_ACCOUNT_UPLOAD_URL_TTL", 15 * 60)? as i64,
            pending_upload_ttl: num("NAPPLE_ACCOUNT_PENDING_UPLOAD_TTL", 86_400)? as i64,
            free: PlanLimits {
                synced_titles: num("NAPPLE_PLAN_FREE_TITLES", 1)? as u32,
                versions_per_title: num("NAPPLE_PLAN_FREE_VERSIONS", 10)? as u32,
                quota_bytes: num("NAPPLE_PLAN_FREE_QUOTA", 256 * MIB)?,
            },
            plus: PlanLimits {
                synced_titles: num("NAPPLE_PLAN_PLUS_TITLES", 0)? as u32,
                versions_per_title: num("NAPPLE_PLAN_PLUS_VERSIONS", 20)? as u32,
                quota_bytes: num("NAPPLE_PLAN_PLUS_QUOTA", 5 * GIB)?,
            },
            title_switch_days: num("NAPPLE_PLAN_TITLE_SWITCH_DAYS", 30)? as i64,
            readonly_days: num("NAPPLE_PLAN_READONLY_DAYS", 90)? as i64,
            deletion_warning_days: num("NAPPLE_PLAN_DELETION_WARNING_DAYS", 7)? as i64,
            max_profiles: num("NAPPLE_ACCOUNT_MAX_PROFILES", 8)? as u32,
            stripe,
            blobs,
            mailer,
            jwt_seed: get("NAPPLE_ACCOUNT_JWT_SEED"),
            auth_ip_per_min: num("NAPPLE_ACCOUNT_AUTH_IP_PER_MIN", 30)? as u32,
            email_per_window: num("NAPPLE_ACCOUNT_EMAIL_PER_15MIN", 5)? as u32,
        })
    }

    pub fn limits(&self, plus: bool) -> &PlanLimits {
        if plus { &self.plus } else { &self.free }
    }

    /// The public origin (scheme://host[:port]) of `public_base`.
    pub fn public_origin(&self) -> String {
        match self.public_base.find("://") {
            Some(i) => match self.public_base[i + 3..].find('/') {
                Some(j) => self.public_base[..i + 3 + j].to_string(),
                None => self.public_base.clone(),
            },
            None => self.public_base.clone(),
        }
    }
}

/// `1048576`, `64MiB`, `5GiB`, `256M`, `1G`, `512K`.
pub fn parse_size(s: &str) -> Option<u64> {
    let s = s.trim();
    let split = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    let (n, unit) = s.split_at(split);
    let n: u64 = n.parse().ok()?;
    let mul = match unit.trim().to_ascii_lowercase().as_str() {
        "" | "b" => 1,
        "k" | "kib" | "kb" => 1024,
        "m" | "mib" | "mb" => MIB,
        "g" | "gib" | "gb" => GIB,
        _ => return None,
    };
    n.checked_mul(mul)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes() {
        assert_eq!(parse_size("64MiB"), Some(64 * MIB));
        assert_eq!(parse_size("5GiB"), Some(5 * GIB));
        assert_eq!(parse_size("900"), Some(900));
        assert_eq!(parse_size("1x"), None);
    }

    #[test]
    fn defaults_and_stripe_all_or_nothing() {
        let c = Config::from_vars(HashMap::new()).unwrap();
        assert!(c.stripe.is_none());
        assert_eq!(c.free.synced_titles, 1);
        assert_eq!(c.plus.versions_per_title, 20);
        assert_eq!(c.public_origin(), "https://pretend.napple.lat");
        let mut v = HashMap::new();
        v.insert("STRIPE_SECRET_KEY".to_string(), "sk_test_x".to_string());
        assert!(Config::from_vars(v).is_err());
        let mut p = HashMap::new();
        p.insert("NAPPLE_ACCOUNT_ENV".to_string(), "production".to_string());
        assert!(
            Config::from_vars(p).is_err(),
            "production refuses the dev mailer"
        );
    }
}
