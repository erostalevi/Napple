//! Argument parsing for `napple build <files...> -k <keys> ... -o <outdir>`.
use crate::error::{BuildError, Result};
use crate::ios::Signing;
use crate::pipeline::{Opts, UpdateRomfs};
use napple_core::application::package::{Choice, DlcChoice};
use std::path::PathBuf;

pub const USAGE: &str = "usage:
  napple build <game files or folders...> -k <prod.keys> -o <outdir> [options]     build <outdir>/<Name>.app
      --firmware <zip|dir>          Firmware zip or directory of NCAs (shared system fonts); without it the build works but flags a compat note
      --title-keys <title.keys>     accepted for Studio compatibility (tickets inside NSPs are used)
      --updates all|latest|none|<v,v>   updates to embed (default latest)
      --dlc all|none|<idhex,idhex>      DLC to embed (default all)
      --target macos|ios|iphone|ipad|ios-sim   macos (default); ios = iPhone+iPad, iPadOS/iOS 26 or later (needs signing); ios-sim (or --sim) = iOS Simulator build, ad-hoc signed, smoke tests only
      --signing-identity <name|sha1>   iOS: certificate for codesign (see `napple build --list-signing`)
      --provisioning-profile <file>    iOS: .mobileprovision covering the bundle id and your device
      --bundle-id <id>                 iOS: bundle identifier (default com.napple.game.<titleid>; a free Apple ID needs the id its profile was made for)
      --ipa                            iOS: also write <Name>.ipa
      --install <device>               iOS: install on this device with `xcrun devicectl` (identifier, UDID or name); --launch starts it
      --name <app name>             default: the name in the game's NACP
      --runtime <path>              runtime binary (default: next to napple, or runtime/build/napple-runtime in a checkout)
      --launcher <path>             launcher binary (default: packaging/build/napple-launcher)
      --make-runtime                run make -C runtime and make -C packaging first (checkout only)
      --update-romfs materialize|delta|in-place  one patched romfs.bin per update (default), a BKTR delta layer, or (with --data-only, development) a romfs.map per version that reads the RomFS straight out of the game files (no copy; the map holds decryption keys)
      --mods <a.napmod,dir,...>     apply community mods (code patches before the rewrite, romfs overlay, cheats); passing a mod enables it; manifest.json records the set
      --keep-cache                  keep <outdir>/.napple-cache (resumable RomFS copies)
      --no-sign                     skip codesign (macOS: no signature; iOS: unsigned app/IPA for an external signer)
      --data-only                   write <outdir>/<titleid>/<buildid16>/ data directories only (runtime development; no .app)
  napple build --list-signing [--json]    iOS signing identities and provisioning profiles on this Mac
  napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]   validate keys/firmware (never prints key values)
  napple build --dump-romfs <game> -k <keys> -o <file> | --dump-exefs <game> -k <keys> -o <dir> | --dump-fonts <firmware> -k <keys> -o <dir>
Stdout: PROGRESS <stage> <percent> <message> lines, then RESULT <path to the .app>. Errors: `error: [code] message` on stderr.";

pub fn parse_updates(v: &str) -> Result<Choice> {
    Ok(match v {
        "all" => Choice::All,
        "latest" => Choice::Latest,
        "none" => Choice::None,
        v => Choice::Only(
            v.split(',')
                .map(|s| {
                    s.trim().parse().map_err(|_| {
                        BuildError::usage(format!("--updates: {s:?} is not a version number"))
                    })
                })
                .collect::<Result<_>>()?,
        ),
    })
}

pub fn parse_dlc(v: &str) -> Result<DlcChoice> {
    Ok(match v {
        "all" => DlcChoice::All,
        "none" => DlcChoice::None,
        v => DlcChoice::Only(
            v.split(',')
                .map(|s| {
                    u64::from_str_radix(s.trim().trim_start_matches("0x"), 16).map_err(|_| {
                        BuildError::usage(format!("--dlc: {s:?} is not a hex title id"))
                    })
                })
                .collect::<Result<_>>()?,
        ),
    })
}

pub fn parse(args: &[String]) -> Result<Opts> {
    let mut o = Opts {
        files: vec![],
        keys: PathBuf::new(),
        title_keys: None,
        firmware: None,
        updates: Choice::Latest,
        dlc: DlcChoice::All,
        target: "macos".into(),
        out: PathBuf::new(),
        name: None,
        runtime: None,
        launcher: None,
        make_runtime: false,
        update_romfs: UpdateRomfs::Materialize,
        keep_cache: false,
        no_sign: false,
        data_only: false,
        signing: Signing::default(),
        launch: false,
        mods: vec![],
    };
    let (mut have_keys, mut have_out) = (false, false);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || {
            it.next()
                .cloned()
                .ok_or_else(|| BuildError::usage(format!("{a} needs a value\n{USAGE}")))
        };
        match a.as_str() {
            "-k" => {
                o.keys = val()?.into();
                have_keys = true;
            }
            "-o" => {
                o.out = val()?.into();
                have_out = true;
            }
            "--title-keys" => o.title_keys = Some(val()?.into()),
            "--firmware" => o.firmware = Some(val()?.into()),
            "--updates" => o.updates = parse_updates(&val()?)?,
            "--dlc" => o.dlc = parse_dlc(&val()?)?,
            "--target" => o.target = val()?,
            "--sim" => o.target = "ios-sim".into(),
            "--name" => o.name = Some(val()?),
            "--runtime" => o.runtime = Some(val()?.into()),
            "--launcher" => o.launcher = Some(val()?.into()),
            "--make-runtime" => o.make_runtime = true,
            "--keep-cache" => o.keep_cache = true,
            "--no-sign" => o.no_sign = true,
            "--data-only" => o.data_only = true,
            "--signing-identity" => o.signing.identity = Some(val()?),
            "--provisioning-profile" => o.signing.profile = Some(val()?.into()),
            "--bundle-id" => {
                let b = val()?;
                if !crate::ios::valid_bundle_id(&b) {
                    return Err(BuildError::usage(format!(
                        "--bundle-id: {b:?} is not a reverse-DNS identifier like com.example.game"
                    )));
                }
                o.signing.bundle_id = Some(b);
            }
            "--ipa" => o.signing.ipa = true,
            "--install" => o.signing.install = Some(val()?),
            "--launch" => o.launch = true,
            "--mods" => {
                for m in val()?.split(',').filter(|m| !m.is_empty()) {
                    o.mods.push(m.into());
                }
            }
            "--update-romfs" => {
                o.update_romfs = match val()?.as_str() {
                    "materialize" => UpdateRomfs::Materialize,
                    "delta" => UpdateRomfs::Delta,
                    "in-place" => UpdateRomfs::InPlace,
                    x => {
                        return Err(BuildError::usage(format!(
                            "--update-romfs: expected materialize|delta|in-place, got {x:?}"
                        )));
                    }
                }
            }
            "-h" | "--help" => return Err(BuildError::usage(USAGE)),
            x if x.starts_with('-') && x.len() > 1 => {
                return Err(BuildError::usage(format!("unknown option {x}\n{USAGE}")));
            }
            f => o.files.push(f.into()),
        }
    }
    o.files = o.files.iter().flat_map(|f| expand_dir(f)).collect();
    if crate::ios::Target::parse(&o.target).is_none() {
        return Err(BuildError::usage(format!(
            "--target: expected macos|ios|iphone|ipad|ios-sim, got {:?}",
            o.target
        )));
    }
    let tgt = crate::ios::Target::parse(&o.target).unwrap();
    if tgt.is_sim()
        && (o.signing.identity.is_some()
            || o.signing.profile.is_some()
            || o.signing.ipa
            || o.signing.install.is_some())
    {
        return Err(BuildError::usage(
            "--target ios-sim is ad-hoc signed and installed with `xcrun simctl`: --signing-identity/--provisioning-profile/--ipa/--install do not apply",
        ));
    }
    if !tgt.is_ios()
        && (o.signing.ipa
            || o.signing.install.is_some()
            || o.signing.identity.is_some()
            || o.signing.profile.is_some()
            || o.signing.bundle_id.is_some())
    {
        return Err(BuildError::usage(
            "--signing-identity/--provisioning-profile/--bundle-id/--ipa/--install need --target ios, iphone or ipad",
        ));
    }
    if o.files.is_empty() || !have_keys || !have_out {
        return Err(BuildError::usage(USAGE));
    }
    Ok(o)
}

/// A directory argument stands for every game container inside it, recursively (a DLC set often sits in a subfolder next to the base game);
/// anything else (files, paths that do not exist) is kept as given so the build reports its own error.
fn expand_dir(p: &std::path::Path) -> Vec<PathBuf> {
    if !p.is_dir() {
        return vec![p.to_path_buf()];
    }
    let mut out = Vec::new();
    let mut ents: Vec<_> = std::fs::read_dir(p)
        .map(|d| d.flatten().map(|e| e.path()).collect())
        .unwrap_or_default();
    ents.sort();
    for e in ents {
        if e.is_dir() {
            out.extend(expand_dir(&e));
        } else if e.extension().and_then(|x| x.to_str()).is_some_and(|x| {
            ["nsp", "xci", "nsz", "xcz"].contains(&x.to_ascii_lowercase().as_str())
        }) {
            out.push(e);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn parses_studio_invocation() {
        let o = parse(&s(&[
            "a.nsp",
            "b.nsp",
            "-k",
            "prod.keys",
            "--title-keys",
            "t.keys",
            "--firmware",
            "fw.zip",
            "--updates",
            "65536,131072",
            "--dlc",
            "0100000000001001,none".split(',').next().unwrap(),
            "--target",
            "macos",
            "-o",
            "out",
        ]))
        .unwrap();
        assert_eq!(o.files.len(), 2);
        assert!(matches!(o.updates, Choice::Only(ref v) if *v == [65536, 131072]));
        assert!(matches!(o.dlc, DlcChoice::Only(ref v) if *v == [0x0100000000001001]));
        assert_eq!(o.target, "macos");
        assert_eq!(o.firmware.as_deref(), Some(std::path::Path::new("fw.zip")));
    }

    #[test]
    fn rejects_bad_input() {
        assert!(parse(&s(&["a.nsp", "-k", "k"])).unwrap_err().is_usage()); // no -o
        assert!(parse(&s(&["-k", "k", "-o", "o"])).unwrap_err().is_usage()); // no files
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--updates", "x"])).is_err());
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--dlc", "zz"])).is_err());
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--bogus"])).is_err());
        assert!(parse(&s(&["a", "-k", "k", "-o"])).is_err());
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--update-romfs", "x"])).is_err());
        let o = parse(&s(&[
            "a",
            "-k",
            "k",
            "-o",
            "o",
            "--target",
            "ios",
            "--signing-identity",
            "Apple Development: x",
            "--provisioning-profile",
            "p.mobileprovision",
            "--bundle-id",
            "com.me.g",
            "--ipa",
            "--install",
            "my-iphone",
            "--launch",
        ]))
        .unwrap();
        assert_eq!(
            (
                o.target.as_str(),
                o.signing.bundle_id.as_deref(),
                o.signing.ipa,
                o.signing.install.as_deref(),
                o.launch
            ),
            ("ios", Some("com.me.g"), true, Some("my-iphone"), true)
        );
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--target", "tvos"])).is_err());
        let o = parse(&s(&[
            "a",
            "-k",
            "k",
            "-o",
            "o",
            "--target",
            "ios-sim",
            "--bundle-id",
            "com.me.sim",
        ]))
        .unwrap();
        assert_eq!(
            (o.target.as_str(), o.signing.bundle_id.as_deref()),
            ("ios-sim", Some("com.me.sim"))
        );
        assert!(
            parse(&s(&[
                "a",
                "-k",
                "k",
                "-o",
                "o",
                "--target",
                "ios-sim",
                "--signing-identity",
                "x"
            ]))
            .is_err()
        );
        assert!(parse(&s(&["a", "-k", "k", "-o", "o", "--ipa"])).is_err()); // ipa needs an iOS target
        assert!(
            parse(&s(&[
                "a",
                "-k",
                "k",
                "-o",
                "o",
                "--target",
                "ios",
                "--bundle-id",
                "nodots"
            ]))
            .is_err()
        );
        let o = parse(&s(&[
            "a",
            "-k",
            "k",
            "-o",
            "o",
            "--updates",
            "none",
            "--dlc",
            "none",
            "--update-romfs",
            "delta",
        ]))
        .unwrap();
        assert!(
            matches!(o.updates, Choice::None)
                && matches!(o.dlc, DlcChoice::None)
                && o.update_romfs == UpdateRomfs::Delta
        );
    }
}
