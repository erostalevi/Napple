//! napple-cloud-stub [ADDR] [--code CODE] [--quota BYTES] [--versions N] [--access-ttl SECS] [--quiet]
//! A local stand-in for the Napple Account service (see crates/napple-cloud/src/stub.rs). Prints the base URL to use as NAPPLE_ACCOUNT_URL.
fn main() {
    let mut addr = "127.0.0.1:8787".to_string();
    let mut opts = napple_cloud::stub::Options::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--code" => opts.fixed_code = args.next(),
            "--quota" => opts.quota_bytes = args.next().and_then(|s| s.parse().ok()),
            "--versions" => opts.versions = args.next().and_then(|s| s.parse().ok()),
            "--access-ttl" => opts.access_ttl = args.next().and_then(|s| s.parse().ok()),
            "--quiet" => opts.quiet = true,
            _ if !a.starts_with('-') => addr = a,
            _ => {
                eprintln!("usage: napple-cloud-stub [ADDR] [--code CODE] [--quota BYTES] [--versions N] [--access-ttl SECS] [--quiet]");
                std::process::exit(2);
            }
        }
    }
    let s = napple_cloud::stub::Stub::start(&addr, opts).unwrap_or_else(|e| {
        eprintln!("napple-cloud-stub: cannot listen on {addr}: {e}");
        std::process::exit(1);
    });
    println!("napple-cloud-stub: NAPPLE_ACCOUNT_URL={}", s.base);
    s.join();
}
