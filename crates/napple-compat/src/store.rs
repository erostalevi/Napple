//! Persistence: stage result files (work/compat/<titleid>/<buildid>/<stage>.json), matrix.json, merging.
use crate::model::*;
use serde::Deserialize;
use std::fs;
use std::path::{Path, PathBuf};

const MAX_FILE: u64 = 4 << 20;

pub fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

/// Tolerant on-disk form: status is a case-insensitive string, everything else optional.
#[derive(Deserialize, Default)]
#[serde(default)]
struct RawStage {
    status: String,
    reason: String,
    evidence: String,
    time: u64,
    tool: String,
    counters: Counters,
}

pub fn is_title_id(s: &str) -> bool {
    s.len() == 16 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn is_build_id(s: &str) -> bool {
    s == "unknown" || (!s.is_empty() && s.len() <= 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
}

pub fn stage_path(root: &Path, title_id: &str, build_id: &str, stage: &str) -> PathBuf {
    root.join(title_id.to_ascii_uppercase()).join(build_id.to_ascii_lowercase()).join(format!("{stage}.json"))
}

pub fn read_stage(path: &Path) -> Result<StageResult, String> {
    let md = fs::metadata(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if md.len() > MAX_FILE {
        return Err(format!("{}: stage file too large", path.display()));
    }
    let text = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let raw: RawStage = serde_json::from_str(&text).map_err(|e| format!("{}: invalid JSON: {e}", path.display()))?;
    let status = Status::parse(&raw.status).ok_or_else(|| format!("{}: unknown status '{}'", path.display(), raw.status))?;
    let mtime = md.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
    Ok(StageResult {
        status,
        reason: raw.reason,
        evidence: raw.evidence,
        time: if raw.time == 0 { mtime } else { raw.time },
        tool: raw.tool,
        counters: raw.counters,
    })
}

pub fn write_stage(root: &Path, title_id: &str, build_id: &str, stage: &str, r: &StageResult) -> Result<(), String> {
    let p = stage_path(root, title_id, build_id, stage);
    fs::create_dir_all(p.parent().unwrap()).map_err(|e| e.to_string())?;
    let mut v = serde_json::to_value(r).map_err(|e| e.to_string())?;
    v["stage"] = stage.into();
    atomic_write(&p, &(serde_json::to_string_pretty(&v).map_err(|e| e.to_string())? + "\n"))
}

pub fn atomic_write(p: &Path, text: &str) -> Result<(), String> {
    if let Some(d) = p.parent() {
        fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let tmp = p.with_extension(format!("tmp{}", std::process::id()));
    fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, p).map_err(|e| format!("{}: {e}", p.display()))
}

pub fn load_matrix(p: &Path) -> Result<Matrix, String> {
    match fs::read_to_string(p) {
        Ok(t) => serde_json::from_str(&t).map_err(|e| format!("{}: {e}", p.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Matrix::new()),
        Err(e) => Err(format!("{}: {e}", p.display())),
    }
}

pub fn save_matrix(p: &Path, m: &Matrix) -> Result<(), String> {
    atomic_write(p, &(serde_json::to_string_pretty(m).map_err(|e| e.to_string())? + "\n"))
}

fn subdirs(p: &Path) -> Vec<(String, PathBuf)> {
    let mut v: Vec<(String, PathBuf)> = fs::read_dir(p)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .filter_map(|e| Some((e.file_name().into_string().ok()?, e.path())))
        .collect();
    v.sort();
    v
}

/// Overlays every stage file under `root` onto the matrix (a file wins when it is not older than the
/// matrix entry). Directories with malformed names and unparsable files are skipped and reported.
pub fn overlay_stage_files(m: &mut Matrix, root: &Path) -> Vec<String> {
    let mut warnings = Vec::new();
    for (tid, tdir) in subdirs(root) {
        if !is_title_id(&tid) {
            continue;
        }
        let tid = tid.to_ascii_uppercase();
        for (bid, bdir) in subdirs(&tdir) {
            if !is_build_id(&bid) {
                continue;
            }
            for stage in STAGES {
                let f = bdir.join(format!("{stage}.json"));
                if !f.is_file() {
                    continue;
                }
                match read_stage(&f) {
                    Ok(r) => {
                        let g = m.game_mut(&tid);
                        // Update-agnostic: apply to the version(s) carrying this build id; a build id that
                        // is unknown to the matrix gets a stub entry (e.g. runtime ran before `compat run`).
                        let idxs: Vec<usize> = g.versions.iter().enumerate().filter(|(_, v)| v.build_id == bid).map(|(i, _)| i).collect();
                        let idxs = if idxs.is_empty() {
                            g.versions.push(VersionEntry { kind: "base".into(), title_id: tid.clone(), build_id: bid.clone(), ..Default::default() });
                            vec![g.versions.len() - 1]
                        } else {
                            idxs
                        };
                        for i in idxs {
                            let cur = g.versions[i].stages.get(stage);
                            if cur.is_none_or(|c| r.time >= c.time) {
                                g.versions[i].stages.insert(stage.to_string(), r.clone());
                            }
                        }
                    }
                    Err(e) => warnings.push(e),
                }
            }
        }
    }
    warnings
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("napple-compat-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn c_style_stage_file_is_read() {
        let d = tmp("cfile");
        let p = d.join("runtime-boot.json");
        fs::write(&p, r#"{"stage":"runtime-boot","status":"PARTIAL","reason":"x","time":5,"counters":{"crashes":[{"signal":11,"pc":4096}],"unsupported_svcs":[26]},"extra":1}"#).unwrap();
        let r = read_stage(&p).unwrap();
        assert_eq!((r.status, r.time, r.counters.crashes[0].pc.as_str(), r.counters.unsupported_svcs.clone()), (Status::Partial, 5, "4096", vec![26]));
        fs::write(&p, r#"{"status":"maybe"}"#).unwrap();
        assert!(read_stage(&p).is_err());
        fs::write(&p, "{").unwrap();
        assert!(read_stage(&p).is_err());
    }

    #[test]
    fn overlay_newest_wins_and_creates_stub() {
        let d = tmp("overlay");
        let (tid, bid) = ("0100000000010000", "abcd");
        let mut m = Matrix::new();
        m.game_mut(tid).version_mut(0, bid).stages.insert("video".into(), StageResult { status: Status::Fail, time: 100, ..Default::default() });
        write_stage(&d, tid, bid, "video", &StageResult { status: Status::Pass, time: 50, ..Default::default() }).unwrap();
        write_stage(&d, tid, bid, "audio", &StageResult { status: Status::Pass, time: 50, ..Default::default() }).unwrap();
        write_stage(&d, "0100000000020000", "ff", "input", &StageResult { status: Status::Blocked, time: 1, ..Default::default() }).unwrap();
        fs::create_dir_all(d.join("not-a-title")).unwrap();
        assert!(overlay_stage_files(&mut m, &d).is_empty());
        let v = &m.games[0].versions[0];
        assert_eq!(v.stage("video").status, Status::Fail); // older file loses
        assert_eq!(v.stage("audio").status, Status::Pass);
        assert_eq!(m.games.len(), 2);
        // idempotent
        let before = m.clone();
        overlay_stage_files(&mut m, &d);
        assert_eq!(before, m);
    }

    #[test]
    fn matrix_roundtrip_and_missing_file() {
        let d = tmp("mx");
        assert_eq!(load_matrix(&d.join("none.json")).unwrap(), Matrix::new());
        let mut m = Matrix::new();
        m.game_mut("01").name = "G".into();
        save_matrix(&d.join("m.json"), &m).unwrap();
        assert_eq!(load_matrix(&d.join("m.json")).unwrap(), m);
    }
}
