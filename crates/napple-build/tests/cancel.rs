//! Own process: a SIGTERM during a build makes it stop with the `cancelled` code and leave no staging directory.
mod support;
use support::*;

#[test]
fn sigterm_cancels_and_cleans_up() {
    let _t = tmp("cancel");
    let g = make_game(&_t, true);
    let out = tmp("cancel-out");
    napple_build::util::install_cancel_handlers();
    unsafe { libc::raise(libc::SIGTERM) };
    let args: Vec<String> = [g.base_nsp.to_str().unwrap(), "-k", g.keys.to_str().unwrap(), "--data-only", "-o", out.to_str().unwrap()].iter().map(|s| s.to_string()).collect();
    let e = napple_build::run(&args).unwrap_err();
    assert!(e.starts_with("[cancelled]"), "{e}");
    assert!(std::fs::read_dir(&out).unwrap().flatten().all(|e| !e.file_name().to_string_lossy().starts_with(".napple-build")));
    assert!(!out.join(format!("{APP_ID:016X}")).exists());
}
