//! napple-build: packaging. `napple build` turns a base game (+ updates + DLC) into a locally signed macOS `.app`.
//! Pipeline in process (no Python): ingest -> rewrite -> NTB1 tables -> RomFS streaming (BKTR for updates, DLC layers)
//! -> system fonts from the firmware -> bundle -> ad-hoc codesign -> atomic publish. Progress lines follow
//! docs/architecture/studio-protocol.md: `PROGRESS <stage> <percent> <message>` and a final `RESULT <path>`.
pub mod bktr;
pub mod bundle;
pub mod check;
pub mod cli;
pub mod container;
pub mod control;
pub mod error;
pub mod firmware;
pub mod fwlist;
pub mod fwprog;
pub mod ios;
pub mod json;
pub mod legacy;
pub mod macho;
pub mod nro;
pub mod pipeline;
pub mod romfs_map;
pub mod stream;
pub mod sysdata;
pub mod tables;
pub mod tables32;
pub mod util;
pub mod zipread;

/// Entry point used by the `napple` binary (`napple build ...`); errors are returned as `[code] message`.
pub fn run(args: &[String]) -> Result<(), String> {
    // Studio reads progress through a pipe; when it cancels or quits, the next println! would panic on EPIPE (Rust ignores SIGPIPE).
    // Die quietly instead: a later build removes the staging directory of a process that no longer exists.
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_DFL) };
    let has = |f: &str| args.iter().any(|a| a == f);
    if has("--dump-fonts") {
        return sysdata::run_dump_fonts(args);
    }
    if has("--firmware-program") {
        return fwprog::run(args);
    }
    if has("--list-firmware") {
        return fwlist::run(args);
    }
    if has("--dump-exefs") || has("--dump-romfs") {
        return legacy::run(args);
    }
    if has("--list-signing") {
        return ios::list_signing(has("--json")).map_err(|e| e.to_string());
    }
    if has("--check") || has("--list-keys-check") {
        return check::run(args);
    }
    let opts = cli::parse(args).map_err(|e| if e.is_usage() { e.msg } else { e.to_string() })?;
    pipeline::build(&opts).map(|_| ()).map_err(|e| {
        println!("ERROR {} {}", e.code, e.msg.replace('\n', " "));
        e.to_string()
    })
}
