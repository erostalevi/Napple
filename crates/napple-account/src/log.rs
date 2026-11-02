//! One-line logs to stdout/stderr. Never pass secrets (tokens, codes, keys, signatures) or email addresses here.

fn line(level: &str, msg: &str) -> String {
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    format!("{} napple-account {level} {msg}", crate::util::amz_date(ts))
}

pub fn info(msg: &str) {
    if std::env::var_os("NAPPLE_ACCOUNT_QUIET").is_none() {
        println!("{}", line("info", msg));
    }
}

pub fn warn(msg: &str) {
    eprintln!("{}", line("warn", msg));
}

pub fn error(msg: &str) {
    eprintln!("{}", line("error", msg));
}
