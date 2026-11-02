//! `napple-online serve [--listen ADDR:PORT] [--data DIR] [-v]`: runs the Napple online service.

use napple_online::{accounts::Accounts, server::Server};
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::Arc;

const USAGE: &str = "usage: napple-online serve [--listen ADDR:PORT] [--data DIR] [-v]
  --listen   address and port to listen on (default 127.0.0.1:8737; 0.0.0.0:8737 for a LAN)
  --data     where accounts and the server key are kept (default ~/Library/Application Support/Napple/online-server)
  -v         log every call

Point Napple at it with NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://<address>:<port>. Put a TLS reverse proxy in front of a public server and
use https://<name>. Never contacts Nintendo; see docs/guides/napple-online.md.";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(|s| s.as_str()) != Some("serve") {
        eprintln!("{USAGE}");
        std::process::exit(2);
    }
    let mut listen = "127.0.0.1:8737".to_string();
    let mut data: Option<PathBuf> = None;
    let mut verbose = false;
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
    eprintln!("napple-online: listening on {listen} (data {})", data.display());
    let server = Arc::new(Server::new(accounts, verbose));
    if let Err(e) = napple_online::run(listener, server) {
        eprintln!("napple-online: {e}");
        std::process::exit(1);
    }
}
