//! A32 mode: ahead-of-time AArch32 -> AArch64 recompiler for 32-bit titles (Mario Kart 8 Deluxe).
//!
//! `napple rewrite a32 <modules dir> [-o <work dir>]` reads the `<name>.patched.bin` + `<name>.tables` pairs `napple build` wrote for the ELF32
//! modules (crates/napple-build/src/tables32.rs), lifts every reachable ARM/Thumb-2 instruction to AArch64 assembly (the C lifter in
//! runtime/src/a32jit, built into this crate by build.rs, sharing the runtime interpreter's decoder), assembles it with clang and links the
//! result into `<modules dir>/a32jit.dylib` with its `a32jit.manifest`. The image is an ordinary Mach-O dylib: nothing is generated at run
//! time, so it can be code signed and shipped on iOS (which forbids JIT). Whatever the lifter did not reach, and every instruction it does not
//! lift natively, runs in the runtime's interpreter (mixed mode), so the image never has to be complete to be correct.
use crate::error::RewriteError;
use rayon::prelude::*;
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::path::{Path, PathBuf};
use std::process::Command;

unsafe extern "C" {
    fn a32lift_tool(dir: *const c_char, prefix: *const c_char) -> c_int;
}

const USAGE: &str = "usage: napple rewrite a32 <modules dir> [-o <work dir>]   (the dir holding main.patched.bin / main.tables ...)";
/// Deployment target of the produced dylib. `rmif` (FEAT_FlagM, Armv8.4-A) is used for the flag-preserving logical operations: Apple A13 and later.
const TARGET: &str = "arm64-apple-macos13";

fn io(e: std::io::Error) -> RewriteError {
    RewriteError::Io(e.to_string())
}

fn clang(args: &[&str]) -> Result<(), RewriteError> {
    let out = Command::new("clang").args(args).output().map_err(io)?;
    if out.status.success() {
        Ok(())
    } else {
        Err(RewriteError::Usage(format!(
            "clang {}: {}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr)
        )))
    }
}

/// Lifts the modules in `modules` and returns the assembly files written under `work`.
pub fn lift(modules: &Path, work: &Path) -> Result<Vec<PathBuf>, RewriteError> {
    std::fs::create_dir_all(work).map_err(io)?;
    let prefix = work.join("j");
    let dir = CString::new(modules.to_string_lossy().as_bytes())
        .map_err(|e| RewriteError::Usage(e.to_string()))?;
    let pre = CString::new(prefix.to_string_lossy().as_bytes())
        .map_err(|e| RewriteError::Usage(e.to_string()))?;
    // SAFETY: both pointers are valid NUL-terminated strings for the duration of the call; the lifter keeps no reference to them.
    let rc = unsafe { a32lift_tool(dir.as_ptr(), pre.as_ptr()) };
    if rc != 0 {
        return Err(RewriteError::Usage(format!(
            "the A32 lifter failed (code {rc}) on {}",
            modules.display()
        )));
    }
    let list = std::fs::read_to_string(work.join("j.files")).map_err(io)?;
    Ok(list
        .lines()
        .filter(|l| !l.is_empty())
        .map(PathBuf::from)
        .collect())
}

/// Assembles `files` in parallel and links them into `dylib`.
pub fn build_image(files: &[PathBuf], dylib: &Path) -> Result<(), RewriteError> {
    let objs: Vec<PathBuf> = files
        .iter()
        .map(|f| PathBuf::from(format!("{}.o", f.display())))
        .collect();
    files
        .par_iter()
        .zip(objs.par_iter())
        .try_for_each(|(s, o)| {
            clang(&[
                "-c",
                "-target",
                TARGET,
                "-march=armv8.4-a",
                &s.to_string_lossy(),
                "-o",
                &o.to_string_lossy(),
            ])
        })?;
    let mut args: Vec<String> = ["-dynamiclib", "-target", TARGET, "-Wl,-x", "-o"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    args.push(dylib.to_string_lossy().into());
    // the object list goes through a response file: there are hundreds of them
    let rsp = dylib.with_extension("objs");
    std::fs::write(
        &rsp,
        objs.iter()
            .map(|o| o.to_string_lossy().to_string())
            .collect::<Vec<_>>()
            .join("\n"),
    )
    .map_err(io)?;
    args.push(format!("@{}", rsp.display()));
    let a: Vec<&str> = args.iter().map(String::as_str).collect();
    clang(&a)?;
    let _ = std::fs::remove_file(rsp);
    Ok(())
}

/// Full pipeline: lift, assemble, link; installs `a32jit.dylib` and `a32jit.manifest` next to the modules.
pub fn recompile(modules: &Path, work: &Path) -> Result<PathBuf, RewriteError> {
    let files = lift(modules, work)?;
    let dylib = work.join("a32jit.dylib");
    build_image(&files, &dylib)?;
    let out = modules.join("a32jit.dylib");
    std::fs::copy(&dylib, &out).map_err(io)?;
    std::fs::copy(work.join("j.manifest"), modules.join("a32jit.manifest")).map_err(io)?;
    for f in files {
        let _ = std::fs::remove_file(format!("{}.o", f.display()));
        let _ = std::fs::remove_file(f);
    }
    Ok(out)
}

pub fn run(args: &[String]) -> Result<(), RewriteError> {
    let (mut modules, mut work) = (None, None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-o" => {
                work = Some(PathBuf::from(
                    it.next().ok_or_else(|| RewriteError::Usage(USAGE.into()))?,
                ))
            }
            "-h" | "--help" => return Err(RewriteError::Usage(USAGE.into())),
            d => modules = Some(PathBuf::from(d)),
        }
    }
    let modules = modules.ok_or_else(|| RewriteError::Usage(USAGE.into()))?;
    let work = work.unwrap_or_else(|| std::env::temp_dir().join("napple-a32jit"));
    let out = recompile(&modules, &work)?;
    println!(
        "a32: wrote {} ({} bytes)",
        out.display(),
        std::fs::metadata(&out).map(|m| m.len()).unwrap_or(0)
    );
    Ok(())
}
