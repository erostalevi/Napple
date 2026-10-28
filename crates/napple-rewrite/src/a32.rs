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

const USAGE: &str = "usage: napple rewrite a32 [--coverage] <modules dir> [-o <work dir>]   (the dir holding main.patched.bin / main.tables ...)";
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
    // A fixed install name: the default is the temp work path (unique per build process), which went into LC_ID_DYLIB and the content-derived UUID, so two
    // builds of the same title never produced the same bytes. The runtime dlopens it by absolute path, so the name is never resolved.
    let mut args: Vec<String> = [
        "-dynamiclib",
        "-target",
        TARGET,
        "-Wl,-x",
        "-install_name",
        "@rpath/a32jit.dylib",
        "-o",
    ]
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

/// Prints the fallback coverage table of a lift: per module and operation the number of instructions the lifter did not turn into native code, with the
/// first address and encoding of each group. The full list (module, address, A|T, encoding, operation) is kept in `<work>/j.cov`.
pub fn coverage_table(cov: &Path) -> Result<String, RewriteError> {
    use std::collections::BTreeMap;
    let text = std::fs::read_to_string(cov).map_err(io)?;
    let mut groups: BTreeMap<(String, String), (u64, String)> = BTreeMap::new();
    let (mut native, mut fb) = (0u64, 0u64);
    for l in text.lines() {
        if let Some(s) = l.strip_prefix("#stats ") {
            let mut it = s.split_whitespace().map(|v| v.parse::<u64>().unwrap_or(0));
            native = it.next().unwrap_or(0);
            fb = it.next().unwrap_or(0);
            continue;
        }
        let f: Vec<&str> = l.split_whitespace().collect();
        if f.len() != 5 {
            continue;
        }
        let e = groups
            .entry((f[0].to_string(), f[4].to_string()))
            .or_insert((0, format!("{} {} {}", f[1], f[2], f[3])));
        e.0 += 1;
    }
    let mut o = String::new();
    o.push_str("module     operation   count   first (address A|T encoding)\n");
    o.push_str("# operations the lifter does not inline: each is a build-time decoded instruction executed by an out-of-line semantics routine (a32_op_exec);\n");
    o.push_str("# nothing is fetched or decoded at run time and there is no interpreter loop in the runtime\n");
    for ((m, op), (n, first)) in &groups {
        o.push_str(&format!("{m:<10} {op:<10} {n:>7}   {first}\n"));
    }
    o.push_str(&format!(
        "total: {native} instructions lifted inline, {fb} out-of-line, 0 interpreter fallbacks\n"
    ));
    Ok(o)
}

pub fn run(args: &[String]) -> Result<(), RewriteError> {
    let (mut modules, mut work) = (None, None);
    let mut coverage = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "-o" => {
                work = Some(PathBuf::from(
                    it.next().ok_or_else(|| RewriteError::Usage(USAGE.into()))?,
                ))
            }
            "--coverage" => coverage = true,
            "-h" | "--help" => return Err(RewriteError::Usage(USAGE.into())),
            d => modules = Some(PathBuf::from(d)),
        }
    }
    let modules = modules.ok_or_else(|| RewriteError::Usage(USAGE.into()))?;
    let work = work.unwrap_or_else(|| std::env::temp_dir().join("napple-a32jit"));
    if coverage {
        lift(&modules, &work)?;
        print!("{}", coverage_table(&work.join("j.cov"))?);
        return Ok(());
    }
    let out = recompile(&modules, &work)?;
    println!(
        "a32: wrote {} ({} bytes)",
        out.display(),
        std::fs::metadata(&out).map(|m| m.len()).unwrap_or(0)
    );
    Ok(())
}
