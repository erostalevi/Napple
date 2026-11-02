//! Napple Account server (`/account/v1` at `https://pretend.napple.lat`): one identity across Mac, iPhone and the
//! CLI, and end-to-end encrypted cloud saves. The API and security model are in
//! `docs/guides/napple-account.md`; the shared contract is `orchestrations/ACCOUNT-CONTRACT.md`.

pub mod account;
pub mod admin;
pub mod app;
pub mod aws;
pub mod blob;
pub mod config;
pub mod db;
pub mod error;
pub mod jwt;
pub mod keys;
pub mod log;
pub mod mailer;
pub mod names;
pub mod nicknames;
pub mod outbound;
pub mod pages;
pub mod passkey;
pub mod photos;
pub mod profiles;
pub mod retention;
pub mod saves;
pub mod signin;
pub mod util;
pub mod webauthn;

use app::{App, AppState, Clock, RateLimiter, SystemClock};
use blob::{BlobStore, FsBlobStore, S3BlobStore};
use config::{BlobBackend, Config, MailerKind};
use mailer::{DevMailer, Mailer, SesMailer};
use outbound::{Outbound, ReqwestOutbound};
use std::sync::{Arc, Mutex};

/// Everything that talks to the outside world, swappable for tests.
pub struct Deps {
    pub clock: Arc<dyn Clock>,
    pub mailer: Arc<dyn Mailer>,
    pub blobs: Arc<dyn BlobStore>,
    pub dev_blobs: Option<Arc<FsBlobStore>>,
    pub out: Arc<dyn Outbound>,
}

pub fn build_app(
    cfg: Config,
    conn: rusqlite::Connection,
    jwt: jwt::JwtKeys,
    deps: Deps,
) -> AppState {
    let photos = photos::default_store(&cfg.data_dir, &deps.out);
    Arc::new(App {
        cfg,
        db: Mutex::new(conn),
        clock: deps.clock,
        jwt,
        mailer: deps.mailer,
        blobs: deps.blobs,
        dev_blobs: deps.dev_blobs,
        photos,
        out: deps.out,
        limiter: RateLimiter::default(),
        code_key: util::random_bytes::<32>(),
    })
}

/// The real server: data dir, SQLite file, JWT key file, SES or dev mailer, S3 or filesystem blobs.
pub fn from_config(cfg: Config) -> Result<AppState, String> {
    std::fs::create_dir_all(&cfg.data_dir)
        .map_err(|e| format!("{}: {e}", cfg.data_dir.display()))?;
    let conn =
        db::open(&cfg.data_dir.join("account.sqlite3")).map_err(|e| format!("sqlite: {e}"))?;
    let clock: Arc<dyn Clock> = Arc::new(SystemClock);
    let jwt = match &cfg.jwt_seed {
        Some(s) => jwt::JwtKeys::from_seed(
            util::b64_decode(s)
                .and_then(|v| v.try_into().ok())
                .ok_or("NAPPLE_ACCOUNT_JWT_SEED: 32 bytes, base64")?,
        ),
        None => jwt::JwtKeys::load_or_create(&cfg.data_dir, clock.now())?,
    };
    let out: Arc<dyn Outbound> = Arc::new(ReqwestOutbound::new());
    let creds = Arc::new(aws::CredentialProvider::from_env(out.clone()));
    let mailer: Arc<dyn Mailer> = match &cfg.mailer {
        MailerKind::Dev => {
            log::warn("dev mailer: sign-in codes are printed to stdout and no email is sent");
            Arc::new(DevMailer)
        }
        MailerKind::Ses { region, from } => Arc::new(SesMailer {
            region: region.clone(),
            from: from.clone(),
            creds: creds.clone(),
            out: out.clone(),
        }),
    };
    let (blobs, dev_blobs): (Arc<dyn BlobStore>, Option<Arc<FsBlobStore>>) = match &cfg.blobs {
        BlobBackend::Fs(dir) => {
            let fs = Arc::new(
                FsBlobStore::new(dir.clone(), &cfg.public_base)
                    .map_err(|e| format!("{}: {e}", dir.display()))?,
            );
            (fs.clone(), Some(fs))
        }
        BlobBackend::S3(s3) => (
            Arc::new(S3BlobStore::new(s3.clone(), creds.clone(), out.clone())),
            None,
        ),
    };
    Ok(build_app(
        cfg,
        conn,
        jwt,
        Deps {
            clock,
            mailer,
            blobs,
            dev_blobs,
            out,
        },
    ))
}
