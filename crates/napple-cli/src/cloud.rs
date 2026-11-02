//! `napple account ...` and `napple saves ...`: the Napple Account and its end-to-end encrypted cloud saves (crates/napple-cloud,
//! docs/guides/cloud-saves.md). The CLI is the same device as Napple Studio and the games built on this Mac (same Keychain items).
use napple_cloud::api::str_field;
use napple_cloud::session::{self, KeySetup, Session};
use napple_cloud::state;
use napple_cloud::sync::{self, PullResult, PushResult, Target, DEFAULT_UID};
use napple_cloud::{keys, util, Error};
use std::io::{BufRead, Write};
use std::path::PathBuf;

const ACCOUNT_USAGE: &str = "usage: napple account <command>
  login [--email E] [--code C] [--server URL] [--name DEVICE]   sign in with an emailed code (this Mac becomes a device)
  logout                         sign out (local saves and restore points stay)
  status                         account, device, key and sync state
  devices [revoke <device-id>]   devices of the account, and devices waiting for approval
  approve [<device-id>]          give waiting devices the cloud save key (end-to-end encryption)
  recover [--phrase \"24 words\"]  unlock this device with the recovery code instead of an approval
  profiles [add <nickname>]      Napple player profiles (up to 8)
  link <profile-id> [--user N|UID]  link a local user (default: the first) to a profile
environment: NAPPLE_ACCOUNT_URL (server), NAPPLE_ACCOUNT_NAMESPACE (another sign-in on this machine), NAPPLE_SUPPORT_DIR";

const SAVES_USAGE: &str = "usage: napple saves <command> [--save-root DIR] [--user N|UID]
  list                     cloud and local saves with their sync status
  pull <title-id>          download the newest cloud save (the local one is kept as a restore point if it differs)
  push <title-id>          upload the local save
  history <title-id>       cloud versions and local restore points
  restore <title-id> <version | restore-point file>   make an older save current (and upload it)
  enable <title-id> | disable <title-id>              turn syncing a title on or off (default on)
--save-root: the games' save root (default ~/Library/Application Support/Napple, as the runtime's --save-root)";

struct Opts {
    pos: Vec<String>,
    flags: std::collections::HashMap<String, String>,
    switches: Vec<String>,
}

fn opts(args: &[String], with_value: &[&str]) -> Opts {
    let mut o = Opts { pos: vec![], flags: Default::default(), switches: vec![] };
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if let Some(name) = a.strip_prefix("--") {
            if with_value.contains(&name) && i + 1 < args.len() {
                o.flags.insert(name.to_string(), args[i + 1].clone());
                i += 1;
            } else {
                o.switches.push(name.to_string());
            }
        } else {
            o.pos.push(a.clone());
        }
        i += 1;
    }
    o
}

fn prompt(q: &str) -> Result<String, String> {
    eprint!("{q}");
    std::io::stderr().flush().ok();
    let mut s = String::new();
    std::io::stdin().lock().read_line(&mut s).map_err(|e| e.to_string())?;
    Ok(s.trim().to_string())
}

fn e(e: Error) -> String {
    e.to_string()
}

fn support() -> PathBuf {
    state::default_support_dir()
}

fn open() -> Result<Session, String> {
    Session::open(&support()).map_err(e)
}

/// The runtime's default save root (main.c): ~/Library/Application Support/Napple, whatever NAPPLE_SUPPORT_DIR says.
fn save_root(o: &Opts) -> PathBuf {
    if let Some(r) = o.flags.get("save-root") {
        return PathBuf::from(r);
    }
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join("Library/Application Support/Napple")
}

/// --user N (0-based index of the runtime's local users) or a 32-hex uid; default the first user.
fn user(o: &Opts) -> Result<String, String> {
    match o.flags.get("user") {
        None => Ok(DEFAULT_UID.to_string()),
        Some(u) if u.len() == 32 && util::unhex(u).is_some() => Ok(u.to_ascii_lowercase()),
        Some(u) => {
            let i: u8 = u.parse().map_err(|_| format!("--user: an index 0..7 or a 32-hex-digit user id, not {u}"))?;
            if i > 7 {
                return Err("--user: at most 8 local users".into());
            }
            Ok(format!("{}{:02x}", &DEFAULT_UID[..30], 1 + i))
        }
    }
}

pub fn account(args: &[String]) -> Result<(), String> {
    let cmd = args.first().map(String::as_str).unwrap_or("");
    let o = opts(args.get(1..).unwrap_or(&[]), &["email", "code", "server", "name", "user", "phrase"]);
    match cmd {
        "login" => login(&o),
        "logout" => {
            let s = open()?;
            let email = s.cfg().email;
            s.logout().map_err(e)?;
            println!("signed out of {email} (local saves and restore points are kept)");
            Ok(())
        }
        "status" => status(),
        "devices" => {
            let s = open()?;
            if o.pos.first().map(String::as_str) == Some("revoke") {
                let id = o.pos.get(1).ok_or("usage: napple account devices revoke <device-id>")?;
                s.call("DELETE", &format!("/devices/{id}"), None).map_err(e)?;
                println!("revoked {id}");
                return Ok(());
            }
            let me = s.cfg().device_id;
            let v = s.call("GET", "/devices", None).map_err(e)?;
            for d in napple_cloud::api::list(&v, "devices") {
                let id = str_field(d, &["device_id", "id"]).unwrap_or("?");
                println!(
                    "{id}  {:<24} {:<6}{}",
                    str_field(d, &["name"]).unwrap_or(""),
                    str_field(d, &["platform"]).unwrap_or(""),
                    if id == me { "  (this device)" } else { "" }
                );
            }
            let pending = s.key_requests().map_err(e)?;
            if !pending.is_empty() {
                println!("\nwaiting for approval (napple account approve):");
                for r in pending {
                    println!("  {}  {}", str_field(&r, &["device_id", "id"]).unwrap_or("?"), str_field(&r, &["name"]).unwrap_or(""));
                }
            }
            Ok(())
        }
        "approve" => {
            let s = open()?;
            let done = s.approve(o.pos.first().map(String::as_str)).map_err(e)?;
            if done.is_empty() {
                println!("no device is waiting for approval");
            }
            for d in done {
                println!("approved {d}: it can now decrypt the account's cloud saves");
            }
            Ok(())
        }
        "recover" => {
            let s = open()?;
            let phrase = match o.flags.get("phrase") {
                Some(p) => p.clone(),
                None => prompt("recovery code (24 words): ")?,
            };
            s.recover(&phrase).map_err(e)?;
            println!("this device is unlocked: cloud saves can be decrypted here");
            Ok(())
        }
        "profiles" => {
            let s = open()?;
            if o.pos.first().map(String::as_str) == Some("add") {
                let nick = o.pos.get(1).ok_or("usage: napple account profiles add <nickname>")?;
                println!("{}", s.create_profile(nick).map_err(e)?);
                return Ok(());
            }
            let links = s.cfg().links;
            for p in s.profiles().map_err(e)? {
                let id = str_field(&p, &["profile_id", "id"]).unwrap_or("?");
                let users: Vec<&str> = links.iter().filter(|(_, v)| v.as_str() == id).map(|(k, _)| k.as_str()).collect();
                let linked = if users.is_empty() { String::new() } else { format!("  linked to local user {}", users.join(", ")) };
                println!("{id}  {}{linked}", str_field(&p, &["nickname"]).unwrap_or(""));
            }
            Ok(())
        }
        "link" => {
            let s = open()?;
            let pid = o.pos.first().ok_or("usage: napple account link <profile-id> [--user N|UID]")?;
            let uid = user(&o)?;
            s.link(&uid, Some(pid)).map_err(e)?;
            println!("local user {uid} -> profile {pid}");
            Ok(())
        }
        _ => Err(ACCOUNT_USAGE.into()),
    }
}

fn login(o: &Opts) -> Result<(), String> {
    let server = o.flags.get("server").cloned().unwrap_or_else(session::default_server);
    let email = match o.flags.get("email") {
        Some(m) => m.clone(),
        None => prompt("email: ")?,
    };
    if !email.contains('@') {
        return Err("an email address is needed".into());
    }
    session::email_start(&server, &email).map_err(e)?;
    let code = match o.flags.get("code") {
        Some(c) => c.clone(),
        None => prompt(&format!("a sign-in code was sent to {email}; code: "))?,
    };
    let name = o.flags.get("name").cloned().unwrap_or_else(session::default_device_name);
    let ns = state::namespace();
    let s = Session::email_finish(&support(), &ns, &server, &email, &code, &name).map_err(e)?;
    let c = s.cfg();
    println!("signed in as {email} (device {} \"{}\", secrets in {})", c.device_id, c.device_name, s.store.describe());
    match s.setup_keys(keys::RECOVERY_COST).map_err(e)? {
        KeySetup::Ready => println!("cloud save key: received (this device is approved)"),
        KeySetup::CreatedFirst { recovery_phrase } => {
            println!("cloud save key: created (first device of this account)");
            println!("\nRECOVERY CODE (shown once; write it down, it is the only way back in if you lose every device):\n");
            for (i, w) in recovery_phrase.split(' ').enumerate() {
                print!("{:>2}. {w:<10}{}", i + 1, if i % 6 == 5 { "\n" } else { " " });
            }
            println!();
        }
        KeySetup::WaitingApproval => println!(
            "cloud save key: waiting. On a device that is already signed in run `napple account approve`, or enter the recovery code here with `napple account recover`."
        ),
    }
    match s.link(DEFAULT_UID, None) {
        Ok(p) => println!("local user {DEFAULT_UID} -> profile {p}"),
        Err(err) => println!("profile link: {err}"),
    }
    Ok(())
}

fn status() -> Result<(), String> {
    let s = match open() {
        Ok(s) => s,
        Err(m) => {
            println!("{m}");
            return Ok(());
        }
    };
    let me = s.me();
    let c = s.cfg();
    println!("account:  {} ({})", c.email, c.account_id);
    println!("server:   {}", c.server);
    println!("device:   {} \"{}\" ({})", c.device_id, c.device_name, session::platform());
    println!("secrets:  {}", s.store.describe());
    let key = match s.account_key() {
        Ok(_) => "ready".to_string(),
        Err(Error::NoAccountKey) => "waiting for approval (napple account approve on another device, or napple account recover)".into(),
        Err(err) => format!("unknown ({err})"),
    };
    println!("save key: {key}");
    if let Err(err) = me {
        println!("          (server: {err})");
    }
    for (u, p) in &c.links {
        println!("link:     local user {u} -> profile {p}");
    }
    for (t, _) in c.titles.iter().filter(|(_, t)| !t.enabled) {
        println!("sync off: {t}");
    }
    let q = s.dir.load_queue();
    if !q.is_empty() {
        println!("queued:   {} push(es) waiting for the network", q.len());
    }
    Ok(())
}

fn title_arg(o: &Opts, i: usize) -> Result<String, String> {
    let t = o.pos.get(i).ok_or(SAVES_USAGE)?;
    util::normalize_title(t).ok_or_else(|| format!("{t}: a title id is 16 hex digits"))
}

fn target(s: &Session, o: &Opts, title: &str) -> Result<Target, String> {
    let uid = user(o)?;
    let profile = s.cfg().links.get(&uid).cloned().ok_or_else(|| format!("local user {uid} is not linked to a profile (napple account link)"))?;
    Ok(Target { title: title.to_string(), profile, uid, save_dir: save_root(o).join(title).join("save") })
}

pub fn saves(args: &[String]) -> Result<(), String> {
    let cmd = args.first().map(String::as_str).unwrap_or("");
    let o = opts(args.get(1..).unwrap_or(&[]), &["save-root", "user"]);
    match cmd {
        "list" => list(&o),
        "pull" => {
            let s = open()?;
            let t = target(&s, &o, &title_arg(&o, 0)?)?;
            match sync::pull(&s, &t, None).map_err(e)? {
                PullResult::Pulled { version, restore_point } => {
                    println!("{}: version {version} installed in {}", t.title, t.save_dir.display());
                    if let Some(p) = restore_point {
                        println!("the previous local save was kept as a restore point: {}", p.display());
                    }
                }
                PullResult::UpToDate => println!("{}: up to date", t.title),
                PullResult::NoRemote => println!("{}: nothing in the cloud yet", t.title),
                PullResult::LocalNewer => println!("{}: the local save is newer (napple saves push {})", t.title, t.title),
                PullResult::Disabled => println!("{}: sync is turned off (napple saves enable {})", t.title, t.title),
            }
            Ok(())
        }
        "push" => {
            let s = open()?;
            let t = target(&s, &o, &title_arg(&o, 0)?)?;
            print_push(&t, sync::push(&s, &t, true).map_err(e)?);
            Ok(())
        }
        "history" => {
            let s = open()?;
            let t = target(&s, &o, &title_arg(&o, 0)?)?;
            let st = s.dir.load_sync(&t.title, &t.profile);
            let (vs, head) = sync::versions(&s, &t).map_err(e)?;
            println!("cloud versions of {} (profile {}):", t.title, t.profile);
            for v in &vs {
                let n = v["version"].as_u64().unwrap_or(0);
                let mark = if n == st.version { "  <- this device" } else if n == head { "  <- newest" } else { "" };
                println!(
                    "  v{n:<4} {:>9} bytes  {}  {}{mark}",
                    v["size"].as_u64().unwrap_or(0),
                    v["created_at"].as_u64().map(|t| t.to_string()).or_else(|| v["created_at"].as_str().map(str::to_string)).unwrap_or_default(),
                    str_field(v, &["device_id"]).unwrap_or("")
                );
            }
            let pts = s.dir.restore_points(&t.title, &t.profile);
            if !pts.is_empty() {
                println!("local restore points:");
                for (p, size) in pts {
                    println!("  {}  ({size} bytes)", p.display());
                }
            }
            Ok(())
        }
        "restore" => {
            let s = open()?;
            let t = target(&s, &o, &title_arg(&o, 0)?)?;
            let what = o.pos.get(1).ok_or(SAVES_USAGE)?;
            let r = match what.trim_start_matches('v').parse::<u64>() {
                Ok(v) if !std::path::Path::new(what).exists() => sync::restore_version(&s, &t, v),
                _ => sync::restore_point(&s, &t, std::path::Path::new(what)),
            };
            println!("restored into {}", t.save_dir.display());
            print_push(&t, r.map_err(e)?);
            Ok(())
        }
        "enable" | "disable" => {
            let s = open()?;
            let t = title_arg(&o, 0)?;
            let on = cmd == "enable";
            s.update_cfg(|c| {
                c.titles.insert(t.clone(), state::TitleCfg { enabled: on });
            })
            .map_err(e)?;
            println!("{t}: cloud sync {}", if on { "on" } else { "off" });
            Ok(())
        }
        _ => Err(SAVES_USAGE.into()),
    }
}

fn print_push(t: &Target, r: PushResult) {
    match r {
        PushResult::Pushed { version } => println!("{}: uploaded as version {version}", t.title),
        PushResult::Unchanged => println!("{}: already in the cloud", t.title),
        PushResult::NothingLocal => println!("{}: no local save in {}", t.title, t.save_dir.display()),
        PushResult::Conflict { head, restore_point } => println!(
            "{}: the cloud has a newer save (version {head}) from another device. Yours is kept as a restore point ({}); \
             the next launch or `napple saves pull {}` uses the cloud one, `napple saves restore {} <file>` brings yours back.",
            t.title,
            restore_point.display(),
            t.title,
            t.title
        ),
        PushResult::Quota { message } => println!("{}: not synced, the account's cloud storage is full ({message}); the save stays on this device", t.title),
        PushResult::Queued { error } => println!("{}: offline, queued for later ({error})", t.title),
        PushResult::Disabled => println!("{}: sync is turned off (napple saves enable {})", t.title, t.title),
    }
}

fn list(o: &Opts) -> Result<(), String> {
    let s = open()?;
    let c = s.cfg();
    let remote = s.call("GET", "/saves", None).map_err(e)?;
    let mut rows: std::collections::BTreeMap<(String, String), (u64, u64)> = Default::default();
    for r in napple_cloud::api::list(&remote, "saves") {
        let (Some(t), Some(p)) = (str_field(r, &["title_id"]), str_field(r, &["profile_id"])) else { continue };
        rows.insert((t.to_string(), p.to_string()), (r["latest_version"].as_u64().unwrap_or(0), r["size"].as_u64().unwrap_or(0)));
    }
    let states = s.dir.all_sync();
    for (t, p, _) in &states {
        rows.entry((t.clone(), p.clone())).or_insert((0, 0));
    }
    // local titles that have a save but no sync state yet
    let root = save_root(o);
    let uid = user(o)?;
    if let (Ok(rd), Some(p)) = (std::fs::read_dir(&root), c.links.get(&uid)) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if util::normalize_title(&n).as_deref() == Some(n.as_str()) && e.path().join("save/space1").is_dir() {
                rows.entry((n, p.clone())).or_insert((0, 0));
            }
        }
    }
    println!("{:<17} {:<20} {:>6} {:>10}  status", "TITLE", "PROFILE", "CLOUD", "SIZE");
    for ((t, p), (v, size)) in rows {
        let st = states.iter().find(|(a, b, _)| *a == t && *b == p).map(|x| x.2.clone()).unwrap_or_default();
        let status = if !c.title_enabled(&t) {
            "sync off".to_string()
        } else {
            match st.status.as_str() {
                "synced" if st.version == v => format!("synced (v{})", st.version),
                "synced" => format!("cloud is newer (this device v{})", st.version),
                "quota" => "not synced: cloud storage full (the save stays local)".into(),
                "conflict" => format!("conflict: cloud v{} kept as current, local kept as restore point", st.remote_head),
                "pending" => "waiting to upload (offline)".into(),
                "error" => format!("error: {}", st.detail),
                _ if v > 0 => "in the cloud, not on this device yet".into(),
                _ => "local only".into(),
            }
        };
        let cloud = if v > 0 { format!("v{v}") } else { "-".into() };
        println!("{t:<17} {p:<20} {cloud:>6} {size:>10}  {status}");
    }
    Ok(())
}
