//! `napple build --firmware-program <title id hex> --firmware <zip|dir> -k <prod.keys> -o <dir>`: turns a firmware *system program*
//! (an applet such as MiiEdit 0100000000001009, a library applet, a sysmodule) into a normal runtime data directory, so the
//! runtime can boot it standalone: its ExeFS (main.npdm + NSOs) is AOT-rewritten like any title, its RomFS becomes romfs.bin and the
//! system data archives and fonts are written next to it. Layout: `<dir>/<title id>/{napple-app.json, default, sysdata/, system/fonts/, <build id>/...}`.
use crate::error::{BuildError, Result};
use crate::firmware::Firmware;
use crate::json::{arr, hex, jstr, obj};
use crate::pipeline::{load_order, rewrite_version};
use crate::sysdata;
use napple_core::infrastructure::io::read_vec;
use napple_core::infrastructure::nca::{self, FsType, Nca, TitleKeys};
use napple_core::infrastructure::nso::Nso;
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::romfs::RomFs;
use std::path::{Path, PathBuf};

/// Finds the function a stripped module uses as `nvnBootstrapLoader`: the callee of the call that follows the use of the string "nvnDeviceGetProcAddress"
/// (the SDK's NVN loader asks its own bootstrap for the proc-address getter). `image` is the rewritten module image (segments in place). Returns its offset.
pub(crate) fn find_nvn_bootstrap(image: &[u8], text_end: usize) -> Option<u64> {
    let needle = b"nvnDeviceGetProcAddress\0";
    let target = image.windows(needle.len()).position(|w| w == needle)? as u64;
    let word = |o: usize| u32::from_le_bytes(image[o..o + 4].try_into().unwrap());
    let end = text_end.min(image.len()).saturating_sub(64);
    let mut o = 0usize;
    while o < end {
        let w = word(o);
        if w & 0x9F00_0000 == 0x9000_0000 {
            let rd = w & 31;
            let mut imm = (((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)) as i64;
            if imm & 0x10_0000 != 0 {
                imm -= 0x20_0000;
            }
            let page = (((o as i64) >> 12) + imm) << 12;
            let w2 = word(o + 4);
            if page as u64 == target & !0xfff
                && w2 & 0xFFC0_0000 == 0x9100_0000
                && (w2 >> 5) & 31 == rd
                && ((w2 >> 10) & 0xFFF) as u64 == target & 0xfff
            {
                for k in 2..8 {
                    let b = word(o + k * 4);
                    if b & 0xFC00_0000 == 0x9400_0000 {
                        let mut off = (b & 0x03FF_FFFF) as i64;
                        if off & 0x0200_0000 != 0 {
                            off -= 0x0400_0000;
                        }
                        return Some(((o + k * 4) as i64 + off * 4) as u64);
                    }
                    if b & 0xFFFF_FC1F == 0xD63F_0000 {
                        break; // an indirect call: this is the proc-address getter use, not the bootstrap
                    }
                }
            }
        }
        o += 4;
    }
    None
}

pub fn run(args: &[String]) -> std::result::Result<(), String> {
    inner(args).map_err(|e| e.to_string())
}

const USAGE: &str = "usage: napple build --firmware-program <title id hex> --firmware <zip|dir> -k <prod.keys> -o <dir> [--name <name>]";

fn inner(args: &[String]) -> Result<()> {
    let (mut id, mut fw, mut keys, mut out, mut name) = (None, None, None, None, None);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = || {
            it.next()
                .cloned()
                .ok_or_else(|| BuildError::usage(format!("{a} needs a value")))
        };
        match a.as_str() {
            "--firmware-program" => {
                id = Some(
                    u64::from_str_radix(val()?.trim_start_matches("0x"), 16).map_err(|_| {
                        BuildError::usage("--firmware-program takes a hex title id")
                    })?,
                )
            }
            "--firmware" => fw = Some(PathBuf::from(val()?)),
            "-k" => keys = Some(PathBuf::from(val()?)),
            "-o" => out = Some(PathBuf::from(val()?)),
            "--name" => name = Some(val()?),
            "--data-only" => {}
            x => return Err(BuildError::usage(format!("unknown argument {x}\n{USAGE}"))),
        }
    }
    let (Some(id), Some(fw), Some(keys), Some(out)) = (id, fw, keys, out) else {
        return Err(BuildError::usage(USAGE));
    };
    let keys = crate::util::load_keys(&keys)?;
    let fwi = Firmware::open(&fw)?;
    let root = build_program(&fwi, &keys, id, &out, name, None)?;
    println!("RESULT {}", root.display());
    Ok(())
}

/// Builds firmware program `id` into `<out>/<title id>/` (see the module comment). `shared`: when the program is built inside a game's data directory,
/// the system data archives, fonts and icon are not written again: `<root>/sysdata` and `<root>/system` become links to these (relative) targets.
pub fn build_program(
    fwi: &Firmware,
    keys: &napple_core::domain::keys::KeySet,
    id: u64,
    out: &Path,
    name: Option<String>,
    shared: Option<(&str, &str)>,
) -> Result<PathBuf> {
    let titles = fwi.titles(keys)?;
    let t = titles
        .iter()
        .rev()
        .find(|t| t.meta.title_id == id)
        .ok_or_else(|| {
            BuildError::new("firmware", format!("title {id:016x} not in the firmware"))
        })?;
    let rec = t
        .meta
        .contents
        .iter()
        .find(|c| c.ctype == 1)
        .ok_or_else(|| {
            BuildError::new(
                "firmware",
                format!("title {id:016x} has no program content"),
            )
        })?;
    let src = fwi
        .nca_src(&rec.nca_name())
        .ok_or_else(|| BuildError::new("firmware", "program NCA missing from the firmware"))?;
    let n = Nca::open(src, keys, &TitleKeys::new())?;

    let mut exefs: Vec<(String, Vec<u8>)> = Vec::new();
    let mut romfs: Option<RomFs> = None;
    for sec in &n.sections {
        let s = n.section(sec.index)?;
        match sec.fs_type {
            FsType::Pfs0 => {
                let fs = parse_pfs0(nca::pfs0_data(&s, sec)?)?;
                for e in &fs.entries {
                    exefs.push((e.name.clone(), fs.read(&e.name)?));
                }
            }
            FsType::RomFs => romfs = Some(RomFs::parse(nca::romfs_data(&s, sec)?)?),
        }
    }
    let main = exefs
        .iter()
        .find(|(n, _)| n == "main")
        .ok_or_else(|| BuildError::new("firmware", "ExeFS has no main"))?;
    let build16 = Nso::parse(&main.1)?.build_id_hex()[..16].to_string();
    let app_name = name.unwrap_or_else(|| {
        exefs
            .iter()
            .find(|(n, _)| n == "main.npdm")
            .and_then(|(_, d)| napple_core::infrastructure::npdm::parse_npdm(d).ok())
            .map(|m| m.name)
            .unwrap_or_else(|| format!("{id:016x}"))
    });

    let root = out.join(format!("{id:016X}"));
    let dir = root.join(&build16);
    std::fs::create_dir_all(&dir).map_err(|e| BuildError::io("create data dir", e))?;
    let mods = rewrite_version(&exefs, &dir, &app_name, 0.0, 100.0)?;
    if let Some((_, d)) = exefs.iter().find(|(n, _)| n == "main.npdm") {
        std::fs::write(dir.join("main.npdm"), d)
            .map_err(|e| BuildError::io("write main.npdm", e))?;
    }
    let (mut rom_size, mut rom_sha) = (0u64, String::new());
    if let Some(r) = &romfs {
        let img = r.image();
        let data = read_vec(&**img, 0, img.size() as usize)?;
        rom_size = data.len() as u64;
        rom_sha = hex(&<sha2::Sha256 as sha2::Digest>::digest(&data));
        std::fs::write(dir.join("romfs.bin"), &data)
            .map_err(|e| BuildError::io("write romfs.bin", e))?;
    }
    let fw_version = sysdata::firmware_version(fwi, keys)?;
    let (mut n_sys, mut fonts) = (0usize, Vec::new());
    if let Some((sys_target, system_target)) = shared {
        // inside a game's data directory: reuse its sysdata and system folders
        for (name, target) in [("sysdata", sys_target), ("system", system_target)] {
            let link = root.join(name);
            let _ = std::fs::remove_file(&link);
            std::os::unix::fs::symlink(target, &link)
                .map_err(|e| BuildError::io("link shared system data", e))?;
        }
    } else {
        n_sys = sysdata::write_system_archives(fwi, keys, &root.join("sysdata"))?.len();
        fonts = sysdata::find_fonts(fwi, keys)?;
        if !fonts.is_empty() {
            sysdata::write_fonts(&fonts, fw_version.as_deref(), &root.join("system/fonts"))?;
        }
    }

    let mut hooks = Vec::new();
    if let (Some(m), Ok(img)) = (
        mods.iter().find(|m| m.name == "main"),
        std::fs::read(dir.join("modules/main.patched.bin")),
    ) {
        let _ = m;
        let text_end = img.len().min(0x4000_0000);
        if let Some(off) = find_nvn_bootstrap(&img, text_end) {
            hooks.push(obj(
                &[
                    ("module", jstr("main")),
                    ("offset", off.to_string()),
                    ("symbol", jstr("napple_applet_nvn_bootstrap")),
                ],
                false,
            ));
        }
    }
    let patches: Vec<String> = Vec::new(); // known-build instruction patches (manifest "patches"): none needed now
    let order = load_order(&mods.iter().map(|m| m.name.clone()).collect::<Vec<_>>());
    let mod_json: Vec<String> = mods
        .iter()
        .map(|m| {
            obj(
                &[
                    ("name", jstr(&m.name)),
                    ("file_size", m.file_size.to_string()),
                    ("sha256", jstr(&m.sha256)),
                    ("build_id", jstr(&hex(&m.build_id))),
                    ("symbols", m.symbols.to_string()),
                    ("relocs", m.relocs.to_string()),
                    ("gates", m.gates.to_string()),
                ],
                false,
            )
        })
        .collect();
    let rom_desc = obj(
        &[
            ("mode", jstr("full")),
            ("file", jstr("romfs.bin")),
            ("size", rom_size.to_string()),
            ("sha256", jstr(&rom_sha)),
        ],
        false,
    );
    let version = t.meta.version;
    let manifest = obj(
        &[
            ("format", "1".into()),
            ("title_id", jstr(&format!("{id:016X}"))),
            ("build_id", jstr(&build16)),
            ("kind", jstr("firmware-program")),
            ("version", version.to_string()),
            ("label", jstr("firmware")),
            (
                "load_order",
                arr(&order.iter().map(|n| jstr(n)).collect::<Vec<_>>()),
            ),
            ("modules", arr(&mod_json)),
            ("nro", "[]".into()),
            ("romfs", rom_desc),
            ("system_dir", jstr("../system")),
            ("dlc", "[]".into()),
            ("hooks", arr(&hooks)),
            ("patches", arr(&patches)),
        ],
        true,
    ) + "\n";
    std::fs::write(dir.join("manifest.json"), manifest)
        .map_err(|e| BuildError::io("write manifest.json", e))?;
    let ver = obj(
        &[
            ("kind", jstr("firmware-program")),
            ("version", version.to_string()),
            ("label", jstr("firmware")),
            ("dir", jstr(&build16)),
            ("main_build_id", jstr(&build16)),
            ("romfs_mode", jstr("full")),
            ("romfs_size", rom_size.to_string()),
            ("title_id", jstr(&format!("{id:016X}"))),
        ],
        false,
    );
    let app_json = obj(
        &[
            ("format", "1".into()),
            ("title_id", jstr(&format!("{id:016X}"))),
            ("name", jstr(&app_name)),
            (
                "display_version",
                jstr(fw_version.as_deref().unwrap_or("firmware")),
            ),
            (
                "builder",
                jstr(&format!(
                    "napple-build {} --firmware-program",
                    env!("CARGO_PKG_VERSION")
                )),
            ),
            ("target", jstr("macos")),
            ("versions", arr(&[ver])),
            ("dlc", "[]".into()),
            (
                "defaults",
                obj(&[("version", jstr(&build16)), ("dlc", "[]".into())], false),
            ),
            (
                "system",
                obj(
                    &[
                        (
                            "fonts",
                            if fonts.is_empty() {
                                "null".into()
                            } else {
                                jstr("system/fonts")
                            },
                        ),
                        (
                            "firmware_version",
                            fw_version
                                .as_deref()
                                .map(jstr)
                                .unwrap_or_else(|| "null".into()),
                        ),
                    ],
                    false,
                ),
            ),
            ("notes", "[]".into()),
        ],
        true,
    ) + "\n";
    std::fs::write(root.join("napple-app.json"), app_json)
        .map_err(|e| BuildError::io("write napple-app.json", e))?;
    std::fs::write(root.join("default"), format!("{build16}\n"))
        .map_err(|e| BuildError::io("write default", e))?;
    println!(
        "note: firmware program {id:016x} ({app_name}): {} modules rewritten, romfs {rom_size} bytes, {} system archives, {} fonts",
        mods.len(),
        n_sys,
        fonts.len()
    );
    let _ = Path::new("");
    Ok(root)
}
