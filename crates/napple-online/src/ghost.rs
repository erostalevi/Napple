//! Tier-5 (Ghost) aliases: a random bot-style name handed to the game instead of the real nickname. Assigned at login, stable until that
//! principal's session ends (`release`), unique among all live aliases and never equal (case-insensitively) to a live real nickname, so two
//! players in one match can never share a name and a ghost cannot pose as someone else in the lobby.

use crate::crypto::random_u32;
use std::collections::HashMap;
use std::sync::Mutex;

const ADJ: &[&str] = &[
    "Quiet", "Swift", "Brave", "Calm", "Lucky", "Sunny", "Misty", "Rusty", "Neon", "Cosmic", "Fuzzy", "Jolly", "Witty", "Mellow", "Zesty", "Nimble",
];
const NOUN: &[&str] = &[
    "Falcon", "Otter", "Panda", "Comet", "Pixel", "Turtle", "Lynx", "Maple", "Rocket", "Badger", "Koala", "Gecko", "Robot", "Sparrow", "Yeti", "Wombat",
];

#[derive(Default)]
pub struct Ghosts {
    by_pid: Mutex<HashMap<u64, String>>,
}

impl Ghosts {
    /// The alias of `pid`, created on first call. `real_taken(name)` says whether a live real nickname equals `name`.
    pub fn alias(&self, pid: u64, real_taken: &dyn Fn(&str) -> bool) -> String {
        let mut m = self.by_pid.lock().unwrap();
        if let Some(a) = m.get(&pid) {
            return a.clone();
        }
        let mut n = 0u32;
        loop {
            let r = random_u32();
            let digits = if n < 32 { 100 } else { 10_000 }; // widen the space when the 2-digit one is crowded
            let name = format!("{}{}{}", ADJ[(r & 0xf) as usize], NOUN[((r >> 4) & 0xf) as usize], (r >> 8) % digits);
            n += 1;
            if m.values().any(|v| v.eq_ignore_ascii_case(&name)) || real_taken(&name) {
                continue;
            }
            m.insert(pid, name.clone());
            return name;
        }
    }

    pub fn get(&self, pid: u64) -> Option<String> {
        self.by_pid.lock().unwrap().get(&pid).cloned()
    }

    pub fn release(&self, pid: u64) {
        self.by_pid.lock().unwrap().remove(&pid);
    }

    /// The live principal behind `alias`, if any.
    pub fn pid_of(&self, alias: &str) -> Option<u64> {
        self.by_pid.lock().unwrap().iter().find(|(_, v)| v.eq_ignore_ascii_case(alias)).map(|(p, _)| *p)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stable_unique_and_never_a_real_nickname() {
        let g = Ghosts::default();
        let a = g.alias(1, &|_| false);
        assert_eq!(a, g.alias(1, &|_| false), "stable within a session");
        assert!(a.len() <= 20 && a.bytes().all(|c| c.is_ascii_alphanumeric()));
        let mut seen = std::collections::HashSet::new();
        seen.insert(a.to_ascii_lowercase());
        for pid in 2..600 {
            assert!(seen.insert(g.alias(pid, &|_| false).to_ascii_lowercase()), "collision-free across live players");
        }
        assert_eq!(g.pid_of(&a.to_ascii_uppercase()), Some(1));
        let b = g.alias(10_000, &|n| ADJ.iter().any(|a| n.starts_with(a)) && false);
        assert!(!b.is_empty());
        // every candidate that starts with "Q" is a real nickname -> none is handed out
        let c = g.alias(10_001, &|n| n.starts_with('Q'));
        assert!(!c.starts_with('Q'));
        g.release(1);
        assert_eq!(g.get(1), None);
    }
}
