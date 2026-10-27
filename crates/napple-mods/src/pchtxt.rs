//! pchtxt patch files (`@nsobid-<id>`, `@flag offset_shift`, `@enabled`/`@disabled` blocks, comments) and their JSON form.
//!
//! Offsets: a record address `A` plus the current `offset_shift` `S` is the offset `F` in the NSO *file image including its
//! 0x100-byte header* (the convention of Atmosphere IPS patches and of yuzu/Ryujinx pchtxt). The module offset is `F - 0x100`.
//! Records store `F`; the shift is folded in at parse time.
use crate::util::{Result, err, hex_decode, hex_encode, norm_build_id};
use serde_json::{Value, json};

const MAX_RECORDS: usize = 1 << 20;
const MAX_LINE: usize = 1 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PchRecord {
    /// Offset in the NSO file image including the 0x100 header (address + offset_shift).
    pub offset: u64,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PchPatch {
    pub name: String,
    /// `@enabled` (true) or `@disabled` (false): the default state before the user chooses.
    pub enabled: bool,
    pub records: Vec<PchRecord>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PchBuild {
    /// 32-byte module build id (the file may give fewer digits; the rest is zero).
    pub build_id: [u8; 32],
    pub patches: Vec<PchPatch>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Pchtxt {
    pub builds: Vec<PchBuild>,
    pub warnings: Vec<String>,
}

fn strip_comment(l: &str) -> (&str, Option<&str>) {
    let mut q = false;
    let mut esc = false;
    let b = l.as_bytes();
    for i in 0..b.len() {
        let c = b[i];
        if q {
            if esc {
                esc = false;
            } else if c == b'\\' {
                esc = true;
            } else if c == b'"' {
                q = false;
            }
        } else if c == b'"' {
            q = true;
        } else if c == b'/' && i + 1 < b.len() && b[i + 1] == b'/' {
            return (&l[..i], Some(l[i + 2..].trim()));
        }
    }
    (l, None)
}

fn parse_num(s: &str) -> Result<u64> {
    let t = s.trim();
    let (t, radix) = match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(r) => (r, 16),
        None => (t, 16),
    };
    u64::from_str_radix(t, radix).or_else(|_| err(format!("bad number {s:?}")))
}

fn parse_value(s: &str) -> Result<Vec<u8>> {
    let s = s.trim();
    if let Some(q) = s.strip_prefix('"') {
        let q = q
            .strip_suffix('"')
            .ok_or(crate::util::ModError("unterminated string value".into()))?;
        let mut o = Vec::new();
        let mut it = q.chars();
        while let Some(c) = it.next() {
            if c != '\\' {
                let mut buf = [0u8; 4];
                o.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
                continue;
            }
            match it.next() {
                Some('n') => o.push(b'\n'),
                Some('t') => o.push(b'\t'),
                Some('\\') => o.push(b'\\'),
                Some('"') => o.push(b'"'),
                Some('x') => {
                    let h: String = it.by_ref().take(2).collect();
                    o.push(u8::from_str_radix(&h, 16).or_else(|_| err("bad \\x escape"))?);
                }
                _ => return err("bad string escape"),
            }
        }
        if o.is_empty() {
            return err("empty string value");
        }
        return Ok(o);
    }
    let h = s.strip_prefix("0x").unwrap_or(s);
    if h.is_empty() {
        return err("empty value");
    }
    hex_decode(h)
}

pub fn parse(text: &str) -> Result<Pchtxt> {
    let mut out = Pchtxt::default();
    let mut shift = 0u64;
    let mut comment: Option<String> = None;
    let mut cur: Option<(usize, usize)> = None; // (build, patch) being filled
    let mut total = 0usize;
    for (n, raw) in text.lines().enumerate() {
        let ln = n + 1;
        if raw.len() > MAX_LINE {
            return err(format!("line {ln}: too long"));
        }
        let (code, cm) = strip_comment(raw);
        let line = code.trim();
        if line.is_empty() {
            if let Some(c) = cm {
                comment = Some(c.to_string());
            }
            continue;
        }
        if let Some(d) = line.strip_prefix('@') {
            let (word, arg) = match d.split_once(char::is_whitespace) {
                Some((w, a)) => (w, a.trim()),
                None => (d, ""),
            };
            if let Some(id) = word.strip_prefix("nsobid-") {
                let build_id = norm_build_id(id).or_else(|e| err(format!("line {ln}: {e}")))?;
                match out.builds.iter().position(|b| b.build_id == build_id) {
                    Some(i) => cur = Some((i, usize::MAX)),
                    None => {
                        out.builds.push(PchBuild {
                            build_id,
                            patches: vec![],
                        });
                        cur = Some((out.builds.len() - 1, usize::MAX));
                    }
                }
            } else if word == "enabled" || word == "disabled" {
                let Some((b, _)) = cur else {
                    return err(format!("line {ln}: @{word} before any @nsobid-"));
                };
                let name = comment
                    .take()
                    .filter(|c| !c.is_empty())
                    .unwrap_or_else(|| format!("patch{}", out.builds[b].patches.len() + 1));
                out.builds[b].patches.push(PchPatch {
                    name,
                    enabled: word == "enabled",
                    records: vec![],
                });
                cur = Some((b, out.builds[b].patches.len() - 1));
            } else if word == "flag" {
                let (k, v) = arg.split_once(char::is_whitespace).unwrap_or((arg, ""));
                if k == "offset_shift" {
                    shift = parse_num(v).or_else(|e| err(format!("line {ln}: {e}")))?;
                    if shift > 0xFFFF_FFFF {
                        return err(format!("line {ln}: offset_shift too large"));
                    }
                } else {
                    out.warnings
                        .push(format!("line {ln}: unknown flag {k:?} ignored"));
                }
            } else if word == "stop" {
                break;
            } else {
                out.warnings
                    .push(format!("line {ln}: unknown directive @{word} ignored"));
            }
            comment = None;
            continue;
        }
        // an address/value record
        let Some((b, p)) = cur else {
            return err(format!("line {ln}: record before any @nsobid-"));
        };
        if p == usize::MAX {
            return err(format!(
                "line {ln}: record outside an @enabled/@disabled block"
            ));
        }
        let (a, v) = line
            .split_once(char::is_whitespace)
            .ok_or(crate::util::ModError(format!(
                "line {ln}: expected ADDRESS VALUE"
            )))?;
        let addr = parse_num(a).or_else(|e| err(format!("line {ln}: {e}")))?;
        let data = parse_value(v).or_else(|e| err(format!("line {ln}: {e}")))?;
        let offset = addr + shift;
        if offset > 0xFFFF_FFFF {
            return err(format!("line {ln}: address beyond 4 GiB"));
        }
        total += 1;
        if total > MAX_RECORDS {
            return err("too many records");
        }
        out.builds[b].patches[p]
            .records
            .push(PchRecord { offset, data });
        comment = None;
    }
    Ok(out)
}

/// Writes canonical pchtxt (no shift: offsets are written as file offsets).
pub fn write(p: &Pchtxt) -> String {
    let mut s = String::new();
    for b in &p.builds {
        let id = crate::util::build_id_hex(&b.build_id);
        s.push_str(&format!("@nsobid-{id}\n\n"));
        for pa in &b.patches {
            s.push_str(&format!(
                "// {}\n@{}\n",
                pa.name.replace('\n', " "),
                if pa.enabled { "enabled" } else { "disabled" }
            ));
            for r in &pa.records {
                s.push_str(&format!(
                    "{:08X} {}\n",
                    r.offset,
                    hex_encode(&r.data).to_ascii_uppercase()
                ));
            }
            s.push('\n');
        }
    }
    s.push_str("@stop\n");
    s
}

pub fn to_json(p: &Pchtxt) -> Value {
    json!({
        "format": "napple-patch-json-1",
        "builds": p.builds.iter().map(|b| json!({
            "buildId": crate::util::build_id_hex(&b.build_id),
            "patches": b.patches.iter().map(|pa| json!({
                "name": pa.name,
                "enabled": pa.enabled,
                "records": pa.records.iter().map(|r| json!({
                    "offset": r.offset,
                    "data": hex_encode(&r.data),
                })).collect::<Vec<_>>(),
            })).collect::<Vec<_>>(),
        })).collect::<Vec<_>>(),
    })
}

pub fn from_json(v: &Value) -> Result<Pchtxt> {
    let mut out = Pchtxt::default();
    if v.get("format").and_then(Value::as_str) != Some("napple-patch-json-1") {
        return err("not a napple-patch-json-1 document");
    }
    let builds = v
        .get("builds")
        .and_then(Value::as_array)
        .ok_or(crate::util::ModError("missing builds[]".into()))?;
    for b in builds {
        let id = b
            .get("buildId")
            .and_then(Value::as_str)
            .ok_or(crate::util::ModError("missing buildId".into()))?;
        let mut pb = PchBuild {
            build_id: norm_build_id(id)?,
            patches: vec![],
        };
        for pa in b
            .get("patches")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let mut pp = PchPatch {
                name: pa
                    .get("name")
                    .and_then(Value::as_str)
                    .unwrap_or("patch")
                    .to_string(),
                enabled: pa.get("enabled").and_then(Value::as_bool).unwrap_or(true),
                records: vec![],
            };
            for r in pa
                .get("records")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                let offset = r
                    .get("offset")
                    .and_then(Value::as_u64)
                    .filter(|&o| o <= 0xFFFF_FFFF)
                    .ok_or(crate::util::ModError("bad record offset".into()))?;
                let data = hex_decode(r.get("data").and_then(Value::as_str).unwrap_or(""))?;
                if data.is_empty() {
                    return err("empty record data");
                }
                pp.records.push(PchRecord { offset, data });
            }
            pb.patches.push(pp);
        }
        out.builds.push(pb);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "@nsobid-0123456789ABCDEF0123456789ABCDEF\n\
@flag offset_shift 0x100\n\
// Widescreen 21:9 [someone]\n@enabled\n00001000 1F2003D5 // nop\n00001004 \"hi\\n\"\n\
// Debug menu\n@disabled\n00002000 0x00112233\n\
@nsobid-AA\n// other build\n@enabled\n10 FF\n@stop\n99 99\n";

    #[test]
    fn parses_blocks_shift_comments_builds() {
        let p = parse(SAMPLE).unwrap();
        assert_eq!(p.builds.len(), 2);
        let b = &p.builds[0];
        assert_eq!(b.patches.len(), 2);
        assert_eq!(b.patches[0].name, "Widescreen 21:9 [someone]");
        assert!(b.patches[0].enabled && !b.patches[1].enabled);
        assert_eq!(b.patches[0].records[0].offset, 0x1100);
        assert_eq!(b.patches[0].records[0].data, vec![0x1F, 0x20, 0x03, 0xD5]);
        assert_eq!(b.patches[0].records[1].data, b"hi\n");
        assert_eq!(b.patches[1].records[0].data, vec![0, 0x11, 0x22, 0x33]);
        assert_eq!(p.builds[1].build_id[0], 0xAA);
        assert_eq!(
            p.builds[1].patches[0].records.len(),
            1,
            "@stop ends parsing"
        );
    }

    #[test]
    fn round_trips_through_pchtxt_and_json() {
        let p = parse(SAMPLE).unwrap();
        let again = parse(&write(&p)).unwrap();
        assert_eq!(again.builds, p.builds);
        let j = from_json(&to_json(&p)).unwrap();
        assert_eq!(j.builds, p.builds);
    }

    #[test]
    fn errors() {
        assert!(parse("00001000 00").is_err());
        assert!(parse("@enabled\n").is_err());
        assert!(parse("@nsobid-AA\n10 00\n").is_err());
        assert!(parse("@nsobid-AA\n@enabled\nzz 00\n").is_err());
        assert!(parse("@nsobid-AA\n@enabled\n10 0\n").is_err());
        assert!(parse("@nsobid-AA\n@enabled\n10 \"abc\n").is_err());
        assert!(parse("@nsobid-xyz\n").is_err());
        assert!(parse("@nsobid-AA\n@flag offset_shift 0xFFFFFFFFF\n").is_err());
        let w = parse("@nsobid-AA\n@weird\n@flag print_values on\n").unwrap();
        assert_eq!(w.warnings.len(), 2);
    }

    #[test]
    fn corpus_never_panics() {
        let mut s = 0x9e37_79b9_7f4a_7c15u64;
        let base = SAMPLE.as_bytes();
        for _ in 0..4000 {
            let mut m = base.to_vec();
            for _ in 0..(crate::util::xorshift(&mut s) % 5 + 1) {
                let i = crate::util::xorshift(&mut s) as usize % m.len();
                m[i] = crate::util::xorshift(&mut s) as u8;
            }
            let t = String::from_utf8_lossy(&m);
            let _ = parse(&t);
        }
        let _ = parse("\u{0}\u{ffff}\"\\\"@@@@//");
        let _ = from_json(&json!({"format": 1}));
        let _ = from_json(&json!({"format": "napple-patch-json-1", "builds": [{"buildId": 5}]}));
    }
}
