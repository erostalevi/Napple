//! Matchmaking data: Gathering / MatchmakeSession (NEX 4.x layout), search criteria, and the session table with its matching rules.

use crate::crypto::random_bytes;
use crate::nex::{DateTime, Reader, Result, Writer};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Gathering {
    pub id: u32,
    pub owner: u64,
    pub host: u64,
    pub min: u16,
    pub max: u16,
    pub policy: u32,
    pub policy_arg: u32,
    pub flags: u32,
    pub state: u32,
    pub description: String,
}

/// Gathering flag: when the owner leaves, another participant becomes owner (else the gathering ends).
pub const GATHERING_FLAG_MIGRATE_OWNER: u32 = 0x10;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct MatchmakeSession {
    pub g: Gathering,
    pub g_version: u8,
    pub version: u8,
    pub game_mode: u32,
    pub attribs: Vec<u32>,
    pub open: bool,
    pub system_type: u32,
    pub app_buffer: Vec<u8>,
    pub participation_count: u32,
    pub progress_score: u8,
    pub session_key: Vec<u8>,
    pub option0: u32,
    /// MatchmakeParam (Map<String, Variant>), kept as the client sent it: (version, content).
    pub param: (u8, Vec<u8>),
    pub started: DateTime,
    pub user_password: String,
    pub refer_gid: u32,
    pub user_password_enabled: bool,
    pub system_password_enabled: bool,
    pub codeword: String,
    /// Fields of a newer revision than this server knows, passed through unchanged.
    pub tail: Vec<u8>,
}

impl Gathering {
    fn decode(r: &mut Reader) -> Result<Gathering> {
        Ok(Gathering {
            id: r.u32()?,
            owner: r.pid()?,
            host: r.pid()?,
            min: r.u16()?,
            max: r.u16()?,
            policy: r.u32()?,
            policy_arg: r.u32()?,
            flags: r.u32()?,
            state: r.u32()?,
            description: r.string()?,
        })
    }
    fn encode(&self, w: &mut Writer) {
        w.u32(self.id).pid(self.owner).pid(self.host).u16(self.min).u16(self.max).u32(self.policy).u32(self.policy_arg).u32(self.flags).u32(self.state);
        w.string(&self.description);
    }
}

impl MatchmakeSession {
    /// A MatchmakeSession as it appears in a stream: the Gathering structure, then the MatchmakeSession structure.
    pub fn decode(r: &mut Reader) -> Result<MatchmakeSession> {
        let (g_version, mut gr) = r.structure()?;
        let g = Gathering::decode(&mut gr)?;
        let (version, mut m) = r.structure()?;
        let mut s = MatchmakeSession { g, g_version, version, ..Default::default() };
        s.game_mode = m.u32()?;
        s.attribs = m.list(|r| r.u32())?;
        s.open = m.bool()?;
        s.system_type = m.u32()?;
        s.app_buffer = m.buffer()?;
        s.participation_count = m.u32()?;
        s.progress_score = m.u8()?;
        s.session_key = m.buffer()?;
        s.option0 = m.u32()?;
        let (pv, mut pr) = m.structure()?;
        s.param = (pv, pr.rest().to_vec());
        s.started = m.datetime()?;
        s.user_password = m.string()?;
        s.refer_gid = m.u32()?;
        s.user_password_enabled = m.bool()?;
        s.system_password_enabled = m.bool()?;
        s.codeword = m.string()?;
        s.tail = m.rest().to_vec();
        Ok(s)
    }
    pub fn encode(&self, w: &mut Writer) {
        w.structure(self.g_version, |w| self.g.encode(w));
        w.structure(self.version, |w| {
            w.u32(self.game_mode);
            w.list(&self.attribs, |w, a| {
                w.u32(*a);
            });
            w.bool(self.open).u32(self.system_type).buffer(&self.app_buffer).u32(self.participation_count).u8(self.progress_score);
            w.buffer(&self.session_key).u32(self.option0);
            w.structure(self.param.0, |w| {
                w.bytes(&self.param.1);
            });
            w.datetime(self.started).string(&self.user_password).u32(self.refer_gid);
            w.bool(self.user_password_enabled).bool(self.system_password_enabled).string(&self.codeword).bytes(&self.tail);
        });
    }
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut w = Writer::new();
        self.encode(&mut w);
        w.buf
    }
    /// Data<Gathering> holding this session.
    pub fn to_any(&self) -> Vec<u8> {
        let mut w = Writer::new();
        w.any("MatchmakeSession", &self.to_bytes());
        w.buf
    }
}

/// The parts of MatchmakeSessionSearchCriteria the matching uses (the rest is read past).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Criteria {
    pub attribs: Vec<String>,
    pub game_mode: String,
    pub min: String,
    pub max: String,
    pub system_type: String,
    pub vacant_only: bool,
    pub exclude_locked: bool,
    pub exclude_non_host_pid: bool,
    pub selection_method: u32,
    pub vacant_participants: u16,
    pub exclude_user_password: bool,
    pub exclude_system_password: bool,
    pub codeword: String,
}
impl Criteria {
    pub fn decode(r: &mut Reader) -> Result<Criteria> {
        let (_v, mut c) = r.structure()?;
        let mut k = Criteria {
            attribs: c.list(|r| r.string())?,
            game_mode: c.string()?,
            min: c.string()?,
            max: c.string()?,
            system_type: c.string()?,
            vacant_only: c.bool()?,
            exclude_locked: c.bool()?,
            exclude_non_host_pid: c.bool()?,
            selection_method: c.u32()?,
            ..Default::default()
        };
        // later revisions; a shorter structure just ends here
        let mut more = || -> Result<()> {
            k.vacant_participants = c.u16()?;
            c.structure()?; // MatchmakeParam
            k.exclude_user_password = c.bool()?;
            k.exclude_system_password = c.bool()?;
            c.u32()?; // refer gid
            k.codeword = c.string()?;
            Ok(())
        };
        let _ = more();
        Ok(k)
    }
    fn number_matches(spec: &str, v: u32) -> bool {
        let spec = spec.trim();
        if spec.is_empty() {
            return true;
        }
        if let Some((a, b)) = spec.split_once(',') {
            let lo = a.trim().parse::<u32>().unwrap_or(0);
            let hi = b.trim().parse::<u32>().unwrap_or(u32::MAX);
            return v >= lo && v <= hi;
        }
        spec.parse::<u32>().map(|x| x == v).unwrap_or(false)
    }
    /// Whether `s` (with `free` places left) satisfies this criteria for `joining` more players.
    pub fn matches(&self, s: &MatchmakeSession, free: u32, joining: u32) -> bool {
        let vacant = if self.vacant_participants > 0 { self.vacant_participants as u32 } else { joining.max(1) };
        if !s.open || free < vacant.max(joining) {
            return false;
        }
        if !Criteria::number_matches(&self.game_mode, s.game_mode) || !Criteria::number_matches(&self.system_type, s.system_type) {
            return false;
        }
        if !Criteria::number_matches(&self.min, s.g.min as u32) || !Criteria::number_matches(&self.max, s.g.max as u32) {
            return false;
        }
        if (self.exclude_user_password && s.user_password_enabled) || (self.exclude_system_password && s.system_password_enabled) {
            return false;
        }
        if !self.codeword.is_empty() && self.codeword != s.codeword {
            return false;
        }
        self.attribs.iter().enumerate().all(|(i, spec)| spec.trim().is_empty() || s.attribs.get(i).is_some_and(|v| Criteria::number_matches(spec, *v)))
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Participant {
    pub pid: u64,
    pub cid: u32,
    /// Players this console brings (local multiplayer).
    pub count: u16,
}

#[derive(Clone, Debug)]
pub struct Session {
    pub mm: MatchmakeSession,
    pub participants: Vec<Participant>,
}
impl Session {
    pub fn players(&self) -> u32 {
        self.participants.iter().map(|p| p.count.max(1) as u32).sum()
    }
    pub fn free(&self) -> u32 {
        (self.mm.g.max as u32).saturating_sub(self.players())
    }
    fn sync(&mut self) {
        self.mm.participation_count = self.players();
    }
}

/// What a leave did, for notifications.
#[derive(Debug, PartialEq)]
pub struct Left {
    pub gid: u32,
    pub remaining: Vec<Participant>,
    pub new_owner: Option<u64>,
    pub ended: bool,
}

#[derive(Default)]
pub struct Sessions {
    next_gid: u32,
    pub all: std::collections::BTreeMap<u32, Session>,
}
impl Sessions {
    pub fn new() -> Sessions {
        Sessions { next_gid: 1, all: Default::default() }
    }
    /// Registers `mm` as a new session owned and hosted by `pid` (who joins it with `count` players).
    pub fn create(&mut self, mut mm: MatchmakeSession, pid: u64, cid: u32, count: u16) -> u32 {
        let gid = self.next_gid;
        self.next_gid += 1;
        mm.g.id = gid;
        mm.g.owner = pid;
        mm.g.host = pid;
        if mm.g.max == 0 {
            mm.g.max = 12;
        }
        mm.session_key = random_bytes(32);
        mm.started = DateTime::now();
        let mut s = Session { mm, participants: vec![Participant { pid, cid, count: count.max(1) }] };
        s.sync();
        self.all.insert(gid, s);
        gid
    }
    /// Adds `pid` to session `gid`. Err(()) when it is full, closed or unknown. Joining twice is a no-op.
    pub fn join(&mut self, gid: u32, pid: u64, cid: u32, count: u16) -> std::result::Result<(), ()> {
        let s = self.all.get_mut(&gid).ok_or(())?;
        if s.participants.iter().any(|p| p.pid == pid) {
            return Ok(());
        }
        if s.free() < count.max(1) as u32 {
            return Err(());
        }
        s.participants.push(Participant { pid, cid, count: count.max(1) });
        s.sync();
        Ok(())
    }
    /// The first session matching any of `criteria` for `pid` (not already in it), oldest first.
    pub fn find(&self, criteria: &[Criteria], template: &MatchmakeSession, pid: u64, joining: u16) -> Option<u32> {
        self.all
            .iter()
            .filter(|(_, s)| !s.participants.iter().any(|p| p.pid == pid))
            .find(|(_, s)| {
                if criteria.is_empty() {
                    s.mm.open && s.mm.game_mode == template.game_mode && s.free() >= joining.max(1) as u32
                } else {
                    criteria.iter().any(|c| c.matches(&s.mm, s.free(), joining.max(1) as u32))
                }
            })
            .map(|(g, _)| *g)
    }
    /// Removes `pid` from `gid`; the owner leaving hands ownership (and host) over, or ends the session when nobody is left.
    pub fn leave(&mut self, gid: u32, pid: u64) -> Option<Left> {
        let s = self.all.get_mut(&gid)?;
        let before = s.participants.len();
        s.participants.retain(|p| p.pid != pid);
        if s.participants.len() == before {
            return None;
        }
        s.sync();
        let mut new_owner = None;
        if s.participants.is_empty() {
            self.all.remove(&gid);
            return Some(Left { gid, remaining: vec![], new_owner: None, ended: true });
        }
        if s.mm.g.owner == pid {
            let next = s.participants[0].pid;
            s.mm.g.owner = next;
            new_owner = Some(next);
        }
        if s.mm.g.host == pid {
            s.mm.g.host = s.mm.g.owner;
        }
        Some(Left { gid, remaining: s.participants.clone(), new_owner, ended: false })
    }
    /// Every session `pid` is in.
    pub fn of(&self, pid: u64) -> Vec<u32> {
        self.all.iter().filter(|(_, s)| s.participants.iter().any(|p| p.pid == pid)).map(|(g, _)| *g).collect()
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    pub fn sample(game_mode: u32, max: u16) -> MatchmakeSession {
        MatchmakeSession {
            g: Gathering { max, min: 2, flags: GATHERING_FLAG_MIGRATE_OWNER, description: "race".into(), ..Default::default() },
            g_version: 0,
            version: 0,
            game_mode,
            attribs: vec![1, 5, 0, 0, 0, 0],
            open: true,
            system_type: 2,
            app_buffer: vec![9; 8],
            param: (0, vec![0, 0, 0, 0]),
            started: DateTime(0),
            ..Default::default()
        }
    }

    #[test]
    fn session_roundtrip() {
        let mut s = sample(3, 12);
        s.tail = vec![0xee];
        let b = s.to_bytes();
        let d = MatchmakeSession::decode(&mut Reader::new(&b)).unwrap();
        assert_eq!(d, s);
        let any = s.to_any();
        let (name, obj) = Reader::new(&any).any().unwrap();
        assert_eq!((name.as_str(), obj), ("MatchmakeSession", &b[..]));
    }

    fn criteria(game_mode: &str, attribs: &[&str]) -> Criteria {
        Criteria { game_mode: game_mode.into(), attribs: attribs.iter().map(|s| s.to_string()).collect(), vacant_only: true, ..Default::default() }
    }

    #[test]
    fn criteria_decode() {
        let mut w = Writer::new();
        w.structure(0, |w| {
            w.list(&["1", "0,10"], |w, s| {
                w.string(s);
            });
            w.string("3").string("").string("").string("2").bool(true).bool(false).bool(false).u32(1).u16(2);
            w.structure(0, |w| {
                w.u32(0);
            });
            w.bool(false).bool(true).u32(0).string("").structure(0, |w| {
                w.u32(0).u32(1);
            });
        });
        let c = Criteria::decode(&mut Reader::new(&w.buf)).unwrap();
        assert_eq!((c.attribs.len(), c.game_mode.as_str(), c.system_type.as_str(), c.vacant_participants, c.exclude_system_password), (2, "3", "2", 2, true));
    }

    #[test]
    fn matching_join_leave() {
        let mut t = Sessions::new();
        let gid = t.create(sample(3, 2), 100, 1, 1);
        assert_eq!(t.all[&gid].mm.g.owner, 100);
        assert_eq!(t.all[&gid].mm.session_key.len(), 32);
        // a second player looking for mode 3 with attribute 1 in 0..10 finds it; mode 4 does not
        assert_eq!(t.find(&[criteria("3", &["", "0,10"])], &sample(3, 2), 200, 1), Some(gid));
        assert_eq!(t.find(&[criteria("4", &[])], &sample(4, 2), 200, 1), None);
        assert_eq!(t.find(&[criteria("3", &["", "6,10"])], &sample(3, 2), 200, 1), None);
        // the owner does not match their own session
        assert_eq!(t.find(&[criteria("3", &[])], &sample(3, 2), 100, 1), None);
        // without criteria, the game mode decides
        assert_eq!(t.find(&[], &sample(3, 2), 200, 1), Some(gid));
        t.join(gid, 200, 2, 1).unwrap();
        assert_eq!(t.all[&gid].mm.participation_count, 2);
        // full now
        assert!(t.join(gid, 300, 3, 1).is_err());
        assert_eq!(t.find(&[criteria("3", &[])], &sample(3, 2), 300, 1), None);
        assert_eq!(t.of(200), vec![gid]);
        // the owner leaves: ownership moves to 200
        assert_eq!(t.leave(gid, 100), Some(Left { gid, remaining: vec![Participant { pid: 200, cid: 2, count: 1 }], new_owner: Some(200), ended: false }));
        assert_eq!(t.all[&gid].mm.g.host, 200);
        assert!(t.leave(gid, 100).is_none());
        assert!(t.leave(gid, 200).unwrap().ended);
        assert!(t.all.is_empty());
    }
}
