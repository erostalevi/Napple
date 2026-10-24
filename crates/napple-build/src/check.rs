//! `napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]`: validates keys and firmware (never prints key values).
use crate::error::{BuildError, Result};
use crate::firmware::Firmware;
use crate::json::{jstr, obj};
use crate::sysdata;
use std::path::PathBuf;

/// "00-15, 17" style ranges of the generations for which `f` holds.
fn ranges(f: impl Fn(u32) -> bool) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut g = 0u32;
    while g <= 0x20 {
        if f(g) {
            let s = g;
            while g < 0x20 && f(g + 1) {
                g += 1;
            }
            out.push(if s == g { format!("{s:02x}") } else { format!("{s:02x}-{g:02x}") });
        }
        g += 1;
    }
    if out.is_empty() { "none".into() } else { out.join(", ") }
}

pub fn run(args: &[String]) -> std::result::Result<(), String> {
    run_inner(args).map_err(|e| e.to_string())
}

fn run_inner(args: &[String]) -> Result<()> {
    let (mut keys, mut fw, mut json) = (None::<PathBuf>, None::<PathBuf>, false);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--check" | "--list-keys-check" => {}
            "--json" => json = true,
            "-k" => keys = Some(it.next().ok_or_else(|| BuildError::usage("-k needs a value"))?.into()),
            "--firmware" => fw = Some(it.next().ok_or_else(|| BuildError::usage("--firmware needs a value"))?.into()),
            x => return Err(BuildError::usage(format!("unknown argument {x}\nusage: napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]"))),
        }
    }
    let keys_path = keys.ok_or_else(|| BuildError::usage("usage: napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]"))?;
    let ks = crate::util::load_keys(&keys_path)?;
    let header = ks.has("header_key") || (ks.has("header_key_source") && ks.has("master_key_00") && ks.has("aes_kek_generation_source"));
    let app_gens = ranges(|g| ks.has(&format!("key_area_key_application_{g:02x}")));
    let tk_gens = ranges(|g| ks.has(&format!("titlekek_{g:02x}")));
    let master = ranges(|g| ks.has(&format!("master_key_{g:02x}")));
    let max_gen = (0..=0x20u32).rev().find(|g| ks.has(&format!("key_area_key_application_{g:02x}")) && ks.has(&format!("titlekek_{g:02x}")));
    let mut problems: Vec<String> = Vec::new();
    if !header {
        problems.push("header_key is missing (and cannot be derived)".into());
    }
    if max_gen.is_none() {
        problems.push("no key generation has both key_area_key_application and titlekek".into());
    }
    let mut fw_json = "null".to_string();
    let mut fw_lines: Vec<String> = Vec::new();
    if let Some(p) = &fw {
        match Firmware::open(p) {
            Err(e) => problems.push(e.msg),
            Ok(f) => {
                fw_lines.push(format!("firmware: {} NCA files", f.nca_count()));
                match (sysdata::firmware_version(&f, &ks), sysdata::find_fonts(&f, &ks)) {
                    (Ok(v), Ok(fonts)) => {
                        let n = sysdata::official_count(&fonts);
                        fw_lines.push(format!("firmware version: {}", v.as_deref().unwrap_or("unknown")));
                        fw_lines.push(format!("shared fonts: {n} of 6 types ({} files)", fonts.len()));
                        if n < 6 {
                            problems.push(format!("only {n} of 6 shared font types found in the firmware"));
                        }
                        fw_json = obj(&[("nca_files", f.nca_count().to_string()), ("version", v.as_deref().map(jstr).unwrap_or_else(|| "null".into())), ("font_types", n.to_string())], false);
                    }
                    (Err(e), _) | (_, Err(e)) => problems.push(format!("firmware cannot be read with these keys: {}", e.msg)),
                }
            }
        }
    }
    let ok = problems.is_empty();
    if json {
        println!(
            "{}",
            obj(
                &[
                    ("ok", ok.to_string()),
                    ("header_key", header.to_string()),
                    ("max_generation", max_gen.map(|g| g.to_string()).unwrap_or_else(|| "null".into())),
                    ("firmware", fw_json),
                    ("problems", format!("[{}]", problems.iter().map(|p| jstr(p)).collect::<Vec<_>>().join(","))),
                ],
                false
            )
        );
    } else {
        println!("keys: header_key {}", if header { "present" } else { "MISSING" });
        println!("keys: master_key generations {master}");
        println!("keys: key_area_key_application generations {app_gens}");
        println!("keys: titlekek generations {tk_gens}");
        if let Some(g) = max_gen {
            println!("keys: newest usable generation {g:02x} (games/firmware needing a newer key will fail with its name)");
        }
        fw_lines.iter().for_each(|l| println!("{l}"));
        problems.iter().for_each(|p| println!("problem: {p}"));
        println!("{}", if ok { "check passed" } else { "check FAILED" });
    }
    if ok { Ok(()) } else { Err(BuildError::new("check", problems.join("; "))) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_formatting() {
        assert_eq!(ranges(|g| g <= 2 || g == 4 || (6..=9).contains(&g)), "00-02, 04, 06-09");
        assert_eq!(ranges(|_| false), "none");
        assert_eq!(ranges(|g| g == 0x20), "20");
    }
}
