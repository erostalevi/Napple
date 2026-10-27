//! Shader replacements: `shaders/<program-hash>.msl`. Parsed and validated only; the gfx hook is a later Mac step.
use crate::util::{Result, err};

pub const MAX_SHADER: usize = 1 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShaderInfo {
    /// Lower-case hex program hash from the file name.
    pub program_hash: String,
    pub entry_points: Vec<String>,
}

/// `path` is the archive path, e.g. `shaders/0123abcd...msl`.
pub fn validate(path: &str, bytes: &[u8]) -> Result<ShaderInfo> {
    let Some(stem) = path
        .strip_prefix("shaders/")
        .and_then(|p| p.strip_suffix(".msl"))
    else {
        return err(format!(
            "{path}: shader replacements live at shaders/<hash>.msl"
        ));
    };
    if stem.len() < 16 || stem.len() > 64 || !stem.bytes().all(|b| b.is_ascii_hexdigit()) {
        return err(format!("{path}: program hash must be 16..=64 hex digits"));
    }
    if bytes.len() > MAX_SHADER {
        return err(format!("{path}: larger than 1 MiB"));
    }
    let Ok(src) = std::str::from_utf8(bytes) else {
        return err(format!("{path}: not UTF-8"));
    };
    if src.contains('\0') {
        return err(format!("{path}: NUL byte"));
    }
    let mut depth = 0i64;
    let mut entries = Vec::new();
    for (n, line) in src.lines().enumerate() {
        let code = line.split("//").next().unwrap_or("");
        for c in code.chars() {
            match c {
                '{' | '(' => depth += 1,
                '}' | ')' => depth -= 1,
                _ => {}
            }
            if depth < 0 {
                return err(format!("{path}:{}: unbalanced closing bracket", n + 1));
            }
        }
        let t = code.trim_start();
        for q in ["vertex ", "fragment ", "kernel "] {
            if let Some(rest) = t.strip_prefix(q) {
                let mut it = rest.split_whitespace();
                let _ret = it.next();
                if let Some(name) = it.next() {
                    let name: String = name
                        .chars()
                        .take_while(|c| c.is_alphanumeric() || *c == '_')
                        .collect();
                    if !name.is_empty() {
                        entries.push(name);
                    }
                }
            }
        }
    }
    if depth != 0 {
        return err(format!("{path}: unbalanced brackets"));
    }
    if !src.contains("#include <metal_stdlib>") {
        return err(format!("{path}: must include <metal_stdlib>"));
    }
    if entries.is_empty() {
        return err(format!("{path}: no vertex/fragment/kernel entry point"));
    }
    Ok(ShaderInfo {
        program_hash: stem.to_ascii_lowercase(),
        entry_points: entries,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const OK: &str = "#include <metal_stdlib>\nusing namespace metal;\nfragment float4 fs_main(float4 p [[stage_in]]) { return p; }\n";

    #[test]
    fn accepts_and_rejects() {
        let p = "shaders/0123456789abcdef.msl";
        assert_eq!(
            validate(p, OK.as_bytes()).unwrap().entry_points,
            vec!["fs_main"]
        );
        assert!(validate("shaders/zz.msl", OK.as_bytes()).is_err());
        assert!(validate("other/0123456789abcdef.msl", OK.as_bytes()).is_err());
        assert!(validate(p, b"fragment float4 f() { }").is_err());
        assert!(validate(p, b"#include <metal_stdlib>\nvoid f() {}").is_err());
        assert!(validate(p, b"#include <metal_stdlib>\nfragment int f() { ").is_err());
        assert!(validate(p, b"#include <metal_stdlib>\n} fragment int f() {}").is_err());
        assert!(validate(p, &[0xFF, 0xFE]).is_err());
    }
}
