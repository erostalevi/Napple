//! The five visibility tiers of `docs/guides/napple-social.md`. The rules live in `crates/napple-visibility` (shared with
//! napple-account); this module keeps napple-social's small API (`Tier`, `Kind`, `Viewer`, `can_view`) and delegates to it.

use napple_visibility as nv;

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
        nv::Tier::from_i64(n).map(Tier::from).unwrap_or(Tier::Public)
    }
    pub fn as_i64(self) -> i64 {
        self as i64
    }
}

impl From<nv::Tier> for Tier {
    fn from(t: nv::Tier) -> Tier {
        match t {
            nv::Tier::Public => Tier::Public,
            nv::Tier::FriendsSearchable => Tier::FriendsSearchable,
            nv::Tier::FriendsHidden => Tier::FriendsHidden,
            nv::Tier::Private => Tier::Private,
            nv::Tier::Ghost => Tier::Ghost,
        }
    }
}

impl From<Tier> for nv::Tier {
    fn from(t: Tier) -> nv::Tier {
        match t {
            Tier::Public => nv::Tier::Public,
            Tier::FriendsSearchable => nv::Tier::FriendsSearchable,
            Tier::FriendsHidden => nv::Tier::FriendsHidden,
            Tier::Private => nv::Tier::Private,
            Tier::Ghost => nv::Tier::Ghost,
        }
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

impl From<Kind> for nv::ViewKind {
    fn from(k: Kind) -> nv::ViewKind {
        match k {
            Kind::Search => nv::ViewKind::Search,
            Kind::FriendRequest => nv::ViewKind::FriendRequest,
            Kind::Card => nv::ViewKind::NicknameAndPhoto,
            Kind::Details => nv::ViewKind::Profile,
            Kind::LobbyReal => nv::ViewKind::LobbyIdentity,
            Kind::Recent => nv::ViewKind::RecentPlayers,
        }
    }
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
    let viewer = nv::Viewer {
        profile_id: None,
        same_account: false,
        is_friend: v.is_friend,
        shared_lobby: v.shared_lobby,
        blocked: v.blocked,
    };
    let target = nv::TargetProfile {
        profile_id: String::new(),
        tier: target.into(),
    };
    nv::can_view(&viewer, &target, kind.into())
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
