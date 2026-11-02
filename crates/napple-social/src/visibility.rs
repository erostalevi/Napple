//! The five visibility tiers of orchestrations/ONLINE-CONTRACT.md as pure functions.
//!
//! This is a private copy of the rules that `crates/napple-visibility` (item account-profiles) will own; the names
//! (`Tier`, `Kind`, `can_view`) match so swapping is a one-line `use` change.

/// Per-profile visibility tier (1 Public .. 5 Ghost).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Tier {
    Public = 1,
    FriendsSearchable = 2,
    FriendsHidden = 3,
    Private = 4,
    Ghost = 5,
}

impl Tier {
    /// Unknown values fall back to the default (Public is the account default; clamping to Ghost would hide people on a bad row).
    pub fn from_i64(n: i64) -> Tier {
        match n {
            2 => Tier::FriendsSearchable,
            3 => Tier::FriendsHidden,
            4 => Tier::Private,
            5 => Tier::Ghost,
            _ => Tier::Public,
        }
    }
    pub fn as_i64(self) -> i64 {
        self as i64
    }
}

/// What the viewer wants to see or do.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// Appear in search results / be found by friend code.
    Search,
    /// Receive a friend request (from the app or from a game).
    FriendRequest,
    /// Nickname, icon and photo.
    Card,
    /// Presence, stats, bio and links.
    Details,
    /// Appear in a shared lobby with the real name and photo (false: bot alias).
    LobbyReal,
    /// Appear in a recent-players list.
    Recent,
}

/// How the viewer relates to the target. Block checks are made by the caller (`blocked` wins over everything).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Viewer {
    pub is_self: bool,
    pub is_friend: bool,
    /// Either side blocked the other.
    pub blocked: bool,
    /// Both are in a live shared lobby / recent encounter vouched for by napple-online.
    pub shared_lobby: bool,
}

pub fn can_view(v: Viewer, target: Tier, kind: Kind) -> bool {
    if v.is_self {
        // Your own data is always yours; you are never in your own search or request inbox.
        return !matches!(kind, Kind::Search | Kind::FriendRequest);
    }
    if v.blocked {
        return false;
    }
    let ghost = target == Tier::Ghost;
    match kind {
        Kind::Search => matches!(target, Tier::Public | Tier::FriendsSearchable),
        Kind::FriendRequest => match target {
            Tier::Public | Tier::FriendsSearchable => true,
            Tier::FriendsHidden => v.shared_lobby,
            Tier::Private | Tier::Ghost => false,
        },
        Kind::Card => match target {
            Tier::Public | Tier::FriendsSearchable => true,
            Tier::FriendsHidden | Tier::Private => v.is_friend,
            Tier::Ghost => false,
        },
        Kind::Details => match target {
            Tier::Public => true,
            Tier::FriendsSearchable | Tier::FriendsHidden | Tier::Private => v.is_friend,
            Tier::Ghost => false,
        },
        Kind::LobbyReal | Kind::Recent => !ghost,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TIERS: [Tier; 5] = [
        Tier::Public,
        Tier::FriendsSearchable,
        Tier::FriendsHidden,
        Tier::Private,
        Tier::Ghost,
    ];
    const STRANGER: Viewer = Viewer {
        is_self: false,
        is_friend: false,
        blocked: false,
        shared_lobby: false,
    };

    fn row(v: Viewer, k: Kind) -> [bool; 5] {
        TIERS.map(|t| can_view(v, t, k))
    }

    #[test]
    fn stranger_columns_match_the_contract_table() {
        assert_eq!(
            row(STRANGER, Kind::Search),
            [true, true, false, false, false]
        );
        assert_eq!(
            row(STRANGER, Kind::FriendRequest),
            [true, true, false, false, false]
        );
        assert_eq!(row(STRANGER, Kind::Card), [true, true, false, false, false]);
        assert_eq!(
            row(STRANGER, Kind::Details),
            [true, false, false, false, false]
        );
        assert_eq!(
            row(STRANGER, Kind::LobbyReal),
            [true, true, true, true, false]
        );
        assert_eq!(row(STRANGER, Kind::Recent), [true, true, true, true, false]);
    }

    #[test]
    fn shared_lobby_only_opens_tier_three_requests() {
        let v = Viewer {
            shared_lobby: true,
            ..STRANGER
        };
        assert_eq!(
            row(v, Kind::FriendRequest),
            [true, true, true, false, false]
        );
        // It does not open search or details.
        assert_eq!(row(v, Kind::Search), row(STRANGER, Kind::Search));
        assert_eq!(row(v, Kind::Details), row(STRANGER, Kind::Details));
    }

    #[test]
    fn friends_see_everything_but_ghosts() {
        let v = Viewer {
            is_friend: true,
            ..STRANGER
        };
        assert_eq!(row(v, Kind::Card), [true, true, true, true, false]);
        assert_eq!(row(v, Kind::Details), [true, true, true, true, false]);
    }

    #[test]
    fn blocked_sees_nothing_and_self_sees_all() {
        let b = Viewer {
            blocked: true,
            is_friend: true,
            shared_lobby: true,
            ..STRANGER
        };
        for k in [
            Kind::Search,
            Kind::FriendRequest,
            Kind::Card,
            Kind::Details,
            Kind::LobbyReal,
            Kind::Recent,
        ] {
            assert_eq!(row(b, k), [false; 5], "{k:?}");
        }
        let me = Viewer {
            is_self: true,
            ..STRANGER
        };
        assert_eq!(row(me, Kind::Details), [true; 5]);
        assert_eq!(row(me, Kind::Card), [true; 5]);
        assert_eq!(row(me, Kind::Search), [false; 5]);
    }

    #[test]
    fn tier_numbers_round_trip() {
        for (i, t) in TIERS.iter().enumerate() {
            assert_eq!(Tier::from_i64(i as i64 + 1), *t);
            assert_eq!(t.as_i64(), i as i64 + 1);
        }
        assert_eq!(Tier::from_i64(0), Tier::Public);
        assert_eq!(Tier::from_i64(99), Tier::Public);
    }
}
