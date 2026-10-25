//! `compat run`: executes the non-runtime stages on every game found in a folder.
use crate::model::*;
use crate::store::*;
use napple_core::analysis::a64::{self, SiteKind};
use napple_core::application::ingest::Ingested;
use napple_core::application::package::{scan, BuildPlan, Pack};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::cnmt::TitleKind;
use napple_core::infrastructure::npdm::{parse_npdm, Npdm};
use napple_core::infrastructure::nso::Nso;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const TOOL: &str = "napple-compat";

pub struct RunOpts {
    pub folder: PathBuf,
    pub keys: PathBuf,
    pub stage_dir: PathBuf,
    pub matrix: PathBuf,
    pub md: PathBuf,
}

/// Lists .nsp/.xci/.nsz/.xcz files (case-insensitive), recursing up to 16 levels, never following symlinks.
pub fn find_games(dir: &Path) -> Vec<PathBuf> {
    fn go(d: &Path, depth: usize, out: &mut Vec<PathBuf>) {
        if depth > 16 {
            return;
        }
        let Ok(rd) = std::fs::read_dir(d) else { return };
        for e in rd.flatten() {
            let Ok(t) = e.file_type() else { continue };
            let p = e.path();
            if t.is_dir() {
                go(&p, depth + 1, out);
            } else if t.is_file() && p.extension().and_then(|x| x.to_str()).is_some_and(|x| ["nsp", "xci", "nsz", "xcz"].iter().any(|e| x.eq_ignore_ascii_case(e))) {
                out.push(p);
            }
        }
    }
    let mut v = Vec::new();
    go(dir, 0, &mut v);
    v.sort();
    v
}

/// Title id from a "[0100XXXXXXXXXXXX]" tag in a file name (used only when the container cannot be parsed).
pub fn title_id_from_name(name: &str) -> Option<String> {
    let mut rest = name;
    while let Some(i) = rest.find('[') {
        let cand = rest.get(i + 1..i + 17)?;
        if is_title_id(cand) && rest.get(i + 17..i + 18) == Some("]") {
            return Some(cand.to_ascii_uppercase());
        }
        rest = &rest[i + 1..];
    }
    None
}

fn res(status: Status, reason: impl Into<String>, evidence: &str, t: u64) -> StageResult {
    StageResult { status, reason: reason.into(), evidence: evidence.to_string(), time: t, tool: TOOL.into(), counters: Counters::default() }
}

pub fn npdm_info(n: &Npdm) -> NpdmInfo {
    let k = &n.kernel;
    let mut used: Vec<String> = n.services.iter().filter(|s| !s.host).map(|s| s.name.clone()).collect();
    let mut hosted: Vec<String> = n.services.iter().filter(|s| s.host).map(|s| s.name.clone()).collect();
    used.sort();
    hosted.sort();
    NpdmInfo {
        name: n.name.clone(),
        is_64bit: n.is_64bit,
        address_space: n.address_space,
        main_thread_priority: n.main_thread_priority,
        main_thread_core: n.main_thread_core,
        main_thread_stack_size: n.main_thread_stack_size,
        system_resource_size: n.system_resource_size,
        memory_region: n.memory_region,
        production: n.acid_production,
        fs_permissions: format!("{:#x}", n.fs_permissions),
        handle_table_size: k.handle_table_size,
        kernel_version: k.kernel_version.map(|(a, b)| format!("{a}.{b}")),
        thread_info: k.thread_info.map(|(lo, hi, c0, c1)| format!("prio {hi}..{lo}, cores {c0}..{c1}")),
        allowed_svcs: k.allowed_svcs.clone(),
        memory_maps: k.memory_maps.iter().map(|(a, s, ro, normal)| format!("mem {a:#x}+{s:#x}{}{}", if *ro { " ro" } else { "" }, if *normal { "" } else { " io" })).collect(),
        io_maps: k.io_maps.iter().map(|a| format!("io {a:#x}")).collect(),
        debug_flags: format!("allow_debug={} force_debug={} program_type={}", k.enable_debug, k.force_debug, k.program_type.map_or("n/a".into(), |t| t.to_string())),
        services_used: used,
        services_hosted: hosted,
    }
}

fn is_nso(name: &str) -> bool {
    matches!(name, "main" | "rtld" | "sdk") || name.starts_with("subsdk")
}

/// Order modules the way the loader maps them: rtld, main, subsdk0..N, sdk.
fn nso_rank(name: &str) -> (u8, u32) {
    match name {
        "rtld" => (0, 0),
        "main" => (1, 0),
        "sdk" => (3, 0),
        n => (2, n.trim_start_matches("subsdk").parse().unwrap_or(0)),
    }
}

pub struct Analysis {
    pub modules: Vec<ModuleInfo>,
    pub npdm: Option<Npdm>,
    pub main_build: Option<String>,
    pub notes: Vec<String>,
    pub nso_error: Option<String>,
    pub scan_totals: BTreeMap<String, u64>,
}

/// nso-load + site-scan over the ExeFS of one program (pure: takes already-ingested data).
pub fn analyze(prog: &Ingested) -> Analysis {
    let mut a = Analysis { modules: vec![], npdm: None, main_build: None, notes: vec![], nso_error: None, scan_totals: BTreeMap::new() };
    let mut files: Vec<&(String, Vec<u8>)> = prog.exefs.iter().filter(|(n, _)| is_nso(n)).collect();
    files.sort_by_key(|(n, _)| nso_rank(n));
    for (name, data) in files {
        let nso = match Nso::parse(data) {
            Ok(n) => n,
            Err(e) => {
                a.nso_error.get_or_insert(format!("{name}: {e}"));
                continue;
            }
        };
        let rep = a64::scan(&nso.text.data, nso.text.mem_offset);
        let mut svcs: Vec<u32> = rep
            .sites
            .get(&SiteKind::Svc)
            .into_iter()
            .flatten()
            .filter_map(|&off| {
                let i = off.checked_sub(nso.text.mem_offset)? as usize;
                let w = u32::from_le_bytes(nso.text.data.get(i..i + 4)?.try_into().ok()?);
                Some((w >> 5) & 0xFFFF)
            })
            .collect();
        svcs.sort_unstable();
        svcs.dedup();
        let sites: BTreeMap<String, u64> = rep.sites.iter().map(|(k, v)| (format!("{k:?}"), v.len() as u64)).collect();
        for (k, n) in &sites {
            *a.scan_totals.entry(k.clone()).or_default() += n;
        }
        let bid = nso.build_id_hex();
        if name == "main" {
            a.main_build = Some(bid[..16].to_string());
        }
        a.modules.push(ModuleInfo { name: name.clone(), build_id: bid, text_bytes: nso.text.data.len() as u64, sites, svc_numbers: svcs });
    }
    match prog.exefs.iter().find(|(n, _)| n == "main.npdm") {
        Some((_, d)) => match parse_npdm(d) {
            Ok(n) => a.npdm = Some(n),
            Err(e) => a.notes.push(format!("[auto] main.npdm could not be parsed: {e}")),
        },
        None => a.notes.push("[auto] no main.npdm in ExeFS".into()),
    }
    if let Some(n) = &a.npdm {
        // SVCs present in module text but not granted by the NPDM: either dead code or a parser gap.
        let allowed: Vec<u32> = n.kernel.allowed_svcs.iter().map(|&x| x as u32).collect();
        for m in &a.modules {
            let extra: Vec<String> = m.svc_numbers.iter().filter(|s| !allowed.contains(s)).map(|s| format!("{s:#x}")).collect();
            if !extra.is_empty() {
                a.notes.push(format!("[auto] module {} contains svc numbers not in the NPDM allow-list: {}", m.name, extra.join(" ")));
            }
        }
        if !n.is_64bit {
            a.notes.push("[auto] RISK: NPDM declares a 32-bit process; the A64 pipeline does not apply".into());
        }
    }
    a
}

/// Hook for the rewrite stages. `napple-rewrite` exposes no library API yet (only a CLI stub `run`), so this
/// returns None and both stages stay Untested. When the crate gains e.g. `rewrite_program(&Ingested)` and a
/// verifier, call them here and return (rewrite, rewrite-verify) results.
pub fn rewrite_hook(_prog: &Ingested, _t: u64) -> Option<(StageResult, StageResult)> {
    None
}

fn kind_name(k: TitleKind) -> &'static str {
    if k == TitleKind::Patch { "update" } else { "base" }
}

pub fn run(o: &RunOpts, log: &mut dyn FnMut(&str)) -> Result<Matrix, String> {
    let keys = KeySet::load(&o.keys).map_err(|e| e.to_string())?;
    let t = now();
    let mut m = load_matrix(&o.matrix)?;
    let files = find_games(&o.folder);
    if files.is_empty() {
        return Err(format!("no .nsp/.xci/.nsz/.xcz files under {}", o.folder.display()));
    }
    let mut packs: Vec<Pack> = Vec::new();
    for f in &files {
        let fname = f.file_name().and_then(|s| s.to_str()).unwrap_or("");
        log(&format!("scan {fname}"));
        match scan(f, &keys) {
            Ok(p) if p.is_empty() => log(&format!("  no content meta found in {fname}")),
            Ok(p) => packs.extend(p),
            Err(e) => {
                log(&format!("  ingest failed: {e}"));
                let tid = title_id_from_name(fname).unwrap_or_else(|| "0000000000000000".into());
                let r = res(Status::Fail, format!("container scan failed: {e}"), "", t);
                write_stage(&o.stage_dir, &tid, "unknown", "ingest", &r)?;
                m.game_mut(&tid).version_mut(0, "unknown").stages.insert("ingest".into(), r);
            }
        }
    }
    let mut groups: BTreeMap<u64, Vec<Pack>> = BTreeMap::new();
    for p in packs {
        groups.entry(p.meta.application_id).or_default().push(p);
    }
    for (app, group) in groups {
        let tid = format!("{app:016X}");
        let plan = BuildPlan::from_packs(group);
        log(&format!("game {tid}: base={} updates={} dlc={}", plan.base.is_some(), plan.updates.len(), plan.dlcs.len()));
        let mut targets: Vec<usize> = plan.base.into_iter().collect();
        targets.extend(&plan.updates);
        let dlc: Vec<DlcEntry> = plan.dlcs.iter().map(|&i| DlcEntry { title_id: format!("{:016X}", plan.packs[i].meta.title_id), version: plan.packs[i].meta.version }).collect();
        let mut touched: Vec<(u32, String)> = vec![];
        let mut sets: Vec<(String, Vec<String>)> = vec![];
        for i in targets {
            let p = &plan.packs[i];
            let (ver, kind) = (p.meta.version, kind_name(p.meta.kind));
            let prog = p.program(&keys);
            let control = p.control(&keys);
            let (bid, entry_stages, modules, npdm, notes) = match &prog {
                Err(e) => ("unknown".to_string(), vec![("ingest", res(Status::Fail, e.to_string(), "", t))], vec![], None, vec![]),
                Ok(prog) => {
                    let a = analyze(prog);
                    let bid = a.main_build.clone().unwrap_or_else(|| "unknown".into());
                    let mut st = vec![];
                    let rom = match (&prog.romfs, &prog.romfs_note) {
                        (Some(r), _) => format!("romfs {} files", r.files.len()),
                        (None, Some(n)) => format!("romfs pending ({n})"),
                        _ => "no romfs".into(),
                    };
                    let st_ingest = if prog.romfs.is_some() || kind == "update" && prog.romfs_note.is_some() {
                        let status = if prog.romfs.is_some() { Status::Pass } else { Status::Partial };
                        res(status, format!("{} ExeFS files, {rom}", prog.exefs.len()), "", t)
                    } else {
                        res(Status::Partial, format!("{} ExeFS files, {rom}", prog.exefs.len()), "", t)
                    };
                    st.push(("ingest", st_ingest));
                    let has_main = a.modules.iter().any(|m| m.name == "main");
                    st.push((
                        "nso-load",
                        match (&a.nso_error, has_main) {
                            (Some(e), _) => res(Status::Fail, e.clone(), "", t),
                            (None, false) => res(Status::Fail, "no main NSO in ExeFS", "", t),
                            (None, true) => res(Status::Pass, format!("{} modules, segment hashes verified, main build {}", a.modules.len(), bid), "", t),
                        },
                    ));
                    if a.modules.is_empty() {
                        st.push(("site-scan", res(Status::Blocked, "no loadable modules", "", t)));
                    } else {
                        let tot: Vec<String> = a.scan_totals.iter().map(|(k, n)| format!("{k}={n}")).collect();
                        st.push(("site-scan", res(Status::Pass, tot.join(" "), "", t)));
                    }
                    if let Some((rw, rv)) = rewrite_hook(prog, t) {
                        st.push(("rewrite", rw));
                        st.push(("rewrite-verify", rv));
                    }
                    (bid, st, a.modules, a.npdm.as_ref().map(npdm_info), a.notes)
                }
            };
            log(&format!("  {kind} v{ver} build {bid}: {} modules", modules.len()));
            let g = m.game_mut(&tid);
            let v = g.version_mut(ver, &bid);
            v.kind = kind.into();
            v.title_id = format!("{:016X}", p.meta.title_id);
            if let Ok(Some(n)) = &control {
                v.display_version = n.display_version.clone();
                if g.name.is_empty() || kind == "base" {
                    if let Some(nm) = n.name() {
                        g.name = nm.to_string();
                        g.publisher = n.publisher().unwrap_or("").to_string();
                    }
                }
            }
            let v = g.version_mut(ver, &bid);
            let mut vnotes: Vec<String> = v.notes.iter().filter(|n| !n.starts_with("[auto]")).cloned().collect();
            vnotes.extend(notes);
            if let Err(e) = &control {
                vnotes.push(format!("[auto] control NCA unreadable: {e}"));
            }
            sets.push((format!("{kind} v{ver}"), modules.iter().map(|x| x.name.clone()).collect()));
            v.notes = vnotes;
            if !modules.is_empty() {
                v.modules = modules;
            }
            if npdm.is_some() {
                v.npdm = npdm;
            }
            for (s, r) in entry_stages {
                write_stage(&o.stage_dir, &tid, &bid, s, &r)?;
                v.stages.insert(s.to_string(), r);
            }
            touched.push((ver, bid));
        }
        // build-plan is a per-game stage, recorded on every version of the group.
        let bp = match (plan.base, plan.warnings.is_empty()) {
            (None, _) => res(Status::Fail, plan.warnings.join("; "), "", t),
            (Some(_), true) => res(Status::Pass, format!("base + {} update(s) + {} DLC", plan.updates.len(), plan.dlcs.len()), "", t),
            (Some(_), false) => res(Status::Partial, plan.warnings.join("; "), "", t),
        };
        let g = m.game_mut(&tid);
        g.dlc = dlc;
        for (ver, bid) in &touched {
            write_stage(&o.stage_dir, &tid, bid, "build-plan", &bp)?;
            g.version_mut(*ver, bid).stages.insert("build-plan".into(), bp.clone());
        }
        // Differing NSO sets between versions of the same game.
        g.notes.retain(|n| !n.starts_with("[auto]"));
        if let Some((n0, s0)) = sets.first() {
            for (n, s) in sets.iter().skip(1).filter(|(_, s)| s != &sets[0].1) {
                g.notes.push(format!("[auto] NSO set differs: {n0} = [{}], {n} = [{}]", s0.join(" "), s.join(" ")));
            }
        }
    }
    // Drop stub "unknown" ingest-failure entries that were superseded by a real parse of the same title.
    m.last_run = t;
    m.last_run_iso = iso(t);
    for w in overlay_stage_files(&mut m, &o.stage_dir) {
        log(&format!("warning: {w}"));
    }
    m.sort();
    save_matrix(&o.matrix, &m)?;
    atomic_write(&o.md, &crate::render::render(&m))?;
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_id_from_file_name() {
        assert_eq!(title_id_from_name("Zelda [01006bb00c6f0000][v0].nsp").as_deref(), Some("01006BB00C6F0000"));
        assert_eq!(title_id_from_name("Game [v0] [0100000000010000].nsp").as_deref(), Some("0100000000010000"));
        assert_eq!(title_id_from_name("nothing.nsp"), None);
        assert_eq!(title_id_from_name("bad [01006bb00c6f000].nsp"), None);
        assert_eq!(title_id_from_name("é[0100"), None);
    }

    #[test]
    fn finds_only_games_and_ranks_modules() {
        let d = std::env::temp_dir().join(format!("napple-compat-find-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("sub")).unwrap();
        for f in ["a.nsp", "sub/b.XCI", "c.txt", "d.nsz", "sub/e.XCZ", "f.nsz.bak"] {
            std::fs::write(d.join(f), b"x").unwrap();
        }
        let v = find_games(&d);
        assert_eq!(v.len(), 4);
        assert!(nso_rank("rtld") < nso_rank("main") && nso_rank("main") < nso_rank("subsdk0") && nso_rank("subsdk2") < nso_rank("subsdk10") && nso_rank("subsdk10") < nso_rank("sdk"));
    }

    #[test]
    fn analyze_without_modules_reports_note() {
        let prog = Ingested { ncas: vec![], exefs: vec![("main.npdm".into(), vec![1, 2, 3])], romfs: None, title_id: None, romfs_note: None };
        let a = analyze(&prog);
        assert!(a.modules.is_empty() && a.npdm.is_none());
        assert!(a.notes[0].contains("could not be parsed"));
    }
}
