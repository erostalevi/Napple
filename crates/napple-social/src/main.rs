//! napple-social server binary. Configuration is environment-only (see docs/guides/napple-social.md); logs go to stdout.

#[tokio::main]
async fn main() {
    let filter = tracing_subscriber::EnvFilter::try_from_env("NAPPLE_SOCIAL_LOG")
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info,napple_social=info"));
    tracing_subscriber::fmt().with_env_filter(filter).with_writer(std::io::stdout).with_target(false).init();
    let config = match napple_social::config::Config::from_env() {
        Ok(c) => c,
        Err(e) => {
            tracing::error!("config: {e}");
            std::process::exit(2);
        }
    };
    if let Err(e) = napple_social::run(config).await {
        tracing::error!("{e}");
        std::process::exit(1);
    }
}
