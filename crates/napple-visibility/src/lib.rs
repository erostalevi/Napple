//! Visibility tiers (`orchestrations/ONLINE-CONTRACT.md`, "Visibility tiers"). The rules live here once; napple-account
//! (photo/profile data) and napple-social (search, requests, presence, stats) both call [`can_view`].
//!
//! | Tier | Search | Friend req | Profile/presence/stats | Photo+nickname | Lobby real name | Recent | Game friend req |
//! |---|---|---|---|---|---|---|---|
//! | 1 Public | all | anyone | all | all | yes | yes | yes |
//! | 2 Friends, searchable | all | anyone | friends | all | yes | yes | yes |
//! | 3 Friends, hidden | none | shared lobby | friends | friends | yes | yes | shared lobby |
//! | 4 Private | none | none | friends | friends | yes | yes | no |
//! | 5 Ghost | none | none | none | none | alias | none | no |
//!
//! A block hides the target from the blocked viewer at every tier. The owner (and the owner's other profiles) always
//! sees everything of their own, ghost included.

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Tier {
    Public = 1,
    FriendsSearchable = 2,
    FriendsHidden = 3,
    Private = 4,
    Ghost = 5,
}

impl Tier {
    pub const DEFAULT: Tier = Tier::Public;

    pub fn from_i64(v: i64) -> Option<Tier> {
        match v {
            1 => Some(Tier::Public),
            2 => Some(Tier::FriendsSearchable),
            3 => Some(Tier::FriendsHidden),
            4 => Some(Tier::Private),
            5 => Some(Tier::Ghost),
            _ => None,
        }
    }

    pub fn as_i64(self) -> i64 {
        self as i64
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewKind {
    /// Found by nickname search or friend code.
    Search,
    /// Sending a (website/app) friend request.
    FriendRequest,
    /// Nickname and photo (what `GET /profiles/{id}/photo` serves).
    NicknameAndPhoto,
    /// Bio, links, icon, full profile.
    Profile,
    Presence,
    Stats,
    /// Whether the real nickname/photo is shown in a shared lobby (false: bot alias, no profile).
    LobbyIdentity,
    /// Appearing in the other side's recent-players list.
    RecentPlayers,
    /// A friend request sent from inside a game.
    GameFriendRequest,
}

/// The profile being looked at.
#[derive(Clone, Debug)]
pub struct TargetProfile {
    pub profile_id: String,
    pub tier: Tier,
}

/// Who is looking, resolved by the caller from its own data (friendships, blocks, lobbies).
#[derive(Clone, Debug, Default)]
pub struct Viewer {
    /// None = anonymous (never a friend).
    pub profile_id: Option<String>,
    /// The viewer signs in with the same account as the target (own other profile).
    pub same_account: bool,
    pub is_friend: bool,
    /// Both are in the same lobby/match right now.
    pub shared_lobby: bool,
    /// The target has blocked the viewer (or the viewer is otherwise to be hidden from).
    pub blocked: bool,
}

pub fn can_view(viewer: &Viewer, target: &TargetProfile, kind: ViewKind) -> bool {
    if viewer.same_account || viewer.profile_id.as_deref() == Some(target.profile_id.as_str()) {
        return true;
    }
    if viewer.blocked {
        return false;
    }
    let friend = viewer.is_friend;
    use Tier::*;
    use ViewKind::*;
    match (kind, target.tier) {
        (Search, Public | FriendsSearchable) => true,
        (Search, _) => false,
        (FriendRequest, Public | FriendsSearchable) => true,
        (FriendRequest, FriendsHidden) => viewer.shared_lobby,
        (FriendRequest, _) => false,
        (GameFriendRequest, Public | FriendsSearchable) => true,
        (GameFriendRequest, FriendsHidden) => viewer.shared_lobby,
        (GameFriendRequest, _) => false,
        (Profile | Presence | Stats, Public) => true,
        (Profile | Presence | Stats, FriendsSearchable | FriendsHidden | Private) => friend,
        (Profile | Presence | Stats, Ghost) => false,
        (NicknameAndPhoto, Public | FriendsSearchable) => true,
        (NicknameAndPhoto, FriendsHidden | Private) => friend,
        (NicknameAndPhoto, Ghost) => false,
        (LobbyIdentity, Ghost) => false,
        (LobbyIdentity, _) => true,
        (RecentPlayers, Ghost) => false,
        (RecentPlayers, _) => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ALL: [Tier; 5] = [
        Tier::Public,
        Tier::FriendsSearchable,
        Tier::FriendsHidden,
        Tier::Private,
        Tier::Ghost,
    ];
    const KINDS: [ViewKind; 9] = [
        ViewKind::Search,
        ViewKind::FriendRequest,
        ViewKind::NicknameAndPhoto,
        ViewKind::Profile,
        ViewKind::Presence,
        ViewKind::Stats,
        ViewKind::LobbyIdentity,
        ViewKind::RecentPlayers,
        ViewKind::GameFriendRequest,
    ];

    fn t(tier: Tier) -> TargetProfile {
        TargetProfile {
            profile_id: "target".into(),
            tier,
        }
    }
    fn stranger() -> Viewer {
        Viewer {
            profile_id: Some("v".into()),
            ..Default::default()
        }
    }
    fn friend() -> Viewer {
        Viewer {
            is_friend: true,
            ..stranger()
        }
    }
    fn lobby() -> Viewer {
        Viewer {
            shared_lobby: true,
            ..stranger()
        }
    }

    #[test]
    fn tier_numbers_round_trip() {
        for t in ALL {
            assert_eq!(Tier::from_i64(t.as_i64()), Some(t));
        }
        assert_eq!(Tier::from_i64(0), None);
        assert_eq!(Tier::from_i64(6), None);
        assert_eq!(Tier::DEFAULT, Tier::Public);
    }

    // Columns: Search, FriendRequest, NicknameAndPhoto, Profile, Presence, Stats, Lobby, Recent, GameFR
    fn row(v: &Viewer, tier: Tier) -> [bool; 9] {
        let tp = t(tier);
        KINDS.map(|k| can_view(v, &tp, k))
    }

    #[test]
    fn stranger_table() {
        let f = false;
        let y = true;
        assert_eq!(row(&stranger(), Tier::Public), [y, y, y, y, y, y, y, y, y]);
        assert_eq!(
            row(&stranger(), Tier::FriendsSearchable),
            [y, y, y, f, f, f, y, y, y]
        );
        assert_eq!(
            row(&stranger(), Tier::FriendsHidden),
            [f, f, f, f, f, f, y, y, f]
        );
        assert_eq!(row(&stranger(), Tier::Private), [f, f, f, f, f, f, y, y, f]);
        assert_eq!(row(&stranger(), Tier::Ghost), [f, f, f, f, f, f, f, f, f]);
    }

    #[test]
    fn friend_table() {
        let f = false;
        let y = true;
        // Friends see profile/presence/stats/photo for tiers 1-4; search/requests still follow the tier.
        assert_eq!(row(&friend(), Tier::Public), [y, y, y, y, y, y, y, y, y]);
        assert_eq!(
            row(&friend(), Tier::FriendsSearchable),
            [y, y, y, y, y, y, y, y, y]
        );
        assert_eq!(
            row(&friend(), Tier::FriendsHidden),
            [f, f, y, y, y, y, y, y, f]
        );
        assert_eq!(row(&friend(), Tier::Private), [f, f, y, y, y, y, y, y, f]);
        // Ghost: friends cannot see you either.
        assert_eq!(row(&friend(), Tier::Ghost), [f, f, f, f, f, f, f, f, f]);
    }

    #[test]
    fn shared_lobby_opens_requests_only_for_tier_3() {
        for tier in ALL {
            let want = matches!(
                tier,
                Tier::Public | Tier::FriendsSearchable | Tier::FriendsHidden
            );
            assert_eq!(
                can_view(&lobby(), &t(tier), ViewKind::FriendRequest),
                want,
                "{tier:?}"
            );
            assert_eq!(
                can_view(&lobby(), &t(tier), ViewKind::GameFriendRequest),
                want,
                "{tier:?}"
            );
        }
    }

    #[test]
    fn blocked_sees_nothing_at_any_tier() {
        for tier in ALL {
            for v in [stranger(), friend(), lobby()] {
                let v = Viewer { blocked: true, ..v };
                assert_eq!(row(&v, tier), [false; 9], "{tier:?}");
            }
        }
    }

    #[test]
    fn owner_sees_everything_even_ghost() {
        let own = Viewer {
            profile_id: Some("target".into()),
            ..Default::default()
        };
        let sib = Viewer {
            same_account: true,
            ..Default::default()
        };
        for tier in ALL {
            assert_eq!(row(&own, tier), [true; 9]);
            assert_eq!(row(&sib, tier), [true; 9]);
        }
    }

    #[test]
    fn anonymous_is_a_stranger() {
        assert_eq!(
            row(&Viewer::default(), Tier::FriendsSearchable),
            row(&stranger(), Tier::FriendsSearchable)
        );
    }
}
