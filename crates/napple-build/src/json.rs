//! Tiny JSON writer helpers (the build tool emits JSON by hand; manifests contain ids, sizes and hashes only).
pub fn jstr(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// `{"k":v,...}` from already-encoded values, one per line when `pretty`.
pub fn obj(fields: &[(&str, String)], pretty: bool) -> String {
    let sep = if pretty { ",\n  " } else { "," };
    let body: Vec<String> = fields
        .iter()
        .map(|(k, v)| format!("{}:{}", jstr(k), v))
        .collect();
    if pretty {
        format!("{{\n  {}\n}}", body.join(sep))
    } else {
        format!("{{{}}}", body.join(sep))
    }
}

pub fn arr(items: &[String]) -> String {
    format!("[{}]", items.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn escapes() {
        assert_eq!(jstr("a\"b\\c\n\u{1}"), "\"a\\\"b\\\\c\\n\\u0001\"");
        assert_eq!(
            obj(&[("a", "1".into()), ("b", jstr("x"))], false),
            "{\"a\":1,\"b\":\"x\"}"
        );
        assert_eq!(arr(&["1".into(), "2".into()]), "[1,2]");
    }
}
