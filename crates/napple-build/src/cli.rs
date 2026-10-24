//! Argument parsing for `napple build <files...> -k <keys> ... -o <outdir>`.
use crate::error::{BuildError, Result};
use crate::pipeline::{Opts, UpdateRomfs};
use napple_core::application::package::{Choice, DlcChoice};
use std::path::PathBuf;

pub const USAGE: &str = "usage:
  napple build <game files...> -k <prod.keys> -o <outdir> [options]     build <outdir>/<Name>.app
      --firmware <zip|dir>          Firmware zip or directory of NCAs (shared system fonts); without it the build works but flags a compat note
      --title-keys <title.keys>     accepted for Studio compatibility (tickets inside NSPs are used)
      --updates all|latest|none|<v,v>   updates to embed (default latest)
      --dlc all|none|<idhex,idhex>      DLC to embed (default all)
      --target macos                only macos is implemented
      --name <app name>             default: the name in the game's NACP
      --runtime <path>              runtime binary (default: next to napple, or runtime/build/napple-runtime in a checkout)
      --launcher <path>             launcher binary (default: packaging/build/napple-launcher)
      --make-runtime                run make -C runtime and make -C packaging first (checkout only)
      --update-romfs materialize|delta  one patched romfs.bin per update (default) or a BKTR delta layer
      --keep-cache                  keep <outdir>/.napple-cache (resumable RomFS copies)
      --no-sign                     skip ad-hoc codesign
      --data-only                   write <outdir>/<titleid>/<buildid16>/ data directories only (runtime development; no .app)
  napple build --check -k <prod.keys> [--firmware <zip|dir>] [--json]   validate keys/firmware (never prints key values)
  napple build --dump-romfs <game> -k <keys> -o <file> | --dump-exefs <game> -k <keys> -o <dir> | --dump-fonts <firmware> -k <keys> -o <dir>
Stdout: PROGRESS <stage> <percent> <message> lines, then RESULT <path to the .app>. Errors: `error: [code] message` on stderr.";

pub fn parse_updates(v: &str) -> Result<Choice> {
    Ok(match v {
        "all" => Choice::All,
        "latest" => Choice::Latest,
        "none" => Choice::None,
        v => Choice::Only(v.split(',').map(|s| s.trim().parse().map_err(|_| BuildError::usage(format!("--updates: {s:?} is not a version number")))).collect::<Result<_>>()?),
    })
}

pub fn parse_dlc(v: &str) -> Result<DlcChoice> {
    Ok(match v {
        "all" => DlcChoice::All,
        "none" => DlcChoice::None,
        v => DlcChoice::Only(v.split(',').map(|s| u64::from_str_radix(s.trim().trim_start_matches("0x"), 16).map_err(|_| BuildError::usage(format!("--dlc: {s:?} is not a hex title id")))).collect::<Result<_>>()?),
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
    };
    let (mut have_keys, mut have_out) = (false, false);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || it.next().cloned().ok_or_else(|| BuildError::usage(format!("{a} needs a value\n{USAGE}")));
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
            "--name" => o.name = Some(val()?),
            "--runtime" => o.runtime = Some(val()?.into()),
            "--launcher" => o.launcher = Some(val()?.into()),
            "--make-runtime" => o.make_runtime = true,
            "--keep-cache" => o.keep_cache = true,
            "--no-sign" => o.no_sign = true,
            "--data-only" => o.data_only = true,
            "--update-romfs" => {
                o.update_romfs = match val()?.as_str() {
                    "materialize" => UpdateRomfs::Materialize,
                    "delta" => UpdateRomfs::Delta,
                    x => return Err(BuildError::usage(format!("--update-romfs: expected materialize|delta, got {x:?}"))),
                }
            }
            "-h" | "--help" => return Err(BuildError::usage(USAGE)),
            x if x.starts_with('-') && x.len() > 1 => return Err(BuildError::usage(format!("unknown option {x}\n{USAGE}"))),
            f => o.files.push(f.into()),
        }
    }
    if o.files.is_empty() || !have_keys || !have_out {
        return Err(BuildError::usage(USAGE));
    }
    Ok(o)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn parses_studio_invocation() {
        let o = parse(&s(&["a.nsp", "b.nsp", "-k", "prod.keys", "--title-keys", "t.keys", "--firmware", "fw.zip", "--updates", "65536,131072", "--dlc", "0100000000001001,none".split(',').next().unwrap(), "--target", "macos", "-o", "out"])).unwrap();
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
        let o = parse(&s(&["a", "-k", "k", "-o", "o", "--updates", "none", "--dlc", "none", "--update-romfs", "delta"])).unwrap();
        assert!(matches!(o.updates, Choice::None) && matches!(o.dlc, DlcChoice::None) && o.update_romfs == UpdateRomfs::Delta);
    }
}
