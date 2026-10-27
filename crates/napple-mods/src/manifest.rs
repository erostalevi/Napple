//! `mod.json`.
use crate::util::{Result, err, norm_build_id};
use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const FORMAT: u32 = 1;
pub const MANIFEST_PATH: &str = "mod.json";
pub const SHA_MANIFEST_PATH: &str = "manifest.sha256";

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Requires {
    /// `>=X.Y.Z` (the only comparison supported) against the Napple version.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub napple: Option<String>,
}

/// What the mod says it contains. `validate` checks this against the files actually present.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Components {
    /// Paths of `.pchtxt` / `.ips` files under `patches/`.
    #[serde(default)]
    pub patches: Vec<String>,
    /// Paths of cheat files under `cheats/`.
    #[serde(default)]
    pub cheats: Vec<String>,
    /// A `files/` romfs overlay tree.
    #[serde(default)]
    pub romfs: bool,
    /// A `config.json`.
    #[serde(default)]
    pub config: bool,
    /// `shaders/<hash>.msl` files.
    #[serde(default)]
    pub shaders: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
    #[serde(default = "one")]
    pub format: u32,
    pub id: String,
    pub name: String,
    pub version: String,
    pub title_id: String,
    #[serde(default)]
    pub build_ids: Vec<String>,
    #[serde(default)]
    pub author: String,
    #[serde(default)]
    pub license: String,
    #[serde(default)]
    pub requires: Requires,
    /// `None` when mod.json has no `components` key (`pack` then derives it from the tree).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub components: Option<Components>,
    #[serde(default)]
    pub description: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumbnail: Option<String>,
    /// Reserved for a detached signature (not verified in format 1; must be null or absent today).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub signature: Option<Value>,
}

fn one() -> u32 {
    1
}

pub fn parse(bytes: &[u8]) -> Result<(Manifest, Vec<String>)> {
    let v: Value = serde_json::from_slice(bytes)?;
    let known = [
        "format",
        "id",
        "name",
        "version",
        "titleId",
        "buildIds",
        "author",
        "license",
        "requires",
        "components",
        "description",
        "thumbnail",
        "signature",
    ];
    let warn = v
        .as_object()
        .map(|o| {
            o.keys()
                .filter(|k| !known.contains(&k.as_str()))
                .map(|k| format!("mod.json: unknown key {k:?}"))
                .collect()
        })
        .unwrap_or_default();
    let m: Manifest = serde_json::from_value(v)?;
    Ok((m, warn))
}

pub fn to_json(m: &Manifest) -> Vec<u8> {
    let mut s = serde_json::to_string_pretty(m).unwrap_or_default();
    s.push('\n');
    s.into_bytes()
}

fn is_semver(v: &str) -> bool {
    let core = v.split(['-', '+']).next().unwrap_or("");
    let p: Vec<&str> = core.split('.').collect();
    p.len() == 3
        && p.iter()
            .all(|x| !x.is_empty() && x.len() <= 9 && x.bytes().all(|b| b.is_ascii_digit()))
}

fn triple(v: &str) -> Option<(u64, u64, u64)> {
    let core = v.split(['-', '+']).next()?;
    let mut it = core.split('.').map(|x| x.parse::<u64>().ok());
    Some((it.next()??, it.next()??, it.next()??))
}

/// Field-level checks. Returns a list of errors (empty = ok).
pub fn check(m: &Manifest) -> Vec<String> {
    let mut e = Vec::new();
    if m.format != FORMAT {
        e.push(format!(
            "mod.json: format {} is not supported (this Napple reads format {FORMAT})",
            m.format
        ));
    }
    let idok = m.id.len() >= 3
        && m.id.len() <= 64
        && m.id.bytes().all(|b| {
            b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'.' || b == b'_' || b == b'-'
        })
        && !m.id.starts_with(['.', '-', '_']);
    if !idok {
        e.push(format!(
            "mod.json: id {:?} must be 3..=64 of [a-z0-9._-], not starting with a symbol",
            m.id
        ));
    }
    if m.name.trim().is_empty() || m.name.len() > 80 {
        e.push("mod.json: name must be 1..=80 characters".into());
    }
    if !is_semver(&m.version) {
        e.push(format!(
            "mod.json: version {:?} is not MAJOR.MINOR.PATCH",
            m.version
        ));
    }
    if m.title_id.len() != 16 || !m.title_id.bytes().all(|b| b.is_ascii_hexdigit()) {
        e.push(format!(
            "mod.json: titleId {:?} must be 16 hex digits",
            m.title_id
        ));
    }
    for b in &m.build_ids {
        if norm_build_id(b).is_err() {
            e.push(format!(
                "mod.json: buildIds entry {b:?} is not 1..=64 hex digits"
            ));
        }
    }
    if m.description.len() > 4000 {
        e.push("mod.json: description longer than 4000 bytes".into());
    }
    if let Some(r) = &m.requires.napple {
        match r.strip_prefix(">=").and_then(|v| triple(v.trim())) {
            None => e.push(format!(
                "mod.json: requires.napple {r:?} must look like >=0.1.0"
            )),
            Some(need) if Some(need) > triple(env!("CARGO_PKG_VERSION")) => e.push(format!(
                "mod.json: needs Napple {r}, this is {}",
                env!("CARGO_PKG_VERSION")
            )),
            _ => {}
        }
    }
    if matches!(&m.signature, Some(v) if !v.is_null()) {
        e.push("mod.json: signature is reserved and cannot be set in format 1".into());
    }
    e
}

pub fn require_valid(m: &Manifest) -> Result<()> {
    let e = check(m);
    if e.is_empty() {
        Ok(())
    } else {
        err(e.join("; "))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const OK: &str = r#"{"id":"example.fix","name":"Fix","version":"1.0.0","titleId":"0100000000000001","buildIds":["AB"]}"#;

    #[test]
    fn ok_and_defaults() {
        let (m, w) = parse(OK.as_bytes()).unwrap();
        assert!(w.is_empty());
        assert!(check(&m).is_empty());
        assert_eq!(m.format, 1);
        assert!(m.components.is_none());
    }

    #[test]
    fn rejects() {
        for (from, to) in [
            ("example.fix", "X"),
            ("1.0.0", "1.0"),
            ("0100000000000001", "01"),
            (r#"["AB"]"#, r#"["xyz"]"#),
        ] {
            let (m, _) = parse(OK.replace(from, to).as_bytes()).unwrap();
            assert!(!check(&m).is_empty(), "{to}");
        }
        let (m, _) = parse(br#"{"id":"abc","name":"n","version":"1.0.0","titleId":"0100000000000001","requires":{"napple":">=99.0.0"},"signature":{"x":1}}"#).unwrap();
        assert_eq!(check(&m).len(), 2);
        assert!(parse(b"{").is_err());
        assert!(parse(b"[]").is_err());
    }
}
