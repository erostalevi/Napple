//! `config.json`: per-title settings a mod asks for, and the merge of several mods' settings.
use crate::util::{Result, err};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModConfig {
    /// Internal render resolution multiplier, 0.25..=8.0.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub render_scale: Option<f64>,
    /// `native`, `16:9`, `21:9`, `32:9`, `4:3` or `W:H`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub aspect: Option<String>,
    /// `default`, `30`, `60` or `unlocked`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub fps_mode: Option<String>,
    /// Named fix toggles, e.g. `{"skipIntroLogos": true}`.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub fixes: BTreeMap<String, bool>,
}

const KNOWN: &[&str] = &["renderScale", "aspect", "fpsMode", "fixes"];

fn valid_aspect(a: &str) -> bool {
    if a == "native" {
        return true;
    }
    let Some((w, h)) = a.split_once(':') else {
        return false;
    };
    let ok = |s: &str| s.len() <= 3 && s.parse::<u32>().is_ok_and(|n| (1..=999).contains(&n));
    ok(w) && ok(h)
}

/// Parses and validates; the second value lists unknown keys (warnings).
pub fn parse(text: &str) -> Result<(ModConfig, Vec<String>)> {
    let v: Value = serde_json::from_str(text)?;
    let Some(obj) = v.as_object() else {
        return err("config.json must be an object");
    };
    let warn = obj
        .keys()
        .filter(|k| !KNOWN.contains(&k.as_str()))
        .map(|k| format!("config.json: unknown key {k:?} ignored"))
        .collect();
    let c: ModConfig = serde_json::from_value(v)?;
    if let Some(s) = c.render_scale
        && !(0.25..=8.0).contains(&s)
    {
        return err(format!("renderScale {s} outside 0.25..=8.0"));
    }
    if let Some(a) = &c.aspect
        && !valid_aspect(a)
    {
        return err(format!("aspect {a:?} is not native or W:H"));
    }
    if let Some(f) = &c.fps_mode
        && !["default", "30", "60", "unlocked"].contains(&f.as_str())
    {
        return err(format!("fpsMode {f:?} is not default/30/60/unlocked"));
    }
    for k in c.fixes.keys() {
        if k.is_empty()
            || k.len() > 64
            || !k
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
        {
            return err(format!("fix name {k:?} must be 1..=64 of [A-Za-z0-9_-]"));
        }
    }
    Ok((c, warn))
}

/// Later configs win key by key; `fixes` maps merge.
pub fn merge(list: &[&ModConfig]) -> ModConfig {
    let mut o = ModConfig::default();
    for c in list {
        if c.render_scale.is_some() {
            o.render_scale = c.render_scale;
        }
        if c.aspect.is_some() {
            o.aspect = c.aspect.clone();
        }
        if c.fps_mode.is_some() {
            o.fps_mode = c.fps_mode.clone();
        }
        for (k, v) in &c.fixes {
            o.fixes.insert(k.clone(), *v);
        }
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_and_merge() {
        let (a, w) =
            parse(r#"{"renderScale":1.5,"aspect":"21:9","fixes":{"a":true},"extra":1}"#).unwrap();
        assert_eq!(w.len(), 1);
        let (b, _) =
            parse(r#"{"renderScale":2,"fpsMode":"60","fixes":{"a":false,"b":true}}"#).unwrap();
        let m = merge(&[&a, &b]);
        assert_eq!(m.render_scale, Some(2.0));
        assert_eq!(m.aspect.as_deref(), Some("21:9"));
        assert_eq!(m.fps_mode.as_deref(), Some("60"));
        assert!(!m.fixes["a"]);
        assert!(m.fixes["b"]);
    }

    #[test]
    fn rejects() {
        for bad in [
            "[]",
            "{",
            r#"{"renderScale":100}"#,
            r#"{"renderScale":"x"}"#,
            r#"{"aspect":"wide"}"#,
            r#"{"fpsMode":"120"}"#,
            r#"{"fixes":{"a b":true}}"#,
            r#"{"fixes":{"a":1}}"#,
        ] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }
}
