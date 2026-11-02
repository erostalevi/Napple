//! `napple-account [serve]` runs the server; `napple-account rotate-jwt-key` adds a new signing key to the data dir
//! (restart to start signing with it; the previous keys stay in the JWKS); `napple-account retention` runs one
//! cleanup pass and exits. All configuration comes from the environment (see `docs/guides/napple-account.md`).

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
        "-h" | "--help" | "help" => {
            println!("usage: napple-account [serve | rotate-jwt-key | retention]")
        }
        other => {
            eprintln!(
                "unknown command {other}; usage: napple-account [serve | rotate-jwt-key | retention]"
            );
            std::process::exit(2);
        }
    }
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
        "listening on {listen} (public base {}, billing {}, blobs {})",
        app.cfg.public_base,
        if app.cfg.stripe.is_some() {
            "on"
        } else {
            "off"
        },
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
