//! `napple-account [serve]` runs the server; `napple-account rotate-jwt-key` adds a new signing key to the data dir
//! (restart to start signing with it; the previous keys stay in the JWKS); `napple-account retention` runs one
//! cleanup pass and exits; `napple-account admin grant|revoke|list <email> [grant]` edits account grants
//! (`owner_napple`, `reserved_prefix`, `admin`) directly in the database. All configuration comes from the
//! environment (see `docs/guides/napple-account.md`).

use napple_account::app::{self, Clock};
use napple_account::{config::Config, from_config, log, retention};
use std::net::SocketAddr;

#[tokio::main]
async fn main() {
    let cmd = std::env::args().nth(1).unwrap_or_else(|| "serve".into());
    let cfg = match Config::from_env() {
        Ok(c) => c,
        Err(e) => {
            log::error(&format!("config: {e}"));
            std::process::exit(2);
        }
    };
    match cmd.as_str() {
        "serve" => serve(cfg).await,
        "rotate-jwt-key" => {
            std::fs::create_dir_all(&cfg.data_dir).ok();
            let now = app::SystemClock.now();
            match napple_account::jwt::JwtKeys::rotate(&cfg.data_dir, now) {
                Ok(kid) => println!("new signing key {kid}; restart napple-account to use it"),
                Err(e) => {
                    log::error(&e);
                    std::process::exit(1);
                }
            }
        }
        "retention" => {
            let app = from_config(cfg).unwrap_or_else(|e| {
                log::error(&e);
                std::process::exit(1)
            });
            match retention::run_once(&app).await {
                Ok(r) => println!("{r:?}"),
                Err(e) => {
                    log::error(&e);
                    std::process::exit(1);
                }
            }
        }
        "admin" => {
            let args: Vec<String> = std::env::args().skip(2).collect();
            if let Err(e) = admin_cli(cfg, &args) {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }
        "-h" | "--help" | "help" => println!("{USAGE}"),
        other => {
            eprintln!("unknown command {other}\n{USAGE}");
            std::process::exit(2);
        }
    }
}

const USAGE: &str = "usage: napple-account [serve | rotate-jwt-key | retention | admin grant|revoke <email> <grant> | admin list <email>]";

fn admin_cli(cfg: Config, args: &[String]) -> Result<(), String> {
    use napple_account::admin::{grants_of, resolve, set_grant};
    let (op, email) = match args {
        [op, email, ..] => (op.as_str(), email.as_str()),
        _ => return Err(USAGE.into()),
    };
    let app = from_config(cfg)?;
    let db = app.db();
    let id = resolve(&db, None, Some(email))
        .map_err(|e| e.to_string())?
        .ok_or_else(|| format!("no account for {email}: it has to sign in once first"))?;
    let grants = match (op, args.get(2)) {
        ("grant", Some(g)) => set_grant(&db, &id, g, true, None, app.now())?,
        ("revoke", Some(g)) => set_grant(&db, &id, g, false, None, app.now())?,
        ("list", None) => grants_of(&db, &id).map_err(|e| e.to_string())?,
        _ => return Err(USAGE.into()),
    };
    println!("{id}: [{}]", grants.join(", "));
    Ok(())
}

async fn serve(cfg: Config) {
    let listen = cfg.listen.clone();
    let app = from_config(cfg).unwrap_or_else(|e| {
        log::error(&e);
        std::process::exit(1)
    });
    let bg = app.clone();
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(std::time::Duration::from_secs(3600));
        loop {
            tick.tick().await;
            match retention::run_once(&bg).await {
                Ok(r) => log::info(&format!("retention: {r:?}")),
                Err(e) => log::error(&format!("retention: {e}")),
            }
        }
    });
    let listener = tokio::net::TcpListener::bind(&listen)
        .await
        .unwrap_or_else(|e| {
            log::error(&format!("bind {listen}: {e}"));
            std::process::exit(1)
        });
    log::info(&format!(
        "listening on {listen} (public base {}, blobs {})",
        app.cfg.public_base,
        if app.dev_blobs.is_some() {
            "filesystem (dev)"
        } else {
            "s3"
        }
    ));
    let svc = app::router(app).into_make_service_with_connect_info::<SocketAddr>();
    let shutdown = async {
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("SIGTERM handler");
        tokio::select! { _ = term.recv() => {}, _ = tokio::signal::ctrl_c() => {} }
        log::info("shutting down");
    };
    if let Err(e) = axum::serve(listener, svc)
        .with_graceful_shutdown(shutdown)
        .await
    {
        log::error(&format!("server: {e}"));
        std::process::exit(1);
    }
}
