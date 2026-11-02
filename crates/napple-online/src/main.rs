//! `napple-online serve [--listen ADDR:PORT] [--data DIR] [-v]`: runs the Napple online service. Every option also has an environment variable
//! (flags win), so a container needs no arguments beyond `serve`.

use napple_online::{accounts::Accounts, natcheck, server::Server};
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::Arc;

const USAGE: &str = "usage: napple-online serve [--listen ADDR:PORT] [--data DIR] [-v]
  --listen   NAPPLE_ONLINE_HTTP   address and port of the one TCP listener: account/token API, GET /healthz, and the NEX game
                                  server (PRUDP Lite inside WebSocket) (default 127.0.0.1:8737; 0.0.0.0:8737 for a LAN)
  --data     NAPPLE_ONLINE_DATA   accounts and the server key, the only state the server keeps
                                  (default ~/Library/Application Support/Napple/online-server)
  -v         NAPPLE_ONLINE_VERBOSE=1  log every call and every packet
             NAPPLE_ONLINE_PUBLIC_HOST  the name clients use (reported by /napple/v1/info)
             NAPPLE_ONLINE_PUBLIC_IP    the address reported for clients on the server's own machine (default: its LAN address)
             NAPPLE_ONLINE_NATCHECK_BASE  first UDP port of the NAT check (default 10025: UDP 10025, 10125, 33334, 33335 and the same + 1000; 0 = off)
Logs go to stdout. Point Napple at it with NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://<address>:<port> (https://<name> behind a TLS proxy that
forwards WebSocket upgrades). Never contacts Nintendo; see docs/guides/napple-online.md.";

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(|s| s.as_str()) != Some("serve") {
        eprintln!("{USAGE}");
        std::process::exit(2);
    }
    let mut listen = env("NAPPLE_ONLINE_HTTP").unwrap_or_else(|| "127.0.0.1:8737".to_string());
    let mut data: Option<PathBuf> = env("NAPPLE_ONLINE_DATA").map(PathBuf::from);
    let mut verbose = env("NAPPLE_ONLINE_VERBOSE").is_some_and(|v| v == "1");
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--listen" if i + 1 < args.len() => {
                listen = args[i + 1].clone();
                i += 1;
            }
            "--data" if i + 1 < args.len() => {
                data = Some(PathBuf::from(&args[i + 1]));
                i += 1;
            }
            "-v" | "--verbose" => verbose = true,
            _ => {
                eprintln!("{USAGE}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let data = data.unwrap_or_else(|| {
        let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
        PathBuf::from(home).join("Library/Application Support/Napple/online-server")
    });
    for unused in ["NAPPLE_ONLINE_NEX_AUTH", "NAPPLE_ONLINE_NEX_SECURE"] {
        if env(unused).is_some() {
            println!("napple-online: {unused} is not used: Switch titles reach NEX through the WebSocket on {listen} (station URLs name the same server)");
        }
    }
    let accounts = match Accounts::open(Some(&data)) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("napple-online: cannot open {}: {e:?}", data.display());
            std::process::exit(1);
        }
    };
    let listener = match TcpListener::bind(&listen) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("napple-online: cannot listen on {listen}: {e}");
            std::process::exit(1);
        }
    };
    println!(
        "napple-online: listening on {listen} (data {}, public host {})",
        data.display(),
        env("NAPPLE_ONLINE_PUBLIC_HOST").unwrap_or_else(|| "-".into())
    );
    // the NAT check titles run before matchmaking (UDP, on the listener's address)
    let nat_base = env("NAPPLE_ONLINE_NATCHECK_BASE").and_then(|v| v.parse::<u16>().ok()).unwrap_or(natcheck::DEFAULT_BASE);
    let ip = listener.local_addr().map(|a| a.ip()).unwrap_or(std::net::IpAddr::from([127, 0, 0, 1]));
    if nat_base != 0 {
        if let Err(e) = natcheck::start(ip, nat_base, verbose) {
            println!("napple-online: NAT check not started (UDP {nat_base}…: {e}); titles that need it cannot matchmake");
        }
    }
    let server = Arc::new(Server::new(accounts, verbose));
    if let Err(e) = napple_online::run(listener, server) {
        eprintln!("napple-online: {e}");
        std::process::exit(1);
    }
}
