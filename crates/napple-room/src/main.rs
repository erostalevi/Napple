//! napple-room: serve rooms, or list the rooms of a server.
//!
//!   napple-room [serve] [--listen ADDR] [--subnet CIDR] [--max-members N] [--max-rooms N] [--max-clients N]
//!                       [--rate-kbps N] [--rate-pps N] [--keepalive SECS] [--quiet]
//!   napple-room list HOST[:PORT]

use napple_room::{Config, Server, client, proto};
use std::process::ExitCode;
use std::time::Duration;

const USAGE: &str = "usage:
  napple-room [serve] [--listen ADDR] [--subnet CIDR] [--max-members N] [--max-rooms N] [--max-clients N]
                      [--rate-kbps N] [--rate-pps N] [--keepalive SECS] [--quiet]
  napple-room list HOST[:PORT]

defaults: --listen 0.0.0.0:31415 --subnet 10.13.0.0/16 --max-members 16 --max-rooms 256 --max-clients 1024
          --rate-kbps 2048 --rate-pps 4000 --keepalive 30";

fn with_port(s: &str) -> String {
    if s.rsplit_once(':').map(|(_, p)| p.parse::<u16>().is_ok()).unwrap_or(false) && !s.ends_with(']') {
        s.to_string()
    } else {
        format!("{s}:{}", proto::DEFAULT_PORT)
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut it = args.iter().map(String::as_str).peekable();
    match it.peek() {
        Some(&"list") => {
            it.next();
            let Some(host) = it.next() else {
                eprintln!("{USAGE}");
                return ExitCode::from(2);
            };
            return match client::list(with_port(host)) {
                Ok(rooms) => {
                    println!("{} room(s)", rooms.len());
                    for r in rooms {
                        println!("  {:<32} {:>3}/{:<3} {}", r.name, r.members, r.max_members, if r.password { "password" } else { "open" });
                    }
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("napple-room: {e:?}");
                    ExitCode::FAILURE
                }
            };
        }
        Some(&"serve") => {
            it.next();
        }
        Some(&"-h") | Some(&"--help") => {
            println!("{USAGE}");
            return ExitCode::SUCCESS;
        }
        _ => {}
    }
    let mut cfg = Config::default();
    while let Some(a) = it.next() {
        let mut val = || it.next().map(str::to_string);
        let ok = match a {
            "--listen" => val().and_then(|v| with_port(&v).parse().ok()).map(|v| cfg.listen = v).is_some(),
            "--subnet" => val().and_then(|v| proto::parse_cidr(&v)).map(|v| cfg.subnet = v).is_some(),
            "--max-members" => val().and_then(|v| v.parse().ok()).filter(|n| (2..=254).contains(n)).map(|n| cfg.max_members = n).is_some(),
            "--max-rooms" => val().and_then(|v| v.parse().ok()).filter(|n| *n >= 1).map(|n| cfg.max_rooms = n).is_some(),
            "--max-clients" => val().and_then(|v| v.parse().ok()).filter(|n| *n >= 1).map(|n| cfg.max_clients = n).is_some(),
            "--rate-kbps" => val().and_then(|v| v.parse::<u64>().ok()).filter(|n| *n >= 1).map(|n| {
                cfg.rate_bytes = n * 1024;
                cfg.burst_bytes = (n * 1024 / 4).max(64 * 1024);
            }).is_some(),
            "--rate-pps" => val().and_then(|v| v.parse::<u64>().ok()).filter(|n| *n >= 1).map(|n| {
                cfg.rate_packets = n;
                cfg.burst_packets = (n / 4).max(64);
            }).is_some(),
            "--keepalive" => val().and_then(|v| v.parse::<u64>().ok()).filter(|n| *n >= 5).map(|n| cfg.keepalive = Duration::from_secs(n)).is_some(),
            "--quiet" => {
                cfg.quiet = true;
                true
            }
            _ => false,
        };
        if !ok {
            eprintln!("napple-room: bad argument {a}\n{USAGE}");
            return ExitCode::from(2);
        }
    }
    match Server::bind(cfg) {
        Ok(s) => {
            s.run();
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("napple-room: {e}");
            ExitCode::FAILURE
        }
    }
}
