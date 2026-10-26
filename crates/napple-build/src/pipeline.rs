//! `napple build`: base + updates + DLC -> locally signed `<Name>.app` (no Python, everything in process).
use crate::bktr::{self, PatchedRomfs, RawNca};
use crate::bundle::{self, LAUNCHER_NAME, RUNTIME_NAME};
use crate::container::{Container, Containers};
use crate::control::{self, Control};
use crate::error::{BuildError, Result};
use crate::firmware::Firmware;
use crate::json::{arr, hex, jstr, obj};
use crate::stream::{cached_stream, Streamed};
use crate::sysdata;
use crate::tables::{build_tables, GateRow, Layout, TableInput};
use crate::util::{check_cancel, clean_stale_staging, clone_or_copy, free_space, gb, progress, sanitize_name, TempDir};
use napple_core::application::ingest::Ingested;
use napple_core::application::package::{scan, version_label, BuildPlan, Choice, DlcChoice, Pack};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::Src;
use napple_core::infrastructure::nca::{self, Enc, FsType, Nca};
use napple_core::infrastructure::nso::Nso;
use napple_core::infrastructure::romfs::RomFs;
use napple_rewrite::check;
use napple_rewrite::image::ModuleImage;
use napple_rewrite::types::RewriteOptions;
use rayon::prelude::*;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UpdateRomfs {
    /// One full patched romfs.bin per update (boots with the current runtime; costs a full RomFS of disk per update).
    Materialize,
    /// BKTR delta layer (`romfs.delta/`): patch storage + relocation table over the base romfs.bin.
    Delta,
    /// Development (`--data-only` only): every version gets a `romfs.map` (extents of the game files + AES-CTR
    /// parameters) that the runtime decrypts on read; nothing is copied. The map holds keys: never ship it.
    InPlace,
}

#[derive(Clone, Debug)]
pub struct Opts {
    pub files: Vec<PathBuf>,
    pub keys: PathBuf,
    pub title_keys: Option<PathBuf>,
    pub firmware: Option<PathBuf>,
    pub updates: Choice,
    pub dlc: DlcChoice,
    pub target: String,
    pub out: PathBuf,
    pub name: Option<String>,
    pub runtime: Option<PathBuf>,
    pub launcher: Option<PathBuf>,
    pub make_runtime: bool,
    pub update_romfs: UpdateRomfs,
    pub keep_cache: bool,
    pub no_sign: bool,
    /// Write only the runtime data directories (`<out>/<titleid>/<buildid16>/...`, same layout as an app's Resources/game)
    /// instead of an .app: what `tools/prepare_game.sh` used to produce, for runtime development.
    pub data_only: bool,
    /// iOS targets: signing inputs, IPA export, device install.
    pub signing: crate::ios::Signing,
    /// iOS: launch the app after `--install`.
    pub launch: bool,
}

// Overall progress budget (percent).
const P_INGEST: f64 = 2.0;
const P_PLAN: f64 = 7.0;
const P_REWRITE: (f64, f64) = (8.0, 24.0);
const P_ROMFS: (f64, f64) = (24.0, 88.0);
const P_FONTS: f64 = 90.0;
const P_BUNDLE: f64 = 94.0;
const P_SIGN: f64 = 99.0;

struct ModOut {
    name: String,
    file_size: usize,
    sha256: String,
    build_id: [u8; 32],
    symbols: usize,
    relocs: usize,
    gates: usize,
}

enum RomfsPlan {
    /// Stream this data level to romfs.bin.
    Full { src: Src, key: String },
    /// BKTR delta layer.
    Delta { patched: PatchedRomfs, base_shift: u64, key: String },
    /// The update has no RomFS of its own: reuse the base image.
    SameAsBase,
}

struct VersionPlan {
    kind: &'static str,
    pack: usize,
    version: u32,
    label: String,
    exefs: Vec<(String, Vec<u8>)>,
    romfs: RomfsPlan,
    dir: String,
    main_build16: String,
}

struct RomfsOut {
    mode: &'static str,
    size: u64,
    sha256: String,
}

struct DlcPlan {
    pack: usize,
    title_id: u64,
    version: u32,
    src: Src,
    key: String,
}

fn is_module(n: &str) -> bool {
    matches!(n, "main" | "sdk") || n.strip_prefix("subsdk").is_some_and(|d| !d.is_empty() && d.bytes().all(|b| b.is_ascii_digit()))
}

/// main, subsdk0..N, sdk (symbols resolve in that order).
fn load_order(names: &[String]) -> Vec<String> {
    let mut sub: Vec<&String> = names.iter().filter(|n| n.starts_with("subsdk")).collect();
    sub.sort_by_key(|n| n[6..].parse::<u32>().unwrap_or(u32::MAX));
    let mut v = Vec::new();
    if names.iter().any(|n| n == "main") {
        v.push("main".to_string());
    }
    v.extend(sub.into_iter().cloned());
    if names.iter().any(|n| n == "sdk") {
        v.push("sdk".to_string());
    }
    v
}

/// Rewrites every module of one version and writes `<dir>/modules/<name>.{patched.bin,tables}`.
fn rewrite_version(exefs: &[(String, Vec<u8>)], dir: &Path, label: &str, lo: f64, hi: f64) -> Result<Vec<ModOut>> {
    let mods: Vec<&(String, Vec<u8>)> = exefs.iter().filter(|(n, _)| is_module(n)).collect();
    if !mods.iter().any(|(n, _)| n == "main") {
        return Err(BuildError::new("container", format!("{label}: ExeFS has no main module")));
    }
    let mdir = dir.join("modules");
    std::fs::create_dir_all(&mdir).map_err(|e| BuildError::io("create modules dir", e))?;
    let done = std::sync::atomic::AtomicUsize::new(0);
    let total = mods.len();
    let results: Vec<Result<ModOut>> = mods
        .par_iter()
        .map(|(name, bytes)| {
            check_cancel()?;
            let nso = Nso::parse(bytes).map_err(|e| BuildError::new("container", format!("{label} {name}: {e}")))?;
            if crate::tables32::is_elf32(&nso) {      // AArch32 module (MK8D): no AArch64 rewrite; plain image + NTB2 tables
                let (image, tables, st) = crate::tables32::build_module32(name, &nso, bytes)?;
                std::fs::write(mdir.join(format!("{name}.patched.bin")), &image).map_err(|e| BuildError::io("write module", e))?;
                std::fs::write(mdir.join(format!("{name}.tables")), &tables).map_err(|e| BuildError::io("write tables", e))?;
                use sha2::{Digest, Sha256};
                let n = done.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
                progress("rewrite", lo + (hi - lo) * n as f64 / total as f64, &format!("{label}: {name} (AArch32, {} relocations)", st.relocs));
                return Ok(ModOut { name: name.clone(), file_size: image.len(), sha256: hex(&Sha256::digest(&image)), build_id: st.build_id, symbols: st.symbols, relocs: st.relocs, gates: 0 });
            }
            let out = napple_rewrite::rewrite_module(&nso, &nso.build_id_hex()[..32], &RewriteOptions::default()).map_err(|e| BuildError::new("rewrite", format!("{label} {name}: {e}")))?;
            let orig = ModuleImage::from_nso(&nso).map_err(|e| BuildError::new("rewrite", format!("{label} {name}: {e}")))?;
            let chk = check::verify(&orig, &out);
            if !out.unhandled.is_empty() || !chk.violations.is_empty() {
                let first = out.unhandled.first().map(|u| format!("{:#x}: {}", u.addr, u.reason)).or_else(|| chk.violations.first().map(|v| v.to_string())).unwrap_or_default();
                return Err(BuildError::new("rewrite", format!("{label} {name}: {} unhandled site(s), {} check violation(s) (first: {first}); the module cannot run", out.unhandled.len(), chk.violations.len())));
            }
            let l = out.layout;
            let gates: Vec<GateRow> = out.gates.iter().map(GateRow::from).collect();
            let (tables, st) = build_tables(&TableInput {
                name,
                image: &out.image,
                layout: Layout { text_end: l.text_end as u64, ro_off: l.ro_off as u64, data_off: l.data_off as u64, patch_off: l.patch_off as u64, key_page_off: l.key_page_off as u64 },
                gates,
                nso: Some(bytes),
                nro: None,
            })?;
            std::fs::write(mdir.join(format!("{name}.patched.bin")), &out.image).map_err(|e| BuildError::io("write module", e))?;
            std::fs::write(mdir.join(format!("{name}.tables")), &tables).map_err(|e| BuildError::io("write tables", e))?;
            use sha2::{Digest, Sha256};
            let n = done.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
            progress("rewrite", lo + (hi - lo) * n as f64 / total as f64, &format!("{label}: {name} ({} gates, {} relocations)", st.gates, st.relocs));
            Ok(ModOut { name: name.clone(), file_size: out.image.len(), sha256: hex(&Sha256::digest(&out.image)), build_id: st.build_id, symbols: st.symbols, relocs: st.relocs, gates: st.gates })
        })
        .collect();
    let mut v = Vec::new();
    for r in results {
        v.push(r?);
    }
    let order = load_order(&v.iter().map(|m| m.name.clone()).collect::<Vec<_>>());
    v.sort_by_key(|m| order.iter().position(|n| *n == m.name).unwrap_or(usize::MAX));
    crate::tables32::recompile_if_requested(&mdir)?;   // AArch32 titles, only with NAPPLE_A32_RECOMPILE=1
    Ok(v)
}

fn ivfc_data_off(hash_data: &[u8]) -> Option<u64> {
    (hash_data.get(..4)? == b"IVFC").then(|| u64::from_le_bytes(hash_data[0x10 + 5 * 0x18..0x10 + 5 * 0x18 + 8].try_into().unwrap()))
}

/// Opens the decrypted RomFS image (IVFC data level) of the Program NCA of `pack` when it is a plain (non-BKTR) one.
fn base_romfs(cont: &Container, pack: &Pack, keys: &KeySet) -> Result<Option<(Src, Src, u64)>> {
    let Some(rec) = pack.meta.contents.iter().find(|c| c.ctype == 1) else { return Ok(None) };
    let src = cont.ncas.get(&rec.nca_name()).ok_or_else(|| BuildError::new("container", "Program NCA missing from the pack"))?;
    let n = Nca::open(src.clone(), keys, &cont.tks)?;
    for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs && s.enc != Enc::Bktr) {
        let section = n.section(sec.index)?;
        let data = nca::romfs_data(&section, sec)?;
        return Ok(Some((data, section, ivfc_data_off(&sec.hash_data).unwrap_or(0))));
    }
    Ok(None)
}

fn romfs_is_sane(data: &Src) -> bool {
    match RomFs::parse(data.clone()) {
        Ok(fs) => !fs.files.is_empty() && fs.files.iter().all(|f| f.offset.checked_add(f.size).is_some_and(|e| e <= data.size())),
        Err(_) => false,
    }
}

/// BKTR RomFS of an update over the base (both base interpretations are tried, see bktr.rs).
fn update_romfs(cont: &Container, pack: &Pack, keys: &KeySet, base: &(Src, Src, u64)) -> Result<Option<(PatchedRomfs, u64)>> {
    let Some(rec) = pack.meta.contents.iter().find(|c| c.ctype == 1) else { return Ok(None) };
    let src = cont.ncas.get(&rec.nca_name()).ok_or_else(|| BuildError::new("container", "Program NCA missing from the update"))?;
    let raw = RawNca::open(src.clone(), keys, &cont.tks)?;
    let Some(sec) = raw.sections.iter().find(|s| s.fs_type == 0 && s.enc_type == 4) else { return Ok(None) };
    // A: relocation sources are offsets in the base RomFS section; B: offsets in the base data level.
    for (candidate, shift) in [(base.1.clone(), base.2), (base.0.clone(), 0)] {
        if let Ok(p) = bktr::open_patched(&raw, sec, candidate) {
            if romfs_is_sane(&p.data) {
                return Ok(Some((p, shift)));
            }
        }
    }
    Err(BuildError::new("bktr", format!("update {} RomFS: the BKTR patch did not produce a valid RomFS over the base (unvalidated against retail; rebuild with --updates none to skip it)", version_label(pack.meta.version))))
}

fn dlc_romfs(cont: &Container, pack: &Pack, keys: &KeySet) -> Result<Option<(Src, String)>> {
    for rec in pack.meta.contents.iter().filter(|c| c.ctype == 2) {
        let Some(src) = cont.ncas.get(&rec.nca_name()) else { continue };
        let n = Nca::open(src.clone(), keys, &cont.tks)?;
        for sec in n.sections.iter().filter(|s| s.fs_type == FsType::RomFs && s.enc != Enc::Bktr) {
            let data = nca::romfs_data(&n.section(sec.index)?, sec)?;
            return Ok(Some((data, rec.nca_name().trim_end_matches(".nca").to_string())));
        }
    }
    Ok(None)
}

fn write_delta(p: &PatchedRomfs, base_shift: u64, dir: &Path, base_dir: &str, base_size: u64) -> Result<RomfsOut> {
    use std::io::Write;
    let d = dir.join("romfs.delta");
    std::fs::create_dir_all(&d).map_err(|e| BuildError::io("create romfs.delta", e))?;
    let entries = p.delta_entries(base_shift)?;
    let mut f = std::io::BufWriter::new(std::fs::File::create(d.join("romfs.reloc")).map_err(|e| BuildError::io("write reloc", e))?);
    f.write_all(b"RLC1").and_then(|_| f.write_all(&(entries.len() as u32).to_le_bytes())).and_then(|_| f.write_all(&p.data_size.to_le_bytes())).map_err(|e| BuildError::io("write reloc", e))?;
    for e in &entries {
        let w = (|| {
            f.write_all(&e.data_offset.to_le_bytes())?;
            f.write_all(&e.source_offset.to_le_bytes())?;
            f.write_all(&e.source.to_le_bytes())?;
            f.write_all(&0u32.to_le_bytes())
        })();
        w.map_err(|e| BuildError::io("write reloc", e))?;
    }
    f.flush().map_err(|e| BuildError::io("write reloc", e))?;
    drop(f);
    let st = crate::stream::stream_to_file(&p.patch, &d.join("patch.bin"), |_, _| {})?;
    let desc = obj(
        &[
            ("format", "1".into()),
            ("data_size", p.data_size.to_string()),
            ("base_dir", jstr(base_dir)),
            ("base_file", jstr("romfs.bin")),
            ("base_size", base_size.to_string()),
            ("patch_file", jstr("patch.bin")),
            ("patch_size", p.patch_size.to_string()),
            ("patch_sha256", jstr(&st.sha256)),
            ("reloc_file", jstr("romfs.reloc")),
            ("entries", entries.len().to_string()),
            ("note", jstr("RLC1: 'RLC1', u32 count, u64 data_size; entries {u64 data_offset, u64 source_offset, u32 source (0 base romfs.bin, 1 patch.bin), u32 0}; entry i covers [data_offset_i, data_offset_i+1)")),
        ],
        true,
    ) + "\n";
    std::fs::write(d.join("romfs.json"), desc).map_err(|e| BuildError::io("write romfs.json", e))?;
    Ok(RomfsOut { mode: "delta", size: p.data_size, sha256: st.sha256 })
}

/// Stable 32-bit tag of (input files, output dir): concurrent builds of different inputs never share a cache,
/// and a retry of the same command finds its cache again.
/// Name of the RomFS cache directory under `<staging root>/.napple-cache.nosync` for one set of inputs and output dir.
pub fn cache_dir_name(app_id: u64, files: &[PathBuf], out: &Path) -> String {
    format!("{app_id:016X}-{:08x}", cache_tag(files, out))
}

fn cache_tag(files: &[PathBuf], out: &Path) -> u32 {
    let mut h: u64 = 0xcbf29ce484222325;
    for f in files.iter().map(|f| std::path::absolute(f).unwrap_or_else(|_| f.clone())).chain(std::iter::once(out.to_path_buf())) {
        for b in f.as_os_str().as_encoded_bytes().iter().chain(&[0u8]) {
            h = (h ^ *b as u64).wrapping_mul(0x100000001b3);
        }
    }
    (h ^ (h >> 32)) as u32
}

fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from)? {
        let e = e?;
        let (src, dst) = (e.path(), to.join(e.file_name()));
        if e.file_type()?.is_dir() {
            copy_tree(&src, &dst)?;
        } else {
            std::fs::copy(&src, &dst)?;
        }
    }
    let _ = std::fs::remove_dir_all(from);
    Ok(())
}

/// Fails clearly before any multi-GB write when the volume cannot hold the build.
fn ensure_space(need: u64, free: u64, out: &Path) -> Result<()> {
    if free < need {
        return Err(BuildError::new("disk-space", format!("not enough free space on the output volume: need about {}, have {} free (output {})", gb(need), gb(free), out.display())));
    }
    Ok(())
}

fn resolve_exec(explicit: &Option<PathBuf>, name: &str, rel: &str, what: &str) -> Result<PathBuf> {
    bundle::find_tool(explicit.as_deref(), name, rel).ok_or_else(|| {
        BuildError::new("runtime", format!("{what} not found: pass --{} <path>, or run `make -C {}` (the `napple build --make-runtime` flag does it for a checkout)", if name == RUNTIME_NAME { "runtime" } else { "launcher" }, rel.split('/').next().unwrap_or("runtime")))
    })
}

pub fn build(o: &Opts) -> Result<PathBuf> {
    crate::util::reset_progress();
    crate::util::install_cancel_handlers();
    let target = crate::ios::Target::parse(&o.target).ok_or_else(|| BuildError::new("target", format!("unknown target {:?} (macos, ios, iphone, ipad, ios-sim)", o.target)))?;
    let ios = target.is_ios();
    if ios && !target.is_sim() && !o.data_only && !o.no_sign && (o.signing.identity.is_none() || o.signing.profile.is_none()) {
        // fail before the multi-GB work, not after it
        return Err(BuildError::new("sign", "iOS signing needs --signing-identity and --provisioning-profile (see `napple build --list-signing`), or pass --no-sign to build an unsigned app/IPA for an external signer"));
    }
    progress("preflight", 0.0, "checking keys and tools");
    let keys = crate::util::load_keys(&o.keys)?;
    if !keys.has("header_key") && !keys.has("header_key_source") {
        return Err(BuildError::new("keys", "the key file has no header_key (is it a prod.keys?)"));
    }
    if let Some(tk) = &o.title_keys {
        std::fs::metadata(tk).map_err(|e| BuildError::new("keys", format!("--title-keys {}: {e}", tk.display())))?;
        println!("note: --title-keys is accepted but not applied yet: titlekey crypto uses the tickets inside the NSP/XCI (napple-core limitation)");
    }
    let fw = match &o.firmware {
        Some(p) => Some(Firmware::open(p)?),
        None => None,
    };
    if o.make_runtime {
        let root = bundle::repo_root().ok_or_else(|| BuildError::new("runtime", "--make-runtime needs a repository checkout (runtime/Makefile)"))?;
        progress("preflight", 0.5, "make -C runtime");
        bundle::make_in(&root, "runtime")?;
        if root.join("packaging/Makefile").is_file() {
            bundle::make_in(&root, "packaging")?;
        }
    }
    let (runtime, launcher) = if o.data_only {
        (PathBuf::new(), PathBuf::new())
    } else if ios {
        // iOS has no launcher (no exec): the runtime is the app executable.
        (resolve_exec(&o.runtime, RUNTIME_NAME, "runtime/build-ios/napple-runtime", "the iOS runtime binary (make -C runtime -f Makefile.ios on a Mac)")?, PathBuf::new())
    } else {
        (resolve_exec(&o.runtime, RUNTIME_NAME, "runtime/build/napple-runtime", "the runtime binary")?, resolve_exec(&o.launcher, LAUNCHER_NAME, "packaging/build/napple-launcher", "the launcher binary")?)
    };
    std::fs::create_dir_all(&o.out).map_err(|e| BuildError::io(&format!("create {}", o.out.display()), e))?;
    let out = std::path::absolute(&o.out).map_err(|e| BuildError::io("resolve output dir", e))?;
    let root = crate::util::stage_root(&out);
    let cleaned = clean_stale_staging(&out) + if root != out { clean_stale_staging(&root) } else { 0 };
    if cleaned > 0 {
        println!("cleaned {cleaned} stale staging dir(s)");
    }
    check_cancel()?;

    // ---- ingest ------------------------------------------------------------------------------------------------
    progress("ingest", 1.0, "reading containers");
    let mut packs = Vec::new();
    for f in &o.files {
        packs.extend(scan(f, &keys).map_err(|e| BuildError::new(if matches!(e, napple_core::domain::error::Error::Io(_)) { "input" } else { "container" }, format!("{}: {e}", f.display())))?);
    }
    let plan = BuildPlan::from_packs(packs);
    for w in &plan.warnings {
        println!("warning: {w}");
    }
    let base_i = plan.base.ok_or_else(|| BuildError::new("plan", "no base game among the supplied files"))?;
    let upd: Vec<usize> = plan.pick_updates(&o.updates);
    let dlc: Vec<usize> = plan.pick_dlcs(&o.dlc);
    let app_id = plan.packs[base_i].meta.title_id;
    let mut conts = Containers::default();
    progress("ingest", P_INGEST, &format!("{:016X}: {} update(s), {} DLC selected", app_id, upd.len(), dlc.len()));

    // name / icon / display version: newest version with a control NCA
    let mut control: Option<Control> = None;
    for &i in upd.iter().rev().chain(std::iter::once(&base_i)) {
        let c = conts.get(&plan.packs[i].path)?;
        if let Some(ctl) = control::read_control(&c, &plan.packs[i], &keys)? {
            if ctl.nacp.is_some() || ctl.icon.is_some() {
                control = Some(ctl);
                break;
            }
        }
    }
    let app_name = o
        .name
        .as_deref()
        .map(sanitize_name)
        .filter(|n| !n.is_empty())
        .or_else(|| control.as_ref().and_then(|c| c.nacp.as_ref()).and_then(control::app_name).map(|n| sanitize_name(&n)).filter(|n| !n.is_empty()))
        .unwrap_or_else(|| format!("Napple Game {app_id:016X}"));
    let display_version = control.as_ref().and_then(|c| c.nacp.as_ref()).map(|n| n.display_version.clone()).unwrap_or_default();
    check_cancel()?;

    // ---- plan versions and RomFS jobs ----------------------------------------------------------------------------
    let mut notes: Vec<String> = Vec::new();
    let mut versions: Vec<VersionPlan> = Vec::new();
    let base_cont = conts.get(&plan.packs[base_i].path)?;
    let base_rom = base_romfs(&base_cont, &plan.packs[base_i], &keys)?;
    for (n, &i) in std::iter::once(&base_i).chain(upd.iter()).enumerate() {
        let p = &plan.packs[i];
        let prog: Ingested = p.program(&keys)?;
        let main = prog.exefs.iter().find(|(n, _)| n == "main").ok_or_else(|| BuildError::new("container", format!("{}: no main module in the ExeFS", version_label(p.meta.version))))?;
        let main_nso = Nso::parse(&main.1).map_err(|e| BuildError::new("container", format!("main: {e}")))?;
        let main_build16 = main_nso.build_id_hex()[..16].to_string();
        let romfs = if n == 0 {
            match &base_rom {
                Some((data, _, _)) => RomfsPlan::Full { src: data.clone(), key: format!("romfs-{}", p.meta.contents.iter().find(|c| c.ctype == 1).map(|c| c.nca_name().trim_end_matches(".nca").to_string()).unwrap_or_default()) },
                None => return Err(BuildError::new("container", "the base game has no readable RomFS")),
            }
        } else {
            let c = conts.get(&p.path)?;
            let nca_id = p.meta.contents.iter().find(|c| c.ctype == 1).map(|c| c.nca_name().trim_end_matches(".nca").to_string()).unwrap_or_default();
            match update_romfs(&c, p, &keys, base_rom.as_ref().ok_or_else(|| BuildError::new("container", "base RomFS needed to patch an update"))?)? {
                Some((patched, shift)) => match o.update_romfs {
                    UpdateRomfs::Materialize | UpdateRomfs::InPlace => RomfsPlan::Full { src: patched.data.clone(), key: format!("romfs-patched-{nca_id}") },
                    UpdateRomfs::Delta => RomfsPlan::Delta { patched, base_shift: shift, key: nca_id },
                },
                None => match base_romfs(&c, p, &keys)? {
                    // an update that carries a complete (non-BKTR) RomFS
                    Some((data, _, _)) => RomfsPlan::Full { src: data, key: format!("romfs-{nca_id}") },
                    None => {
                        notes.push(format!("update {} has no RomFS patch: it reuses the base RomFS", version_label(p.meta.version)));
                        RomfsPlan::SameAsBase
                    }
                },
            }
        };
        versions.push(VersionPlan {
            kind: if n == 0 { "base" } else { "update" },
            pack: i,
            version: p.meta.version,
            label: version_label(p.meta.version),
            exefs: prog.exefs,
            romfs,
            dir: main_build16.clone(),
            main_build16,
        });
    }
    {
        let mut seen = std::collections::HashSet::new();
        for v in &versions {
            if !seen.insert(v.dir.clone()) {
                return Err(BuildError::new("plan", format!("{} has the same main build id ({}) as another included version; build them separately", v.label, v.dir)));
            }
        }
    }
    let mut dlcs: Vec<DlcPlan> = Vec::new();
    for &i in &dlc {
        let p = &plan.packs[i];
        let c = conts.get(&p.path)?;
        match dlc_romfs(&c, p, &keys)? {
            Some((src, id)) => dlcs.push(DlcPlan { pack: i, title_id: p.meta.title_id, version: p.meta.version, src, key: format!("dlc-{id}") }),
            None => notes.push(format!("DLC {:016X} has no RomFS data NCA and was skipped", p.meta.title_id)),
        }
    }

    // sizes -> free-space check
    let in_place = matches!(o.update_romfs, UpdateRomfs::InPlace);
    if in_place && !o.data_only {
        return Err(BuildError::usage("--update-romfs in-place is for --data-only development builds (the map holds decryption keys)"));
    }
    let cache_dir = root.join(".napple-cache.nosync").join(cache_dir_name(app_id, &o.files, &out));
    let mut need: u64 = 256 << 20;
    let mut jobs_total: u64 = 0;
    let mut sized: Vec<(String, u64)> = Vec::new();
    for v in &versions {
        match &v.romfs {
            RomfsPlan::Full { .. } if in_place => {}
            RomfsPlan::Full { src, key } => sized.push((key.clone(), src.size())),
            RomfsPlan::Delta { patched, key, .. } => {
                need += patched.patch_size;
                jobs_total += patched.patch_size;
                let _ = key;
            }
            RomfsPlan::SameAsBase => {}
        }
        need += v.exefs.iter().map(|(_, d)| d.len() as u64 * 3).sum::<u64>();
    }
    for d in &dlcs {
        sized.push((d.key.clone(), d.src.size()));
    }
    for (key, size) in &sized {
        jobs_total += size;
        if crate::stream::cache_lookup(&cache_dir.join(format!("{key}.bin")), *size).is_none() {
            need += size;
        }
    }
    let free = free_space(&root)?.min(free_space(&out)?);
    ensure_space(need, free, &out)?;
    progress("plan", P_PLAN, &format!("{app_name}: {} version(s), {} DLC, about {} to write ({} free)", versions.len(), dlcs.len(), gb(need), gb(free)));
    check_cancel()?;

    // ---- staging bundle ------------------------------------------------------------------------------------------
    let stage = TempDir::create(root.join(crate::util::staging_name()))?;
    // iOS: the app is staged inside Payload/ so the same directory can be zipped into an .ipa without a copy.
    let app = if ios { stage.path.join("Payload").join(format!("{app_name}.app")) } else { stage.path.join(format!("{app_name}.app")) };
    let (macos, res) = if ios { (app.clone(), app.clone()) } else { (app.join("Contents/MacOS"), app.join("Contents/Resources")) };
    let game = if o.data_only { stage.path.join("game") } else { res.join("game") };
    for d in [&macos, &game] {
        std::fs::create_dir_all(d).map_err(|e| BuildError::io("create bundle dirs", e))?;
    }

    // rewrite + tables
    let mut mods_by_version: Vec<Vec<ModOut>> = Vec::new();
    let nv = versions.len() as f64;
    for (k, v) in versions.iter().enumerate() {
        let (lo, hi) = (P_REWRITE.0 + (P_REWRITE.1 - P_REWRITE.0) * k as f64 / nv, P_REWRITE.0 + (P_REWRITE.1 - P_REWRITE.0) * (k + 1) as f64 / nv);
        let dir = game.join(&v.dir);
        let mods = rewrite_version(&v.exefs, &dir, &v.label, lo, hi)?;
        match v.exefs.iter().find(|(n, _)| n == "main.npdm") {
            Some((_, d)) => std::fs::write(dir.join("main.npdm"), d).map_err(|e| BuildError::io("write main.npdm", e))?,
            None => notes.push(format!("{}: no main.npdm in the ExeFS (the runtime skips service access checks)", v.label)),
        }
        mods_by_version.push(mods);
    }

    // NROs the RomFS ships (loaded at run time through ldr:ro): rewritten per version, since an update may replace them
    let mut nros_by_version: Vec<Vec<crate::nro::NroOut>> = Vec::new();
    for v in &versions {
        let src = match &v.romfs {
            RomfsPlan::Full { src, .. } => Some(src.clone()),
            RomfsPlan::Delta { patched, .. } => Some(patched.data.clone()),
            RomfsPlan::SameAsBase => base_rom.as_ref().map(|(d, _, _)| d.clone()),
        };
        let found = match src.map(RomFs::parse) {
            Some(Ok(rom)) => {
                let (outs, bad) = crate::nro::rewrite_romfs_nros(&rom, &game.join(&v.dir), &v.label)?;
                notes.extend(bad);
                outs
            }
            Some(Err(e)) => {
                notes.push(format!("{}: RomFS tables unreadable, NROs not rewritten: {e}", v.label));
                Vec::new()
            }
            None => Vec::new(),
        };
        if !found.is_empty() {
            progress("rewrite", P_REWRITE.1, &format!("{}: {} NRO module(s) rewritten for ldr:ro", v.label, found.len()));
        }
        nros_by_version.push(found);
    }

    // RomFS streaming
    let mut romfs_out: Vec<RomfsOut> = Vec::new();
    let mut dlc_out: Vec<(Streamed, String)> = Vec::new();
    let mut done_bytes: u64 = 0;
    let span = P_ROMFS.1 - P_ROMFS.0;
    let share = |bytes: u64| span * bytes as f64 / jobs_total.max(1) as f64;
    let (mut base_sha, mut base_size) = (String::new(), 0u64);
    let mut cloned_all = true;
    std::fs::create_dir_all(&cache_dir).map_err(|e| BuildError::io("create cache dir", e))?;
    for v in &versions {
        let dir = game.join(&v.dir);
        let label = format!("{} RomFS", v.label);
        let ro = match &v.romfs {
            RomfsPlan::Full { src, .. } if in_place => {
                progress("romfs", P_ROMFS.0, &format!("{label}: in-place map"));
                std::fs::create_dir_all(&dir).map_err(|e| BuildError::io("create dir", e))?;
                let n = crate::romfs_map::write_map(src, &dir.join("romfs.map"))?;
                progress("romfs", P_ROMFS.0, &format!("{label}: {n} extents, {} bytes", src.size()));
                RomfsOut { mode: if v.kind == "base" { "map" } else { "patched-map" }, size: src.size(), sha256: String::new() }
            }
            RomfsPlan::SameAsBase if in_place => {
                std::fs::copy(game.join(&versions[0].dir).join("romfs.map"), dir.join("romfs.map")).map_err(|e| BuildError::io("copy romfs.map", e))?;
                RomfsOut { mode: "base", size: base_size, sha256: base_sha.clone() }
            }
            RomfsPlan::Full { src, key } => {
                let lo = P_ROMFS.0 + share(done_bytes);
                let hi = P_ROMFS.0 + share(done_bytes + src.size());
                let (s, _) = cached_stream(src, &cache_dir.join(format!("{key}.bin")), "romfs", &label, lo, hi)?;
                done_bytes += src.size();
                cloned_all &= clone_or_copy(&cache_dir.join(format!("{key}.bin")), &dir.join("romfs.bin"))?;
                RomfsOut { mode: if v.kind == "base" { "full" } else { "patched" }, size: s.size, sha256: s.sha256 }
            }
            RomfsPlan::Delta { patched, base_shift, .. } => {
                progress("romfs", P_ROMFS.0 + share(done_bytes), &format!("{label}: delta layer"));
                let r = write_delta(patched, *base_shift, &dir, &versions[0].dir, base_size)?;
                done_bytes += patched.patch_size;
                r
            }
            RomfsPlan::SameAsBase => {
                let key_path = game.join(&versions[0].dir).join("romfs.bin");
                cloned_all &= clone_or_copy(&key_path, &dir.join("romfs.bin"))?;
                RomfsOut { mode: "base", size: base_size, sha256: base_sha.clone() }
            }
        };
        if v.kind == "base" {
            base_sha = ro.sha256.clone();
            base_size = ro.size;
        }
        romfs_out.push(ro);
    }
    for d in &dlcs {
        let lo = P_ROMFS.0 + share(done_bytes);
        let hi = P_ROMFS.0 + share(done_bytes + d.src.size());
        let (s, _) = cached_stream(&d.src, &cache_dir.join(format!("{}.bin", d.key)), "dlc", &format!("DLC {:016X}", d.title_id), lo, hi)?;
        done_bytes += d.src.size();
        let rel = format!("dlc/{:016X}/v{}", d.title_id, d.version);
        cloned_all &= clone_or_copy(&cache_dir.join(format!("{}.bin", d.key)), &game.join(&rel).join("romfs.bin"))?;
        dlc_out.push((s, rel));
    }
    if !cloned_all {
        notes.push("the output volume does not support APFS clones: RomFS files were copied (extra disk used until the cache is removed)".into());
    }
    check_cancel()?;

    // fonts / system data
    progress("fonts", P_FONTS - 1.0, "system fonts");
    let mut fonts_present = false;
    let mut fw_version: Option<String> = None;
    match &fw {
        Some(fwi) => {
            let fonts = sysdata::find_fonts(fwi, &keys)?;
            fw_version = sysdata::firmware_version(fwi, &keys)?;
            let sys = sysdata::write_system_archives(fwi, &keys, &game.join("sysdata"))?;
            if !sys.contains(&0x0100_0000_0000_0823) {
                notes.push("compat: system data archive 0100000000000823 (BuiltInSystem) not found in the firmware; games that read it will abort".into());
            }
            if fonts.is_empty() {
                notes.push("compat: no shared fonts found in the firmware; games that render text through pl:u will get empty fonts".into());
            } else {
                sysdata::write_fonts(&fonts, fw_version.as_deref(), &game.join("system/fonts"))?;
                fonts_present = true;
                if sysdata::official_count(&fonts) < 6 {
                    notes.push(format!("compat: only {} of the 6 shared font types were found in the firmware", sysdata::official_count(&fonts)));
                }
            }
        }
        None => notes.push("compat: built without --firmware: no shared system fonts (pl:u serves empty fonts; text in games that use them will not render). Rebuild with --firmware <Firmware.zip> or run `napple build --dump-fonts`".into()),
    }
    progress("fonts", P_FONTS, if fonts_present { "fonts written" } else { "no fonts" });

    // icon
    progress("bundle", P_FONTS + 0.5, "icon");
    let mut has_icon = false;
    let mut ios_icons: Vec<String> = vec![];
    if o.data_only {
    } else if let (true, Some(jpeg)) = (ios, control.as_ref().and_then(|c| c.icon.as_ref())) {
        ios_icons = crate::ios::make_icons(jpeg, &stage.path.join("icon-work"), &app)?;
        if ios_icons.is_empty() {
            notes.push("the icon could not be converted (sips unavailable): the app has the default icon".into());
        }
    } else if ios {
        notes.push("no icon found in the Control NCA: the app has the default icon".into());
    } else if let Some(jpeg) = control.as_ref().and_then(|c| c.icon.as_ref()) {
        has_icon = bundle::make_icns(jpeg, &stage.path.join("icon-work"), &res.join("AppIcon.icns"))?;
        if !has_icon {
            notes.push("the icon could not be converted (sips/iconutil unavailable): the app has the default icon".into());
        }
    } else {
        notes.push("no icon found in the Control NCA: the app has the default icon".into());
    }
    let _ = std::fs::remove_dir_all(stage.path.join("icon-work"));

    // manifests
    let dlc_json: Vec<String> = dlcs
        .iter()
        .zip(&dlc_out)
        .map(|(d, (s, rel))| {
            obj(
                &[
                    ("title_id", jstr(&format!("{:016X}", d.title_id))),
                    ("version", d.version.to_string()),
                    ("label", jstr(&version_label(d.version))),
                    ("pack_size", plan.packs[d.pack].size().to_string()),
                    ("romfs_size", s.size.to_string()),
                    ("sha256", jstr(&s.sha256)),
                    ("dir", jstr(rel)),
                    ("romfs", jstr("romfs.bin")),
                    ("default_enabled", "true".into()),
                ],
                false,
            )
        })
        .collect();
    let mut ver_json = Vec::new();
    for (((v, mods), ro), nros) in versions.iter().zip(&mods_by_version).zip(&romfs_out).zip(&nros_by_version) {
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
            &[("mode", jstr(ro.mode)), ("file", jstr(if ro.mode == "delta" { "romfs.delta/romfs.json" } else { "romfs.bin" })), ("size", ro.size.to_string()), ("sha256", jstr(&ro.sha256))],
            false,
        );
        let manifest = obj(
            &[
                ("format", "1".into()),
                ("title_id", jstr(&format!("{app_id:016X}"))),
                ("build_id", jstr(&v.main_build16)),
                ("kind", jstr(v.kind)),
                ("version", v.version.to_string()),
                ("label", jstr(&v.label)),
                ("load_order", arr(&order.iter().map(|n| jstr(n)).collect::<Vec<_>>())),
                ("modules", arr(&mod_json)),
                ("nro", arr(&nros.iter().map(|n| n.json()).collect::<Vec<_>>())),
                ("romfs", rom_desc),
                ("system_dir", jstr("../system")),
                ("dlc", arr(&dlc_json.iter().map(|d| d.replace("\"dir\":\"dlc/", "\"dir\":\"../dlc/")).collect::<Vec<_>>())),
            ],
            true,
        ) + "\n";
        std::fs::write(game.join(&v.dir).join("manifest.json"), manifest).map_err(|e| BuildError::io("write manifest.json", e))?;
        ver_json.push(obj(
            &[
                ("kind", jstr(v.kind)),
                ("version", v.version.to_string()),
                ("label", jstr(&v.label)),
                ("dir", jstr(&v.dir)),
                ("main_build_id", jstr(&v.main_build16)),
                ("romfs_mode", jstr(ro.mode)),
                ("romfs_size", ro.size.to_string()),
                ("title_id", jstr(&format!("{:016X}", plan.packs[v.pack].meta.title_id))),
            ],
            false,
        ));
    }
    let default_dir = versions.last().map(|v| v.dir.clone()).unwrap();
    let app_json = obj(
        &[
            ("format", "1".into()),
            ("title_id", jstr(&format!("{app_id:016X}"))),
            ("name", jstr(&app_name)),
            ("display_version", jstr(&display_version)),
            ("builder", jstr(&format!("napple-build {}", env!("CARGO_PKG_VERSION")))),
            ("target", jstr(target.name())),
            ("versions", arr(&ver_json)),
            ("dlc", arr(&dlc_json)),
            ("defaults", obj(&[("version", jstr(&default_dir)), ("dlc", arr(&dlcs.iter().map(|d| jstr(&format!("{:016X}", d.title_id))).collect::<Vec<_>>()))], false)),
            ("system", obj(&[("fonts", if fonts_present { jstr("system/fonts") } else { "null".into() }), ("firmware_version", fw_version.as_deref().map(jstr).unwrap_or_else(|| "null".into()))], false)),
            ("notes", arr(&notes.iter().map(|n| jstr(n)).collect::<Vec<_>>())),
        ],
        true,
    ) + "\n";
    std::fs::write(game.join("napple-app.json"), app_json).map_err(|e| BuildError::io("write napple-app.json", e))?;
    std::fs::write(game.join("default"), format!("{default_dir}\n")).map_err(|e| BuildError::io("write default", e))?;

    if o.data_only {
        let target = out.join(format!("{app_id:016X}"));
        std::fs::create_dir_all(&target).map_err(|e| BuildError::io("create data dir", e))?;
        for e in std::fs::read_dir(&game).map_err(|e| BuildError::io("read staging", e))?.flatten() {
            let dst = target.join(e.file_name());
            if dst.is_dir() {
                let _ = std::fs::remove_dir_all(&dst);
            } else {
                let _ = std::fs::remove_file(&dst);
            }
            std::fs::rename(e.path(), &dst).map_err(|e| BuildError::io("publish data dir", e))?;
        }
        drop(stage);
        if !o.keep_cache {
            let _ = std::fs::remove_dir_all(&cache_dir);
            let _ = std::fs::remove_dir(root.join(".napple-cache.nosync"));
        }
        for n in &notes {
            println!("note: {n}");
        }
        progress("done", 100.0, "data directories ready");
        println!("RESULT {}", target.display());
        return Ok(target);
    }

    // ---- bundle files ---------------------------------------------------------------------------------------------
    progress("bundle", P_FONTS + 2.0, "Info.plist, executables");
    let bundle_id = crate::ios::bundle_id_for(app_id, o.signing.bundle_id.as_deref());
    if ios {
        std::fs::write(app.join("Info.plist"), crate::ios::info_plist(app_id, &bundle_id, &app_name, &display_version, target, &ios_icons)).map_err(|e| BuildError::io("write Info.plist", e))?;
        std::fs::write(app.join("PkgInfo"), b"APPL????").map_err(|e| BuildError::io("write PkgInfo", e))?;
        std::fs::copy(&runtime, app.join(RUNTIME_NAME)).map_err(|e| BuildError::io(&format!("copy {}", runtime.display()), e))?;
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(app.join(RUNTIME_NAME), std::fs::Permissions::from_mode(0o755)).map_err(|e| BuildError::io("chmod", e))?;
        // iOS cannot make anonymous memory executable: every rewritten module becomes a signable Mach-O dylib (macho.rs).
        let n = crate::ios::dylibize(&game, &app.join("Frameworks"), target.platform())?;
        notes.push(format!("{n} rewritten module(s) packed as Mach-O dylibs in Frameworks/ (iOS maps executable code only from signed files)"));
    } else {
        std::fs::write(app.join("Contents/Info.plist"), bundle::info_plist(app_id, &app_name, &display_version, has_icon)).map_err(|e| BuildError::io("write Info.plist", e))?;
        std::fs::write(app.join("Contents/PkgInfo"), b"APPL????").map_err(|e| BuildError::io("write PkgInfo", e))?;
        for (src, name) in [(&launcher, LAUNCHER_NAME), (&runtime, RUNTIME_NAME)] {
            std::fs::copy(src, macos.join(name)).map_err(|e| BuildError::io(&format!("copy {}", src.display()), e))?;
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(macos.join(name), std::fs::Permissions::from_mode(0o755)).map_err(|e| BuildError::io("chmod", e))?;
        }
    }
    progress("bundle", P_BUNDLE, "bundle assembled");
    check_cancel()?;

    // ---- sign ----------------------------------------------------------------------------------------------------------
    if o.no_sign {
        progress("sign", P_SIGN, "signing skipped (--no-sign)");
        if ios {
            notes.push("iOS app is unsigned: sign it (and its Frameworks/*.dylib) with your own tool before installing".into());
        }
    } else if target.is_sim() {
        progress("sign", P_BUNDLE + 1.0, "codesign (ad-hoc) for the iOS Simulator");
        notes.extend(crate::ios::sign_sim(&app)?);
        progress("sign", P_SIGN, "codesign --verify passed");
    } else if ios {
        progress("sign", P_BUNDLE + 1.0, "codesign with your identity and provisioning profile");
        let work = stage.path.join("sign-work");
        std::fs::create_dir_all(&work).map_err(|e| BuildError::io("create sign dir", e))?;
        notes.extend(crate::ios::sign(&app, &work, &o.signing, &bundle_id)?);
        let _ = std::fs::remove_dir_all(&work);
        progress("sign", P_SIGN, "codesign --verify passed");
    } else {
        progress("sign", P_BUNDLE + 1.0, "codesign (ad-hoc), hashing the bundle");
        bundle::clear_xattrs(&app);
        bundle::codesign(&app)?;
        progress("sign", P_SIGN, "codesign --verify passed");
    }
    check_cancel()?;

    if ios && o.signing.ipa {
        progress("sign", P_SIGN + 0.2, "writing the .ipa");
        crate::ios::export_ipa(&stage.path.join("Payload"), &out.join(format!("{app_name}.ipa")))?;
    }

    // ---- atomic publish --------------------------------------------------------------------------------------------
    let final_app = out.join(format!("{app_name}.app"));
    let old = out.join(format!(".napple-old-{}-{}", std::process::id(), stage.path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default()));
    let had_old = final_app.exists();
    if had_old {
        std::fs::rename(&final_app, &old).map_err(|e| BuildError::io("move the previous app aside", e))?;
    }
    let moved = std::fs::rename(&app, &final_app).or_else(|e| {
        // Cross-volume staging should not happen (stage_root keeps both on one volume), but never fail a finished build on it.
        if e.raw_os_error() == Some(libc::EXDEV) { copy_tree(&app, &final_app) } else { Err(e) }
    });
    if let Err(e) = moved {
        if had_old {
            let _ = std::fs::rename(&old, &final_app);
        }
        return Err(BuildError::io("publish the app", e));
    }
    if had_old {
        let _ = std::fs::remove_dir_all(&old);
    }
    drop(stage);
    if !ios && !o.no_sign && bundle::verify(&final_app).is_err() {
        // File Provider folders add Finder metadata to the bundle directory after the move: clear it and re-check.
        bundle::clear_xattrs(&final_app);
        if let Err(e) = bundle::verify(&final_app) {
            notes.push(format!("the signature verified before publishing but not at {} ({}); this folder adds Finder/iCloud metadata to the bundle. The app still launches; choose an output folder outside Desktop/Documents to keep `codesign --verify` clean", final_app.display(), e.msg.lines().last().unwrap_or("")));
        }
    }
    if !o.keep_cache {
        let _ = std::fs::remove_dir_all(&cache_dir);
        let _ = std::fs::remove_dir(root.join(".napple-cache.nosync"));
    }
    for n in &notes {
        println!("note: {n}");
    }
    if let Some(dev) = &o.signing.install {
        progress("install", 99.5, &format!("installing on {dev}"));
        crate::ios::install(dev, &final_app, &bundle_id, o.launch)?;
    }
    progress("done", 100.0, &format!("{app_name}.app ready"));
    println!("RESULT {}", final_app.display());
    Ok(final_app)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn module_names_and_order() {
        assert!(is_module("main") && is_module("sdk") && is_module("subsdk0") && is_module("subsdk12"));
        assert!(!is_module("rtld") && !is_module("subsdk") && !is_module("subsdkx") && !is_module("main.npdm"));
        let names: Vec<String> = ["sdk", "subsdk10", "main", "subsdk2", "subsdk0"].iter().map(|s| s.to_string()).collect();
        assert_eq!(load_order(&names), ["main", "subsdk0", "subsdk2", "subsdk10", "sdk"]);
    }

    #[test]
    fn free_space_check_is_clear() {
        assert!(ensure_space(5_000_000_000, 6_000_000_000, Path::new("/o")).is_ok());
        let e = ensure_space(7_000_000_000, 6_000_000_000, Path::new("/o")).unwrap_err();
        assert_eq!(e.code, "disk-space");
        assert!(e.msg.contains("7.00 GB") && e.msg.contains("6.00 GB") && e.msg.contains("/o"), "{}", e.msg);
    }

    #[test]
    fn cache_tag_is_stable_and_input_specific() {
        let a = cache_tag(&["/x/a.nsp".into()], Path::new("/out"));
        assert_eq!(a, cache_tag(&["/x/a.nsp".into()], Path::new("/out")));
        assert_ne!(a, cache_tag(&["/x/b.nsp".into()], Path::new("/out")));
        assert_ne!(a, cache_tag(&["/x/a.nsp".into()], Path::new("/out2")));
    }

    #[test]
    fn ivfc_data_level_offset() {
        let mut h = vec![0u8; 0xF8];
        h[..4].copy_from_slice(b"IVFC");
        h[0x10 + 5 * 0x18..0x10 + 5 * 0x18 + 8].copy_from_slice(&0x4000u64.to_le_bytes());
        assert_eq!(ivfc_data_off(&h), Some(0x4000));
        assert_eq!(ivfc_data_off(&h[..2]), None);
        h[0] = 0;
        assert_eq!(ivfc_data_off(&h), None);
    }
}
