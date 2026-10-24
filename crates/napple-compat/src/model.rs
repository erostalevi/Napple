//! Compatibility matrix data model (domain layer: no I/O).
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// Fixed pipeline order. The kebab-case name is the stage-file name and the JSON key.
pub const STAGES: [&str; 13] = [
    "ingest", "build-plan", "nso-load", "site-scan", "rewrite", "rewrite-verify", "runtime-boot", "video", "audio", "input", "saves", "title-screen",
    "ingame",
];

pub fn is_stage(s: &str) -> bool {
    STAGES.contains(&s)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    #[default]
    Untested,
    Pass,
    Partial,
    Fail,
    Blocked,
}

impl Status {
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s.to_ascii_lowercase().as_str() {
            "untested" => Self::Untested,
            "pass" => Self::Pass,
            "partial" => Self::Partial,
            "fail" => Self::Fail,
            "blocked" => Self::Blocked,
            _ => return None,
        })
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Untested => "untested",
            Self::Pass => "pass",
            Self::Partial => "partial",
            Self::Fail => "fail",
            Self::Blocked => "blocked",
        }
    }
    /// ASCII symbol used in the markdown table.
    pub fn symbol(self) -> &'static str {
        match self {
            Self::Untested => ".",
            Self::Pass => "OK",
            Self::Partial => "~",
            Self::Fail => "X",
            Self::Blocked => "B",
        }
    }
}

/// Accepts a JSON string or number (C writers may emit either) and stores it as text.
fn str_or_num<'de, D: serde::Deserializer<'de>>(d: D) -> Result<String, D::Error> {
    let v = serde_json::Value::deserialize(d)?;
    Ok(match v {
        serde_json::Value::String(s) => s,
        serde_json::Value::Null => String::new(),
        o => o.to_string(),
    })
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Crash {
    #[serde(default)]
    pub signal: i64,
    /// Guest program counter as a hex string ("0x...") or decimal.
    #[serde(default, deserialize_with = "str_or_num")]
    pub pc: String,
    #[serde(default)]
    pub note: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Counters {
    pub unsupported_svcs: Vec<u64>,
    pub missing_ipc: Vec<String>,
    pub missing_nvn: Vec<String>,
    pub crashes: Vec<Crash>,
    pub fps_notes: Vec<String>,
}

impl Counters {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
    /// Union (deduplicated, order-preserving) of two counter sets.
    pub fn merge(&mut self, o: &Counters) {
        fn u<T: Clone + PartialEq>(a: &mut Vec<T>, b: &[T]) {
            for x in b {
                if !a.contains(x) {
                    a.push(x.clone());
                }
            }
        }
        u(&mut self.unsupported_svcs, &o.unsupported_svcs);
        u(&mut self.missing_ipc, &o.missing_ipc);
        u(&mut self.missing_nvn, &o.missing_nvn);
        u(&mut self.crashes, &o.crashes);
        u(&mut self.fps_notes, &o.fps_notes);
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct StageResult {
    pub status: Status,
    pub reason: String,
    /// Log/artifact path (relative, under work/); never game bytes.
    pub evidence: String,
    /// Unix seconds when the result was produced.
    pub time: u64,
    /// Producer ("napple-compat", "runtime", "manual", ...).
    pub tool: String,
    #[serde(skip_serializing_if = "Counters::is_empty")]
    pub counters: Counters,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct ModuleInfo {
    pub name: String,
    pub build_id: String,
    pub text_bytes: u64,
    pub sites: BTreeMap<String, u64>,
    /// Distinct SVC numbers appearing in the module text.
    pub svc_numbers: Vec<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct NpdmInfo {
    pub name: String,
    pub is_64bit: bool,
    pub address_space: u8,
    pub main_thread_priority: u8,
    pub main_thread_core: u8,
    pub main_thread_stack_size: u32,
    pub system_resource_size: u32,
    pub memory_region: u8,
    pub production: bool,
    pub fs_permissions: String,
    pub handle_table_size: Option<u16>,
    pub kernel_version: Option<String>,
    pub thread_info: Option<String>,
    pub allowed_svcs: Vec<u8>,
    pub memory_maps: Vec<String>,
    pub io_maps: Vec<String>,
    pub debug_flags: String,
    /// Services the process connects to ("sm:" style names).
    pub services_used: Vec<String>,
    /// Services the process itself hosts.
    pub services_hosted: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct VersionEntry {
    /// "base" | "update"
    pub kind: String,
    pub title_id: String,
    pub version: u32,
    pub display_version: String,
    /// First 16 hex chars of the main NSO build id (used as directory name); "unknown" if not read.
    pub build_id: String,
    pub stages: BTreeMap<String, StageResult>,
    pub modules: Vec<ModuleInfo>,
    pub npdm: Option<NpdmInfo>,
    pub notes: Vec<String>,
}

impl VersionEntry {
    pub fn stage(&self, s: &str) -> StageResult {
        self.stages.get(s).cloned().unwrap_or_default()
    }
    pub fn counters(&self) -> Counters {
        let mut c = Counters::default();
        for r in self.stages.values() {
            c.merge(&r.counters);
        }
        c
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct DlcEntry {
    pub title_id: String,
    pub version: u32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct GameEntry {
    /// Application (base) title id, 16 uppercase hex digits.
    pub title_id: String,
    pub name: String,
    pub publisher: String,
    pub versions: Vec<VersionEntry>,
    pub dlc: Vec<DlcEntry>,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Matrix {
    pub schema: u32,
    /// Unix seconds of the last `compat run` that touched this matrix.
    pub last_run: u64,
    pub last_run_iso: String,
    pub games: Vec<GameEntry>,
}

impl Matrix {
    pub fn new() -> Self {
        Self { schema: 1, ..Default::default() }
    }

    pub fn game_mut(&mut self, title_id: &str) -> &mut GameEntry {
        if let Some(i) = self.games.iter().position(|g| g.title_id == title_id) {
            return &mut self.games[i];
        }
        self.games.push(GameEntry { title_id: title_id.to_string(), ..Default::default() });
        self.games.last_mut().unwrap()
    }

    pub fn sort(&mut self) {
        self.games.sort_by(|a, b| a.title_id.cmp(&b.title_id));
        for g in &mut self.games {
            g.versions.sort_by(|a, b| (a.version, &a.build_id).cmp(&(b.version, &b.build_id)));
        }
    }
}

impl GameEntry {
    /// Version entry for (version, build id), created if missing.
    pub fn version_mut(&mut self, version: u32, build_id: &str) -> &mut VersionEntry {
        if let Some(i) = self.versions.iter().position(|v| v.version == version && v.build_id == build_id) {
            return &mut self.versions[i];
        }
        self.versions.push(VersionEntry { version, build_id: build_id.to_string(), ..Default::default() });
        self.versions.last_mut().unwrap()
    }
}

/// Unix seconds -> "YYYY-MM-DDTHH:MM:SSZ" (UTC, proleptic Gregorian).
pub fn iso(t: u64) -> String {
    let (days, rem) = ((t / 86400) as i64, t % 86400);
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z", rem / 3600, rem % 3600 / 60, rem % 60)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn iso_known_dates() {
        assert_eq!(iso(0), "1970-01-01T00:00:00Z");
        assert_eq!(iso(1_700_000_000), "2023-11-14T22:13:20Z");
        assert_eq!(iso(951_782_400), "2000-02-29T00:00:00Z");
    }

    #[test]
    fn status_parse_and_symbols() {
        assert_eq!(Status::parse("PASS"), Some(Status::Pass));
        assert_eq!(Status::parse("nope"), None);
        assert_eq!(Status::default(), Status::Untested);
    }

    #[test]
    fn counters_merge_dedups() {
        let mut a = Counters { missing_ipc: vec!["a".into()], ..Default::default() };
        a.merge(&Counters { missing_ipc: vec!["a".into(), "b".into()], unsupported_svcs: vec![7], ..Default::default() });
        assert_eq!((a.missing_ipc.len(), a.unsupported_svcs.len()), (2, 1));
    }

    #[test]
    fn game_and_version_are_upserted() {
        let mut m = Matrix::new();
        m.game_mut("01").version_mut(0, "ab").notes.push("x".into());
        m.game_mut("01").version_mut(0, "ab");
        assert_eq!((m.games.len(), m.games[0].versions.len()), (1, 1));
    }
}
