//! Reports to napple-social after a match, with the service credential (`NAPPLE_SOCIAL_SERVICE_TOKEN`, sent as the value of the
//! `X-Napple-Service` header, which is what napple-social compares):
//! * `POST {social}/social/v1/encounters {profile_ids, title_id, match_id}`: everyone who shared the lobby. This is also the "shared
//!   lobby" proof napple-social needs for tier-3 friend requests. Ghost (tier 5) players are never listed: no trace.
//! * `POST {social}/social/v1/internal/stats {profile_id, title_id, deltas}` per non-ghost participant (`matches_played`, `play_seconds`); the
//!   `profile_id` field is how the service credential says whose stats these are.
//! Jobs that fail (network, 5xx, 429) stay in a bounded outbox and are retried on the next flush.

use crate::matchlog::Match;
use crate::upstream::Upstream;
use serde_json::json;
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

const MAX_QUEUE: usize = 1000;
const MAX_ATTEMPTS: u32 = 8;

struct Job {
    path: &'static str,
    body: String,
    attempts: u32,
}

pub struct Reporter {
    up: Arc<dyn Upstream>,
    social_url: String,
    token: String,
    outbox: Mutex<VecDeque<Job>>,
}

impl Reporter {
    pub fn new(up: Arc<dyn Upstream>, social_url: &str, token: &str) -> Reporter {
        Reporter { up, social_url: social_url.trim_end_matches('/').to_string(), token: token.to_string(), outbox: Mutex::new(VecDeque::new()) }
    }

    /// Queues the encounter and stats reports of a finished match.
    pub fn report_match(&self, m: &Match) {
        let visible: Vec<_> = m.participants.iter().filter(|e| e.who.tier < 5).collect();
        let mut ids: Vec<&str> = visible.iter().map(|e| e.who.profile_id.as_str()).collect();
        ids.sort_unstable();
        ids.dedup();
        let mut q = self.outbox.lock().unwrap();
        let mut push = |path: &'static str, body: serde_json::Value| {
            if q.len() < MAX_QUEUE {
                q.push_back(Job { path, body: body.to_string(), attempts: 0 });
            }
        };
        if ids.len() >= 2 {
            push("/social/v1/encounters", json!({"profile_ids": ids, "title_id": m.title_id, "match_id": m.match_id}));
        }
        for id in &ids {
            let secs: u64 = visible.iter().filter(|e| &e.who.profile_id == id).map(|e| e.left.unwrap_or(m.ended.unwrap_or(e.joined)).saturating_sub(e.joined)).sum();
            push("/social/v1/internal/stats", json!({"profile_id": id, "title_id": m.title_id, "deltas": {"matches_played": 1, "play_seconds": secs}}));
        }
    }

    /// Sends what is queued; returns how many jobs are still pending.
    pub fn flush(&self) -> usize {
        let jobs: Vec<Job> = self.outbox.lock().unwrap().drain(..).collect();
        let mut keep = vec![];
        for mut j in jobs {
            let url = format!("{}{}", self.social_url, j.path);
            let r = self.up.request("POST", &url, None, &[("X-Napple-Service", self.token.as_str())], Some(&j.body));
            let retry = match r {
                Ok((s, _)) if (200..300).contains(&s) => false,
                Ok((s, _)) if (400..500).contains(&s) && s != 408 && s != 429 => {
                    println!("napple-online: social refused {} ({s}); dropped", j.path);
                    false
                }
                _ => true,
            };
            j.attempts += 1;
            if retry && j.attempts < MAX_ATTEMPTS {
                keep.push(j);
            }
        }
        let mut q = self.outbox.lock().unwrap();
        for j in keep.into_iter().rev() {
            q.push_front(j);
        }
        q.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::testutil::Mock;
    use crate::matchlog::{Entry, Who};

    fn entry(n: u8, tier: u8, joined: u64, left: u64) -> Entry {
        Entry { who: Who { account_id: format!("a{n}"), profile_id: format!("p{n}"), pid: n as u64, tier, nickname: "n".into(), shown_as: "n".into() }, joined, left: Some(left) }
    }

    #[test]
    fn encounters_exclude_ghosts_and_stats_are_per_profile() {
        let m = Mock::with_key("k", &crate::identity::testutil::key(1));
        let r = Reporter::new(m.clone(), "http://social/", "svc");
        let game = Match { match_id: "m1".into(), title_id: "0100152000022000".into(), started: 100, ended: Some(400), participants: vec![entry(1, 1, 100, 400), entry(2, 3, 150, 400), entry(3, 5, 100, 400)] };
        r.report_match(&game);
        assert_eq!(r.flush(), 0);
        let posts = m.posts.lock().unwrap();
        assert_eq!(posts.len(), 3);
        let (url, bearer, headers, body) = &posts[0];
        assert_eq!(url, "http://social/social/v1/encounters");
        assert_eq!(bearer.as_deref(), None);
        assert!(headers.contains(&("X-Napple-Service".into(), "svc".into())), "napple-social compares the header value with its token");
        assert_eq!(posts[1].0, "http://social/social/v1/internal/stats");
        assert_eq!(body["profile_ids"], json!(["p1", "p2"]));
        assert_eq!(body["match_id"], "m1");
        assert_eq!(posts[1].3["profile_id"], "p1");
        assert_eq!(posts[1].3["deltas"]["play_seconds"], 300);
        assert_eq!(posts[2].3["deltas"]["play_seconds"], 250);
        assert!(!posts.iter().any(|p| p.3.to_string().contains("p3")), "the ghost leaves no trace");
    }

    #[test]
    fn solo_match_has_no_encounter_and_failures_are_retried() {
        let m = Mock::with_key("k", &crate::identity::testutil::key(1));
        let r = Reporter::new(m.clone(), "http://social", "svc");
        r.report_match(&Match { match_id: "m".into(), title_id: "t".into(), started: 0, ended: Some(10), participants: vec![entry(1, 1, 0, 10)] });
        *m.social_status.lock().unwrap() = 503;
        assert_eq!(r.flush(), 1, "stats kept for retry; no encounter for one player");
        *m.social_status.lock().unwrap() = 201;
        assert_eq!(r.flush(), 0);
        // a permanent refusal is dropped, not retried forever
        r.report_match(&Match { match_id: "m".into(), title_id: "t".into(), started: 0, ended: Some(10), participants: vec![entry(1, 1, 0, 10)] });
        *m.social_status.lock().unwrap() = 403;
        assert_eq!(r.flush(), 0);
    }
}
