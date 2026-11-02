//! napple-room: serve rooms, list a server's rooms, or probe a server's health.
//!
//! Configuration comes from the environment (NAPPLE_ROOM_*), and flags override it; see USAGE. Logs go to stdout. SIGTERM / SIGINT end the process
//! with status 0 (rooms live in memory only).

use napple_room::{Config, Server, client, proto};
use std::process::ExitCode;
use std::time::Duration;

const USAGE: &str = "usage:
  napple-room [serve] [--listen ADDR] [--http ADDR] [--subnet CIDR] [--max-members N] [--max-rooms N] [--max-clients N]
                      [--rate-kbps N] [--rate-pps N] [--keepalive SECS] [--quiet]
  napple-room health [HOST[:PORT]]     exit 0 when the room service at that address answers (default: the configured listen port on 127.0.0.1)
  napple-room list HOST[:PORT]         print the rooms of a server

Every option can also come from the environment (the flag wins):
  NAPPLE_ROOM_LISTEN       room protocol, TCP            default 0.0.0.0:31415
  NAPPLE_ROOM_HTTP         plain HTTP: GET /healthz, /rooms   default off
  NAPPLE_ROOM_SUBNET       virtual LAN of each room      default 10.13.0.0/16
  NAPPLE_ROOM_MAX_MEMBERS  members per room (2-254)      default 16
  NAPPLE_ROOM_MAX_ROOMS    rooms                         default 256
  NAPPLE_ROOM_MAX_CLIENTS  connections                   default 1024
  NAPPLE_ROOM_RATE_KBPS    relayed KiB/s per member      default 2048
  NAPPLE_ROOM_RATE_PPS     relayed packets/s per member  default 4000
  NAPPLE_ROOM_KEEPALIVE    seconds of silence before a member is dropped (>= 5)   default 30
  NAPPLE_ROOM_QUIET        1 = no log lines
  NAPPLE_ROOM_DATA         accepted and unused: the server keeps no state on disk";

fn with_port(s: &str) -> String {
    if s.rsplit_once(':').map(|(_, p)| p.parse::<u16>().is_ok()).unwrap_or(false) && !s.ends_with(']') {
        s.to_string()
    } else {
        format!("{s}:{}", proto::DEFAULT_PORT)
    }
}

/// Applies one option to the configuration. false = bad value.
fn apply(cfg: &mut Config, key: &str, v: &str) -> bool {
    let num = |v: &str| v.trim().parse::<u64>().ok();
    match key {
        "listen" => with_port(v).parse().map(|a| cfg.listen = a).is_ok(),
        "http" => {
            if v.is_empty() || v == "off" {
                cfg.http = None;
                true
            } else {
                v.parse().map(|a| cfg.http = Some(a)).is_ok()
            }
        }
        "subnet" => proto::parse_cidr(v).map(|s| cfg.subnet = s).is_some(),
        "max-members" => num(v).filter(|n| (2..=254).contains(n)).map(|n| cfg.max_members = n as usize).is_some(),
        "max-rooms" => num(v).filter(|n| *n >= 1).map(|n| cfg.max_rooms = n as usize).is_some(),
        "max-clients" => num(v).filter(|n| *n >= 1).map(|n| cfg.max_clients = n as usize).is_some(),
        "rate-kbps" => num(v)
            .filter(|n| *n >= 1)
            .map(|n| {
                cfg.rate_bytes = n * 1024;
                cfg.burst_bytes = (n * 1024 / 4).max(64 * 1024);
            })
            .is_some(),
        "rate-pps" => num(v)
            .filter(|n| *n >= 1)
            .map(|n| {
                cfg.rate_packets = n;
                cfg.burst_packets = (n / 4).max(64);
            })
            .is_some(),
        "keepalive" => num(v).filter(|n| *n >= 5).map(|n| cfg.keepalive = Duration::from_secs(n)).is_some(),
        "quiet" => {
            cfg.quiet = v != "0";
            true
        }
        _ => false,
    }
}
const ENV: &[(&str, &str)] = &[
    ("NAPPLE_ROOM_LISTEN", "listen"),
    ("NAPPLE_ROOM_HTTP", "http"),
    ("NAPPLE_ROOM_SUBNET", "subnet"),
    ("NAPPLE_ROOM_MAX_MEMBERS", "max-members"),
    ("NAPPLE_ROOM_MAX_ROOMS", "max-rooms"),
    ("NAPPLE_ROOM_MAX_CLIENTS", "max-clients"),
    ("NAPPLE_ROOM_RATE_KBPS", "rate-kbps"),
    ("NAPPLE_ROOM_RATE_PPS", "rate-pps"),
    ("NAPPLE_ROOM_KEEPALIVE", "keepalive"),
    ("NAPPLE_ROOM_QUIET", "quiet"),
];
fn config_from_env() -> Result<Config, String> {
    let mut cfg = Config::default();
    for (var, key) in ENV {
        if let Ok(v) = std::env::var(var) {
            if !apply(&mut cfg, key, &v) {
                return Err(format!("bad {var}={v}"));
            }
        }
    }
    Ok(cfg)
}

// SIGTERM / SIGINT: a container stop is a normal end. std has no signal API; libc's signal() and _exit() are on every target this builds for.
unsafe extern "C" {
    fn signal(sig: i32, handler: extern "C" fn(i32)) -> usize;
    fn _exit(status: i32) -> !;
}
extern "C" fn on_term(_: i32) {
    unsafe { _exit(0) }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut it = args.iter().map(String::as_str).peekable();
    let mut cfg = match config_from_env() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("napple-room: {e}\n{USAGE}");
            return ExitCode::from(2);
        }
    };
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
        Some(&"health") => {
            it.next();
            let target = match it.next() {
                Some(h) => with_port(h),
                None => format!("127.0.0.1:{}", cfg.listen.port()),
            };
            return match client::list(&target) {
                Ok(rooms) => {
                    println!("ok: {target} answers ({} room(s))", rooms.len());
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    println!("unhealthy: {target}: {e:?}");
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
    while let Some(a) = it.next() {
        let ok = match a.strip_prefix("--") {
            Some("quiet") => apply(&mut cfg, "quiet", "1"),
            Some(key) => match it.next() {
                Some(v) => apply(&mut cfg, key, v),
                None => false,
            },
            None => false,
        };
        if !ok {
            eprintln!("napple-room: bad argument {a}\n{USAGE}");
            return ExitCode::from(2);
        }
    }
    unsafe {
        signal(15, on_term);
        signal(2, on_term);
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
