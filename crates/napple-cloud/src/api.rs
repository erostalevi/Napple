//! HTTP access to the account API (JSON over HTTPS, `Authorization: Bearer`), and to the presigned save URLs.
use crate::{Error, Result};
use serde_json::Value;
use std::io::Read;
use std::time::{Duration, Instant};

pub struct Api {
    pub base: String,
    agent: ureq::Agent,
    timeout: Duration,
    deadline: std::sync::Mutex<Option<Instant>>,
}

fn status_error(code: u16, body: String) -> Error {
    let v: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
    match code {
        401 => Error::Unauthorized,
        402 => Error::PlanLimit { upgrade_url: v["upgrade_url"].as_str().unwrap_or("").to_string() },
        409 => {
            let h = &v["head"];
            let head = h.as_u64().or_else(|| h["version"].as_u64()).or_else(|| v["head_version"].as_u64()).unwrap_or(0);
            Error::Conflict { head }
        }
        _ => {
            let mut b = body;
            b.truncate(300);
            Error::Http { status: code, body: b }
        }
    }
}

fn map_err(e: ureq::Error) -> Error {
    match e {
        ureq::Error::Status(code, resp) => status_error(code, resp.into_string().unwrap_or_default()),
        ureq::Error::Transport(t) => {
            if t.kind() == ureq::ErrorKind::Io && t.to_string().contains("timed out") {
                Error::Timeout
            } else {
                Error::Net(t.to_string())
            }
        }
    }
}

impl Api {
    pub fn new(base: &str) -> Self {
        let timeout = Duration::from_secs(std::env::var("NAPPLE_ACCOUNT_TIMEOUT").ok().and_then(|s| s.parse().ok()).unwrap_or(20));
        let agent = ureq::AgentBuilder::new()
            .timeout_connect(Duration::from_secs(5))
            .user_agent(concat!("napple-cloud/", env!("CARGO_PKG_VERSION")))
            .build();
        Api { base: base.trim_end_matches('/').to_string(), agent, timeout, deadline: std::sync::Mutex::new(None) }
    }

    /// Every later request must finish before `d` (pull before mount must not hold up a launch for long).
    pub fn set_deadline(&self, d: Option<Instant>) {
        *self.deadline.lock().unwrap() = d;
    }

    fn per_request(&self) -> Result<Duration> {
        match *self.deadline.lock().unwrap() {
            None => Ok(self.timeout),
            Some(d) => {
                let left = d.saturating_duration_since(Instant::now());
                if left.is_zero() { Err(Error::Timeout) } else { Ok(left.min(self.timeout)) }
            }
        }
    }

    pub fn url(&self, path: &str) -> String {
        if path.starts_with("http://") || path.starts_with("https://") { path.to_string() } else { format!("{}{}", self.base, path) }
    }

    /// A JSON request; an empty response body is `Value::Null`.
    pub fn call(&self, method: &str, path: &str, token: Option<&str>, body: Option<&Value>) -> Result<Value> {
        let mut r = self.agent.request(method, &self.url(path)).timeout(self.per_request()?).set("Accept", "application/json");
        if let Some(t) = token {
            r = r.set("Authorization", &format!("Bearer {t}"));
        }
        let resp = match body {
            Some(b) => r.send_json(b.clone()),
            None => r.call(),
        }
        .map_err(map_err)?;
        let s = resp.into_string().map_err(|e| Error::Net(e.to_string()))?;
        if s.trim().is_empty() { Ok(Value::Null) } else { serde_json::from_str(&s).map_err(|e| Error::Format(format!("{path}: {e}"))) }
    }

    /// PUT to a presigned upload URL.
    pub fn put_bytes(&self, url: &str, data: &[u8]) -> Result<()> {
        self.agent
            .put(&self.url(url))
            .timeout(self.per_request()?.max(Duration::from_secs(10)))
            .set("Content-Type", "application/octet-stream")
            .send_bytes(data)
            .map_err(map_err)?;
        Ok(())
    }

    /// GET from a presigned download URL (at most `max` bytes).
    pub fn get_bytes(&self, url: &str, max: usize) -> Result<Vec<u8>> {
        let resp = self.agent.get(&self.url(url)).timeout(self.per_request()?).call().map_err(map_err)?;
        let mut out = Vec::new();
        resp.into_reader().take(max as u64 + 1).read_to_end(&mut out).map_err(|e| Error::Net(e.to_string()))?;
        if out.len() > max {
            return Err(Error::Format("download larger than a save version may be".into()));
        }
        Ok(out)
    }
}

/// A list response: a bare array, or an object holding one under `key`.
pub fn list<'a>(v: &'a Value, key: &str) -> &'a [Value] {
    v.as_array().or_else(|| v[key].as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

/// A string field under any of the given names.
pub fn str_field<'a>(v: &'a Value, names: &[&str]) -> Option<&'a str> {
    names.iter().find_map(|n| v[*n].as_str())
}
