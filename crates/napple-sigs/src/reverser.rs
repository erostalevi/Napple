//! `Reverser` port (function-boundary / naming oracle) and the REA adapter.
//!
//! `ReaCliReverser` runs the `rea` CLI with `--format json` against a raw ARM64 image that lives
//! inside Napple's work directory. No shell, hard timeout, bounded output, defensive parsing:
//! anything unexpected is an error, never a guess.
use crate::domain::SigKind;
use crate::exec::{self, Spec};
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct ReverseRequest {
    pub module: String,
    /// Raw ARM64 code, must be inside the work directory.
    pub image: PathBuf,
    /// Address the image is loaded at; also the analysis entry point (`image` starts at the item).
    pub load_address: u64,
    pub kind: SigKind,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReverseFinding {
    /// Function start (equal to the requested entry) and exclusive end.
    pub start: u64,
    pub end: u64,
    /// Only set when the provider produced a real symbol (not an auto name such as FUN_/entry).
    pub name: Option<String>,
    pub evidence_id: Option<String>,
    pub tool: String,
}

pub trait Reverser {
    fn label(&self) -> String;
    fn analyze(&self, req: &ReverseRequest) -> Result<ReverseFinding, String>;
}

#[derive(Clone, Debug)]
pub struct ReaConfig {
    pub rea_bin: PathBuf,
    pub work_dir: PathBuf,
    pub timeout: Duration,
    pub ghidra_install_dir: Option<PathBuf>,
    pub java_home: Option<PathBuf>,
}

impl ReaConfig {
    /// Defaults: `<work>/tools/node_modules/.bin/rea`, Homebrew Ghidra/JDK when present, 10 min timeout.
    pub fn detect(work_dir: &Path) -> Self {
        let work_dir = &std::path::absolute(work_dir).unwrap_or_else(|_| work_dir.to_path_buf());
        let pick = |env: &str, cands: &[&str]| -> Option<PathBuf> {
            std::env::var_os(env).map(PathBuf::from).or_else(|| cands.iter().map(PathBuf::from).find(|p| p.exists()))
        };
        Self {
            rea_bin: std::env::var_os("NAPPLE_REA_BIN").map(PathBuf::from).unwrap_or_else(|| work_dir.join("tools/node_modules/.bin/rea")),
            work_dir: work_dir.to_path_buf(),
            timeout: Duration::from_secs(600),
            ghidra_install_dir: pick("GHIDRA_INSTALL_DIR", &["/opt/homebrew/opt/ghidra/libexec"]),
            java_home: pick("JAVA_HOME", &["/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home"]),
        }
    }
}

pub struct ReaCliReverser {
    cfg: ReaConfig,
}

impl ReaCliReverser {
    pub fn new(cfg: ReaConfig) -> Self {
        Self { cfg }
    }

    pub fn available(&self) -> bool {
        self.cfg.rea_bin.is_file()
    }

    fn env(&self) -> Vec<(String, String)> {
        let mut e = vec![
            ("PATH".to_string(), std::env::var("PATH").unwrap_or_default()),
            ("HOME".to_string(), std::env::var("HOME").unwrap_or_default()),
        ];
        if let Some(g) = &self.cfg.ghidra_install_dir {
            e.push(("GHIDRA_INSTALL_DIR".into(), g.to_string_lossy().into()));
        }
        if let Some(j) = &self.cfg.java_home {
            e.push(("JAVA_HOME".into(), j.to_string_lossy().into()));
        }
        e
    }

    /// The image must resolve (symlinks included) to a path inside the work directory.
    fn confine(&self, image: &Path) -> Result<PathBuf, String> {
        let work = self.cfg.work_dir.canonicalize().map_err(|e| format!("work dir: {e}"))?;
        let img = image.canonicalize().map_err(|e| format!("image: {e}"))?;
        if !img.starts_with(&work) || !img.is_file() {
            return Err("REA inputs must be regular files inside the work directory".into());
        }
        Ok(img)
    }
}

fn parse_addr(v: &Value) -> Option<u64> {
    let s = v.as_str()?;
    u64::from_str_radix(s.strip_prefix("0x").or_else(|| s.strip_prefix("0X"))?, 16).ok()
}

fn is_auto_name(n: &str) -> bool {
    n == "entry" || n.starts_with("FUN_") || n.starts_with("sub_") || n.starts_with("thunk_FUN_") || n.starts_with("LAB_") || n.starts_with("DAT_")
}

/// Parses `rea function ... --format json` output.
pub fn parse_function_output(stdout: &str, entry: u64, max_len: u64) -> Result<ReverseFinding, String> {
    let start = stdout.find('{').ok_or("no JSON object in REA output")?;
    let v: Value = serde_json::from_str(&stdout[start..]).map_err(|e| format!("REA output is not valid JSON: {e}"))?;
    let proc_ = v.pointer("/normalized_result/procedure").ok_or("REA output has no procedure")?;
    let ranges = proc_.pointer("/body/ranges").and_then(|r| r.as_array()).ok_or("REA output has no function body ranges")?;
    let mut hit = None;
    for r in ranges {
        let (Some(s), Some(e)) = (r.get("start").and_then(parse_addr), r.get("end").and_then(parse_addr)) else {
            return Err("unparseable function range".into());
        };
        if s <= entry && entry <= e {
            hit = Some((s, e));
        }
    }
    let (s, e_incl) = hit.ok_or("provider found no function at the requested entry")?;
    if s != entry {
        return Err("provider placed the function start elsewhere; refusing to guess".into());
    }
    let end = e_incl.checked_add(1).ok_or("range overflow")?;
    if end <= s || end - s > max_len {
        return Err("function range out of bounds".into());
    }
    let name = proc_.get("name").and_then(|n| n.as_str()).filter(|n| !is_auto_name(n)).map(|s| s.to_string());
    let evidence_id = v.get("evidence_id").and_then(|e| e.as_str()).map(|s| s.chars().take(80).collect());
    let tool = match (v.pointer("/provider/id").and_then(|x| x.as_str()), v.pointer("/provider/version").and_then(|x| x.as_str())) {
        (Some(id), Some(ver)) => format!("rea/{id} {ver}"),
        _ => "rea".into(),
    };
    Ok(ReverseFinding { start: s, end, name, evidence_id, tool })
}

impl Reverser for ReaCliReverser {
    fn label(&self) -> String {
        "rea".into()
    }

    fn analyze(&self, req: &ReverseRequest) -> Result<ReverseFinding, String> {
        if !self.available() {
            return Err(format!("rea CLI not found at {} (see docs/tooling.md)", self.cfg.rea_bin.display()));
        }
        let img = self.confine(&req.image)?;
        let addr = format!("0x{:x}", req.load_address);
        let mut spec = Spec::new(
            &self.cfg.rea_bin,
            &[
                "function",
                &img.to_string_lossy(),
                &addr,
                "--target-format",
                "raw-binary",
                "--architecture",
                "arm64",
                "--entry-point",
                &addr,
                "--provider",
                "ghidra",
                "--format",
                "json",
            ],
        );
        spec.cwd = Some(self.cfg.work_dir.clone());
        spec.env = self.env();
        spec.timeout = self.cfg.timeout;
        let out = exec::run(&spec)?;
        if !out.ok() {
            let e: String = out.stderr_str().chars().filter(|c| !c.is_control() || *c == ' ').take(300).collect();
            return Err(format!("rea exited with {:?}: {}", out.code, e.trim()));
        }
        parse_function_output(&out.stdout_str(), req.load_address, 1 << 20)
    }
}

#[cfg(test)]
pub(crate) mod fake {
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};

    /// Writes an executable fake `rea` that prints `body` and exits with `code`.
    pub fn fake_rea(dir: &Path, body: &str, code: i32, sleep: u32) -> PathBuf {
        let p = dir.join("rea");
        let script = format!("#!/bin/sh\nsleep {sleep}\ncat <<'EOF'\n{body}\nEOF\nexit {code}\n");
        std::fs::write(&p, script).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
        p
    }

    pub const GOOD: &str = r#"{"evidence_id":"ev_abc","provider":{"id":"ghidra","version":"12.1.4"},
      "normalized_result":{"procedure":{"address":"0x1000","name":"FUN_00001000","body":{"ranges":[{"start":"0x1000","end":"0x103f"}]}}}}"#;
}

#[cfg(test)]
mod tests {
    use super::fake::*;
    use super::*;

    fn setup(body: &str, code: i32, sleep: u32, timeout_ms: u64) -> (tempfile::TempDir, ReaCliReverser, ReverseRequest) {
        let d = tempfile::tempdir().unwrap();
        let work = d.path().join("work");
        std::fs::create_dir_all(&work).unwrap();
        let rea = fake_rea(&work, body, code, sleep);
        let img = work.join("img.bin");
        std::fs::write(&img, [0u8; 64]).unwrap();
        let mut cfg = ReaConfig::detect(&work);
        cfg.rea_bin = rea;
        cfg.timeout = Duration::from_millis(timeout_ms);
        let req = ReverseRequest { module: "main".into(), image: img, load_address: 0x1000, kind: SigKind::Function };
        (d, ReaCliReverser::new(cfg), req)
    }

    #[test]
    fn parses_function_boundaries_and_ignores_auto_names() {
        let (_d, r, req) = setup(GOOD, 0, 0, 5000);
        let f = r.analyze(&req).unwrap();
        assert_eq!((f.start, f.end), (0x1000, 0x1040));
        assert_eq!(f.name, None);
        assert_eq!(f.tool, "rea/ghidra 12.1.4");
        let named = GOOD.replace("FUN_00001000", "nn::os::Thing");
        let (_d, r, req) = setup(&named, 0, 0, 5000);
        assert_eq!(r.analyze(&req).unwrap().name.as_deref(), Some("nn::os::Thing"));
    }

    #[test]
    fn defensive_parsing_rejects_garbage() {
        for body in ["not json", "{}", r#"{"normalized_result":{"procedure":{"body":{"ranges":[]}}}}"#,
            r#"{"normalized_result":{"procedure":{"body":{"ranges":[{"start":"0x2000","end":"0x20ff"}]}}}}"#,
            r#"{"normalized_result":{"procedure":{"body":{"ranges":[{"start":"0xfff","end":"0x1fff"}]}}}}"#,
            r#"{"normalized_result":{"procedure":{"body":{"ranges":[{"start":"0x1000","end":"0xffffffffff"}]}}}}"#] {
            let (_d, r, req) = setup(body, 0, 0, 5000);
            assert!(r.analyze(&req).is_err(), "{body}");
        }
    }

    #[test]
    fn nonzero_exit_and_timeout_are_errors() {
        let (_d, r, req) = setup(GOOD, 3, 0, 5000);
        assert!(r.analyze(&req).unwrap_err().contains("exited"));
        let (_d, r, req) = setup(GOOD, 0, 30, 300);
        assert!(r.analyze(&req).unwrap_err().contains("timed out"));
    }

    #[test]
    fn inputs_outside_work_are_refused() {
        let (_d, r, mut req) = setup(GOOD, 0, 0, 5000);
        let outside = tempfile::tempdir().unwrap();
        let f = outside.path().join("x.bin");
        std::fs::write(&f, [0u8; 8]).unwrap();
        req.image = f.clone();
        assert!(r.analyze(&req).unwrap_err().contains("inside the work directory"));
        // a symlink inside work pointing outside is refused too
        let link = r.cfg.work_dir.join("link.bin");
        std::os::unix::fs::symlink(&f, &link).unwrap();
        req.image = link;
        assert!(r.analyze(&req).is_err());
    }

    #[test]
    fn missing_binary_is_reported() {
        let (_d, mut r, req) = setup(GOOD, 0, 0, 5000);
        r.cfg.rea_bin = PathBuf::from("/nonexistent/rea");
        assert!(r.analyze(&req).unwrap_err().contains("not found"));
    }
}
