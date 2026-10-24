//! Process runner used by every adapter: no shell, explicit argv, hard timeout, bounded output.
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug)]
pub struct Output {
    pub code: Option<i32>,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}

impl Output {
    pub fn ok(&self) -> bool {
        self.code == Some(0)
    }
    pub fn stdout_str(&self) -> String {
        String::from_utf8_lossy(&self.stdout).into_owned()
    }
    pub fn stderr_str(&self) -> String {
        String::from_utf8_lossy(&self.stderr).into_owned()
    }
}

#[derive(Clone, Debug)]
pub struct Spec {
    pub program: PathBuf,
    pub args: Vec<String>,
    pub cwd: Option<PathBuf>,
    /// The child environment is cleared and only these variables are set.
    pub env: Vec<(String, String)>,
    pub timeout: Duration,
    pub max_output: usize,
}

impl Spec {
    pub fn new(program: impl Into<PathBuf>, args: &[&str]) -> Self {
        Self {
            program: program.into(),
            args: args.iter().map(|s| s.to_string()).collect(),
            cwd: None,
            env: Vec::new(),
            timeout: Duration::from_secs(60),
            max_output: 16 << 20,
        }
    }
}

fn drain<R: Read + Send + 'static>(mut r: R, cap: usize) -> std::thread::JoinHandle<Vec<u8>> {
    std::thread::spawn(move || {
        let mut out = Vec::new();
        let mut buf = [0u8; 8192];
        loop {
            match r.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if out.len() < cap {
                        let take = n.min(cap - out.len());
                        out.extend_from_slice(&buf[..take]);
                    }
                    // keep draining past the cap so the child never blocks on a full pipe
                }
            }
        }
        out
    })
}

pub fn run(spec: &Spec) -> Result<Output, String> {
    use std::os::unix::process::CommandExt;
    let mut cmd = Command::new(&spec.program);
    cmd.process_group(0); // own process group so a timeout can kill grandchildren too
    cmd.args(&spec.args)
        .env_clear()
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    if let Some(c) = &spec.cwd {
        cmd.current_dir(c);
    }
    for (k, v) in &spec.env {
        cmd.env(k, v);
    }
    let mut child = cmd.spawn().map_err(|e| format!("cannot start {}: {e}", spec.program.display()))?;
    let out_h = drain(child.stdout.take().unwrap(), spec.max_output);
    let err_h = drain(child.stderr.take().unwrap(), spec.max_output);
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) => {
                if start.elapsed() >= spec.timeout {
                    // negative pid = whole group (children of node/java launchers included)
                    let _ = Command::new("/bin/kill").args(["-KILL", "--", &format!("-{}", child.id())]).status();
                    let _ = child.kill();
                    let _ = child.wait();
                    // reader threads end when the pipes close; do not block on stragglers
                    drop((out_h, err_h));
                    return Err(format!("{} timed out after {}s", spec.program.display(), spec.timeout.as_secs()));
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(e) => return Err(format!("wait failed: {e}")),
        }
    };
    Ok(Output { code: status.code(), stdout: out_h.join().unwrap_or_default(), stderr: err_h.join().unwrap_or_default() })
}

/// Finds an executable by name in `PATH` (no shell).
pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let p = dir.join(name);
        if is_exec(&p) {
            return Some(p);
        }
    }
    None
}

fn is_exec(p: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p).map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn runs_without_shell_and_clears_env() {
        let mut s = Spec::new("/usr/bin/env", &[]);
        s.env.push(("ONLY".into(), "1".into()));
        let o = run(&s).unwrap();
        assert!(o.ok());
        assert_eq!(o.stdout_str().trim(), "ONLY=1");
        // metacharacters are plain arguments
        let o = run(&Spec::new("/bin/echo", &["a;b && $(c)"])).unwrap();
        assert_eq!(o.stdout_str().trim(), "a;b && $(c)");
    }

    #[test]
    fn hard_timeout_kills_child() {
        let mut s = Spec::new("/bin/sleep", &["30"]);
        s.timeout = Duration::from_millis(200);
        let t = Instant::now();
        assert!(run(&s).unwrap_err().contains("timed out"));
        assert!(t.elapsed() < Duration::from_secs(5));
    }

    #[test]
    fn output_is_capped() {
        let mut s = Spec::new("/usr/bin/yes", &[]);
        s.timeout = Duration::from_millis(300);
        s.max_output = 1000;
        // yes never exits: timeout. Use head-like bounded producer instead:
        let mut s2 = Spec::new("/usr/bin/head", &["-c", "100000", "/dev/zero"]);
        s2.max_output = 1000;
        let o = run(&s2).unwrap();
        assert_eq!(o.stdout.len(), 1000);
        let _ = s;
    }
}
