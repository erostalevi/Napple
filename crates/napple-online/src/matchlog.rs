//! The match log: for every match the real account, profile and title behind each participant, whatever tier they play at (ghosts included:
//! the server always knows who is behind an alias). Kept 30 days after the match ended, purged by `purge` (the hub runs it hourly). Closed
//! matches are appended to `matchlog.jsonl` in the data dir; purge rewrites the file. Used for reports/bans and exported for admins.

use crate::crypto::{hex, random_bytes};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;

pub const RETENTION_SECS: u64 = 30 * 24 * 3600;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Who {
    pub account_id: String,
    pub profile_id: String,
    pub pid: u64,
    pub tier: u8,
    pub nickname: String,
    /// The name the game saw: the ghost alias at tier 5, else the nickname.
    pub shown_as: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Entry {
    #[serde(flatten)]
    pub who: Who,
    pub joined: u64,
    pub left: Option<u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Match {
    pub match_id: String,
    pub title_id: String,
    pub started: u64,
    pub ended: Option<u64>,
    pub participants: Vec<Entry>,
}

#[derive(Default)]
pub struct MatchLog {
    dir: Option<PathBuf>,
    open: Mutex<HashMap<u32, Match>>,
    closed: Mutex<Vec<Match>>,
}

impl MatchLog {
    pub fn open(dir: Option<PathBuf>) -> MatchLog {
        let mut closed = vec![];
        if let Some(d) = &dir {
            let _ = std::fs::create_dir_all(d);
            if let Ok(s) = std::fs::read_to_string(d.join("matchlog.jsonl")) {
                closed = s.lines().filter_map(|l| serde_json::from_str(l).ok()).collect();
            }
        }
        MatchLog { dir, open: Mutex::new(HashMap::new()), closed: Mutex::new(closed) }
    }

    /// `who` joins gathering `gid` (a match is created on the first join).
    pub fn join(&self, gid: u32, who: Who, title_id: &str, now: u64) {
        let mut o = self.open.lock().unwrap();
        let m = o.entry(gid).or_insert_with(|| Match { match_id: format!("m{}", hex(&random_bytes(8))), title_id: title_id.to_ascii_lowercase(), started: now, ended: None, participants: vec![] });
        if m.participants.iter().any(|e| e.who.pid == who.pid && e.left.is_none()) {
            return;
        }
        m.participants.push(Entry { who, joined: now, left: None });
    }

    pub fn leave(&self, gid: u32, pid: u64, now: u64) {
        if let Some(m) = self.open.lock().unwrap().get_mut(&gid) {
            for e in m.participants.iter_mut().filter(|e| e.who.pid == pid && e.left.is_none()) {
                e.left = Some(now);
            }
        }
    }

    /// Ends the match of `gid`: stamps everyone still in, persists it and returns it.
    pub fn close(&self, gid: u32, now: u64) -> Option<Match> {
        let mut m = self.open.lock().unwrap().remove(&gid)?;
        m.ended = Some(now);
        for e in m.participants.iter_mut().filter(|e| e.left.is_none()) {
            e.left = Some(now);
        }
        if let Some(d) = &self.dir {
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(d.join("matchlog.jsonl")) {
                let _ = writeln!(f, "{}", serde_json::to_string(&m).unwrap());
            }
        }
        self.closed.lock().unwrap().push(m.clone());
        Some(m)
    }

    pub fn match_id_of(&self, gid: u32) -> Option<String> {
        self.open.lock().unwrap().get(&gid).map(|m| m.match_id.clone())
    }

    /// Drops matches that ended more than RETENTION_SECS ago; returns how many.
    pub fn purge(&self, now: u64) -> usize {
        let mut c = self.closed.lock().unwrap();
        let before = c.len();
        c.retain(|m| m.ended.map_or(true, |e| now < e + RETENTION_SECS));
        let n = before - c.len();
        if n > 0 {
            if let Some(d) = &self.dir {
                let tmp = d.join("matchlog.jsonl.tmp");
                let body: String = c.iter().map(|m| serde_json::to_string(m).unwrap() + "\n").collect();
                if std::fs::write(&tmp, body).is_ok() {
                    let _ = std::fs::rename(&tmp, d.join("matchlog.jsonl"));
                }
            }
        }
        n
    }

    fn all(&self) -> Vec<Match> {
        let mut v = self.closed.lock().unwrap().clone();
        v.extend(self.open.lock().unwrap().values().cloned());
        v
    }

    /// Resolves a reported alias/nickname (the name the game showed) to the real people behind it, newest first. `match_id` narrows it.
    pub fn resolve(&self, shown_as: &str, match_id: Option<&str>) -> Vec<(Match, Entry)> {
        let mut out: Vec<(Match, Entry)> = self
            .all()
            .into_iter()
            .filter(|m| match_id.map_or(true, |id| m.match_id == id))
            .flat_map(|m| m.participants.iter().filter(|e| e.who.shown_as.eq_ignore_ascii_case(shown_as)).map(|e| (m.clone(), e.clone())).collect::<Vec<_>>())
            .collect();
        out.sort_by(|a, b| b.0.started.cmp(&a.0.started));
        out
    }

    /// Export for admin reports: matches that started at or after `since`.
    pub fn export(&self, since: u64) -> Value {
        let mut v: Vec<Match> = self.all().into_iter().filter(|m| m.started >= since).collect();
        v.sort_by_key(|m| m.started);
        json!({"retention_days": 30, "matches": v})
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn who(n: u8, tier: u8, shown: &str) -> Who {
        Who { account_id: format!("acc{n}"), profile_id: format!("prof{n}"), pid: n as u64, tier, nickname: format!("Nick{n}"), shown_as: shown.into() }
    }

    #[test]
    fn records_resolves_exports_and_purges() {
        let dir = std::env::temp_dir().join(format!("napple-matchlog-{}", hex(&random_bytes(6))));
        let log = MatchLog::open(Some(dir.clone()));
        log.join(1, who(1, 1, "Nick1"), "0100152000022000", 100);
        log.join(1, who(2, 5, "QuietFalcon7"), "0100152000022000", 110);
        log.join(1, who(2, 5, "QuietFalcon7"), "0100152000022000", 111); // duplicate join ignored
        let id = log.match_id_of(1).unwrap();
        // an ongoing match is already reportable
        let r = log.resolve("quietfalcon7", None);
        assert_eq!(r.len(), 1);
        assert_eq!((r[0].1.who.account_id.as_str(), r[0].1.who.nickname.as_str()), ("acc2", "Nick2"));
        log.leave(1, 2, 200);
        let m = log.close(1, 300).unwrap();
        assert_eq!((m.match_id.as_str(), m.ended, m.participants.len()), (id.as_str(), Some(300), 2));
        assert_eq!(m.participants[0].left, Some(300));
        assert_eq!(m.participants[1].left, Some(200));
        assert!(log.resolve("QuietFalcon7", Some("other")).is_empty());
        assert_eq!(log.export(0)["matches"].as_array().unwrap().len(), 1);
        // survives a restart
        let log2 = MatchLog::open(Some(dir.clone()));
        assert_eq!(log2.resolve("Nick1", Some(&id)).len(), 1);
        // retention: kept at 29 days after the end, gone after 30
        assert_eq!(log2.purge(300 + RETENTION_SECS - 1), 0);
        assert_eq!(log2.purge(300 + RETENTION_SECS), 1);
        assert!(log2.resolve("Nick1", None).is_empty());
        assert!(MatchLog::open(Some(dir.clone())).resolve("Nick1", None).is_empty(), "the file was rewritten");
        std::fs::remove_dir_all(dir).unwrap();
    }
}
