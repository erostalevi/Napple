//! Visibility tiers x actions (ONLINE-CONTRACT.md): search, request, profile card, presence, stats, lobby view,
//! recent players, blocks, reports, rate limits.
mod common;

use axum::http::StatusCode;
use common::*;
use serde_json::{Value, json};

const TITLE: &str = "0100152000022000";

async fn reg(env: &Env, u: &User) {
    env.get(u, "/me").await;
}

/// A registered stranger-to-everyone profile of the given tier.
async fn target(env: &Env, name: &str, tier: i64) -> User {
    let u = env.user_tier(name, tier);
    reg(env, &u).await;
    u
}

async fn search(env: &Env, who: &User, q: &str) -> Vec<String> {
    let (s, v) = env.call(who, "GET", &format!("/search?q={q}"), None).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    v["results"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r["profile"]["profile_id"].as_str().unwrap().to_string())
        .collect()
}

async fn go_playing(env: &Env, u: &User) {
    let (s, v) = env
        .call(
            u,
            "PUT",
            "/presence",
            Some(json!({"status": "playing", "title_id": TITLE, "title_name": "Kart", "mode": "Race", "joinable_token": "tok-1"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
}

fn lobby_body(id: &str, members: &[&User]) -> Value {
    json!({"lobby_id": id, "members": members.iter().map(|m| json!({"profile_id": m.profile_id})).collect::<Vec<_>>()})
}

// ------------------------------------------------------------------ search

#[tokio::test]
async fn search_by_nickname_prefix_and_code_follows_tiers() {
    let env = Env::start().await;
    let me = env.user("Seeker");
    let mut found = Vec::new();
    for t in 1..=5 {
        let u = target(&env, &format!("Zed{t}"), t).await;
        found.push((t, u));
    }
    let hits = search(&env, &me, "zed").await;
    for (t, u) in &found {
        assert_eq!(hits.contains(&u.profile_id), *t <= 2, "tier {t} by prefix");
        let code = env.code_of(u).await;
        let by_code = search(&env, &me, &code).await;
        assert_eq!(by_code.contains(&u.profile_id), *t <= 2, "tier {t} by code");
    }
}

#[tokio::test]
async fn search_is_case_insensitive_excludes_self_and_validates() {
    let env = Env::start().await;
    let me = target(&env, "Zedmine", 1).await;
    let other = target(&env, "ZEDother", 1).await;
    assert_eq!(
        search(&env, &me, "zed").await,
        vec![other.profile_id.clone()]
    );
    for q in ["", "z", "%25", "_"] {
        // Too short or wildcard-only queries match nothing (and never everything).
        let r = search(&env, &me, q).await;
        assert!(r.is_empty(), "q={q:?} -> {r:?}");
    }
}

#[tokio::test]
async fn search_tier_change_applies_immediately_and_keeps_friends() {
    let env = Env::start().await;
    let me = env.user("Seeker");
    let t = target(&env, "Chameleon", 1).await;
    assert_eq!(search(&env, &me, "chamel").await.len(), 1);
    env.befriend(&me, &t).await;
    env.set_tier(&t, 3);
    assert!(search(&env, &me, "chamel").await.is_empty());
    // Still friends.
    assert_eq!(env.get(&me, "/friends").await["count"], 1);
}

#[tokio::test]
async fn search_hides_blocks_both_ways() {
    let env = Env::start().await;
    let a = target(&env, "Blockera", 1).await;
    let b = target(&env, "Blockerb", 1).await;
    assert_eq!(search(&env, &a, "blockerb").await.len(), 1);
    env.call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert!(search(&env, &a, "blockerb").await.is_empty());
    assert!(search(&env, &b, "blockera").await.is_empty());
}

#[tokio::test]
async fn search_is_rate_limited() {
    let env = Env::start_with(|c| c.searches_per_min = 4).await;
    let me = env.user("Hammer");
    let mut limited = false;
    for _ in 0..10 {
        let (s, _) = env.call(&me, "GET", "/search?q=anything", None).await;
        if s == StatusCode::TOO_MANY_REQUESTS {
            limited = true;
            break;
        }
    }
    assert!(limited);
}

// ------------------------------------------------------------------ friend requests

#[tokio::test]
async fn request_by_code_follows_tiers() {
    let env = Env::start().await;
    let me = env.user("Asker");
    for t in 1..=5 {
        let u = target(&env, &format!("Tgt{t}"), t).await;
        let code = env.code_of(&u).await;
        let (s, v) = env
            .call(
                &me,
                "POST",
                "/friend-requests",
                Some(json!({"friend_code": code})),
            )
            .await;
        if t <= 2 {
            assert_eq!(s, StatusCode::CREATED, "tier {t}: {v}");
        } else {
            assert_eq!(s, StatusCode::NOT_FOUND, "tier {t}: {v}");
            assert_eq!(v["error"]["code"], "code_not_found");
        }
    }
}

#[tokio::test]
async fn request_by_profile_id_needs_a_shared_lobby_for_tier_three_only() {
    let env = Env::start().await;
    let me = target(&env, "Lobbyist", 1).await;
    let mut targets = Vec::new();
    for t in 1..=5 {
        targets.push(target(&env, &format!("Lob{t}"), t).await);
    }
    // No lobby: tiers 1-2 fine, 3-5 not found.
    for (i, u) in targets.iter().enumerate() {
        let t = i + 1;
        let (s, v) = env
            .call(
                &me,
                "POST",
                "/friend-requests",
                Some(json!({"profile_id": u.profile_id})),
            )
            .await;
        assert_eq!(
            s == StatusCode::CREATED,
            t <= 2,
            "no lobby, tier {t}: {s} {v}"
        );
        if t > 2 {
            assert_eq!(v["error"]["code"], "profile_not_found");
        }
    }
    // Same lobby for everybody (the ghost's membership is registered too).
    let mut members = vec![&me];
    members.extend(targets.iter());
    let (s, v) = env
        .service(
            "POST",
            "/internal/lobbies",
            Some(lobby_body("lobby-1", &members)),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    for (i, u) in targets.iter().enumerate().skip(2) {
        let t = i + 1;
        let (s, v) = env
            .call(
                &me,
                "POST",
                "/friend-requests",
                Some(json!({"profile_id": u.profile_id, "lobby_id": "lobby-1"})),
            )
            .await;
        assert_eq!(s == StatusCode::CREATED, t == 3, "lobby, tier {t}: {s} {v}");
    }
}

#[tokio::test]
async fn lobby_proof_must_cover_both_players_and_expire() {
    let env = Env::start().await;
    let me = target(&env, "Me3", 1).await;
    let hidden = target(&env, "Hidden3", 3).await;
    let third = target(&env, "Third", 1).await;
    env.service(
        "POST",
        "/internal/lobbies",
        Some(lobby_body("only-hidden", &[&hidden, &third])),
    )
    .await;
    let req = json!({"profile_id": hidden.profile_id, "lobby_id": "only-hidden"});
    let (s, _) = env.call(&me, "POST", "/friend-requests", Some(req)).await;
    assert_eq!(s, StatusCode::NOT_FOUND, "caller is not in that lobby");

    let (s, _) = env
        .service("POST", "/internal/lobbies", Some(json!({"lobby_id": "short", "ttl_secs": 60, "members": [{"profile_id": me.profile_id}, {"profile_id": hidden.profile_id}]})))
        .await;
    assert_eq!(s, StatusCode::OK);
    env.clock.advance_secs(61);
    let req = json!({"profile_id": hidden.profile_id, "lobby_id": "short"});
    let (s, _) = env.call(&me, "POST", "/friend-requests", Some(req)).await;
    assert_eq!(s, StatusCode::NOT_FOUND, "lobby expired");
}

#[tokio::test]
async fn blocked_requester_cannot_reach_a_hidden_target_even_in_a_lobby() {
    let env = Env::start().await;
    let me = target(&env, "Pest", 1).await;
    let hidden = target(&env, "Shy", 3).await;
    env.service(
        "POST",
        "/internal/lobbies",
        Some(lobby_body("l", &[&me, &hidden])),
    )
    .await;
    env.call(&hidden, "PUT", &format!("/blocks/{}", me.profile_id), None)
        .await;
    let (s, _) = env
        .call(
            &me,
            "POST",
            "/friend-requests",
            Some(json!({"profile_id": hidden.profile_id, "lobby_id": "l"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn request_needs_exactly_one_selector() {
    let env = Env::start().await;
    let me = env.user("Picky");
    let (s, v) = env
        .call(&me, "POST", "/friend-requests", Some(json!({})))
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "{v}");
    let (s, v) = env
        .call(
            &me,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": "NP-0000-0000-0000", "profile_id": uuid7()})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "{v}");
}

// ------------------------------------------------------------------ public view, presence

#[tokio::test]
async fn public_view_card_and_details_per_tier() {
    let env = Env::start().await;
    let stranger = env.user("Stranger");
    let friend = env.user("Friendly");
    let mut ts = Vec::new();
    for t in 1..=5 {
        let u = target(&env, &format!("View{t}"), 1).await;
        env.befriend(&friend, &u).await;
        env.set_tier(&u, t);
        go_playing(&env, &u).await;
        ts.push(u);
    }
    for (i, u) in ts.iter().enumerate() {
        let t = i + 1;
        let path = format!("/profiles/{}/public", u.profile_id);
        // Stranger.
        let (s, v) = env.call(&stranger, "GET", &path, None).await;
        match t {
            1 => {
                assert_eq!(s, StatusCode::OK, "{v}");
                assert_eq!(v["profile"]["nickname"], format!("View{t}"));
                assert_eq!(v["presence"]["status"], "playing");
            }
            2 => {
                assert_eq!(s, StatusCode::OK, "{v}");
                assert_eq!(v["profile"]["nickname"], format!("View{t}"));
                assert!(
                    v.get("presence").is_none(),
                    "tier 2 hides presence from strangers: {v}"
                );
            }
            _ => assert_eq!(s, StatusCode::NOT_FOUND, "tier {t}: {v}"),
        }
        // Friend.
        let (s, v) = env.call(&friend, "GET", &path, None).await;
        if t <= 4 {
            assert_eq!(s, StatusCode::OK, "friend tier {t}: {v}");
            assert_eq!(v["presence"]["status"], "playing");
            assert_eq!(v["friend_status"], "friend");
        } else {
            assert_eq!(s, StatusCode::NOT_FOUND, "ghost hides from friends: {v}");
        }
        // Self.
        let (s, v) = env.call(u, "GET", &path, None).await;
        assert_eq!(s, StatusCode::OK, "{v}");
        assert_eq!(v["friend_status"], "self");
    }
}

#[tokio::test]
async fn public_view_hides_blocks_both_ways_and_unknown_ids() {
    let env = Env::start().await;
    let a = target(&env, "Pa", 1).await;
    let b = target(&env, "Pb", 1).await;
    env.call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    for (x, y) in [(&a, &b), (&b, &a)] {
        let (s, _) = env
            .call(
                x,
                "GET",
                &format!("/profiles/{}/public", y.profile_id),
                None,
            )
            .await;
        assert_eq!(s, StatusCode::NOT_FOUND);
    }
    let (s, _) = env
        .call(&a, "GET", &format!("/profiles/{}/public", uuid7()), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = env
        .call(&a, "GET", "/profiles/not-a-uuid/public", None)
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn friends_do_not_see_ghost_presence_or_events() {
    let env = Env::start().await;
    let friend = env.user("Watcher");
    let ghost = target(&env, "Spooky", 1).await;
    env.befriend(&friend, &ghost).await;
    let mut ws = env.ws(&friend).await;
    env.set_tier(&ghost, 5);
    go_playing(&env, &ghost).await;
    no_event(&mut ws, "presence", 300).await;
    let f = env.get(&friend, "/friends").await;
    assert_eq!(f["friends"][0]["presence"]["status"], "offline");
    assert!(f["friends"][0]["presence"].get("title_id").is_none());
    // Ghost still sees its own real state.
    assert_eq!(
        env.get(&ghost, "/presence").await["presence"]["status"],
        "playing"
    );
}

#[tokio::test]
async fn presence_reserved_names_and_joinable_token_alias() {
    let env = Env::start().await;
    let u = env.user("Presenter");
    for (field, val) in [("mode", "Napple Official"), ("title_name", "nap-room")] {
        let mut b = json!({"status": "playing", "title_id": TITLE});
        b[field] = json!(val);
        let (s, v) = env.call(&u, "PUT", "/presence", Some(b)).await;
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY, "{field}: {v}");
        assert_eq!(v["error"]["code"], "reserved_name");
    }
    go_playing(&env, &u).await;
    let p = env.get(&u, "/presence").await;
    assert_eq!(p["presence"]["join_token"], "tok-1");
    assert_eq!(p["presence"]["joinable"], true);
}

// ------------------------------------------------------------------ stats

#[tokio::test]
async fn stats_accumulate_and_are_tier_filtered() {
    let env = Env::start().await;
    let stranger = env.user("Nosy");
    let friend = env.user("Pal");
    for t in 1..=5 {
        let u = target(&env, &format!("Stat{t}"), 1).await;
        env.befriend(&friend, &u).await;
        env.set_tier(&u, t);
        for d in [3, 4] {
            let (s, v) = env
                .call(
                    &u,
                    "POST",
                    "/stats",
                    Some(json!({"title_id": TITLE, "deltas": {"races": d, "wins": 1}})),
                )
                .await;
            assert_eq!(s, StatusCode::OK, "{v}");
        }
        let path = format!("/profiles/{}/stats?title_id={TITLE}", u.profile_id);
        let (s, v) = env.call(&u, "GET", &path, None).await;
        assert_eq!(s, StatusCode::OK, "{v}");
        assert_eq!(v["titles"][TITLE]["races"], 7);
        assert_eq!(v["titles"][TITLE]["wins"], 2);
        let (s, _) = env.call(&stranger, "GET", &path, None).await;
        assert_eq!(s == StatusCode::OK, t == 1, "stranger tier {t}");
        let (s, v) = env.call(&friend, "GET", &path, None).await;
        assert_eq!(s == StatusCode::OK, t <= 4, "friend tier {t}: {v}");
    }
}

#[tokio::test]
async fn stats_validation_and_blocks() {
    let env = Env::start().await;
    let u = env.user("Counter");
    let bad = [
        json!({"title_id": "nope", "deltas": {"a": 1}}),
        json!({"title_id": TITLE, "deltas": {}}),
        json!({"title_id": TITLE, "deltas": {"Bad Key!": 1}}),
        json!({"title_id": TITLE, "deltas": {"a": 5_000_000_000i64}}),
        json!({"title_id": TITLE, "deltas": {"a": 1.5}}),
    ];
    for b in bad {
        let (s, v) = env.call(&u, "POST", "/stats", Some(b.clone())).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{b}: {v}");
    }
    // Too many distinct keys.
    let mut d = serde_json::Map::new();
    for i in 0..40 {
        d.insert(format!("k{i}"), json!(1));
    }
    let (s, _) = env
        .call(
            &u,
            "POST",
            "/stats",
            Some(json!({"title_id": TITLE, "deltas": d})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
    // Blocked either way: stats of a public profile are not readable.
    let other = target(&env, "Public9", 1).await;
    env.call(
        &other,
        "POST",
        "/stats",
        Some(json!({"title_id": TITLE, "deltas": {"a": 1}})),
    )
    .await;
    env.call(&u, "PUT", &format!("/blocks/{}", other.profile_id), None)
        .await;
    for (x, y) in [(&u, &other), (&other, &u)] {
        let (s, _) = env
            .call(x, "GET", &format!("/profiles/{}/stats", y.profile_id), None)
            .await;
        assert_eq!(s, StatusCode::NOT_FOUND);
    }
}

#[tokio::test]
async fn stats_service_write_and_read_all_titles() {
    let env = Env::start().await;
    let u = target(&env, "Racer", 1).await;
    let (s, v) = env
        .service(
            "POST",
            "/internal/stats",
            Some(json!({"profile_id": u.profile_id, "title_id": TITLE, "deltas": {"races": 2}})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let v = env
        .get(&u, &format!("/profiles/{}/stats", u.profile_id))
        .await;
    assert_eq!(v["titles"][TITLE]["races"], 2);
}

// ------------------------------------------------------------------ lobby view

#[tokio::test]
async fn lobby_view_shows_real_names_except_ghosts() {
    let env = Env::start().await;
    let viewer = target(&env, "Viewer", 1).await;
    let mut ts = Vec::new();
    for t in 1..=5 {
        ts.push(target(&env, &format!("Lv{t}"), t).await);
    }
    let mut members: Vec<&User> = vec![&viewer];
    members.extend(ts.iter());
    env.service(
        "POST",
        "/internal/lobbies",
        Some(lobby_body("room-9", &members)),
    )
    .await;
    let (s, v) = env.call(&viewer, "GET", "/lobbies/room-9", None).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let ms = v["members"].as_array().unwrap();
    assert_eq!(ms.len(), 6);
    for t in 1..=4 {
        let m = ms
            .iter()
            .find(|m| m["profile_id"] == ts[t - 1].profile_id.as_str())
            .unwrap();
        assert_eq!(m["nickname"], format!("Lv{t}"));
        assert_eq!(m["ghost"], false);
    }
    let ghosts: Vec<&Value> = ms.iter().filter(|m| m["ghost"] == true).collect();
    assert_eq!(ghosts.len(), 1);
    let g = ghosts[0];
    assert!(
        g.get("profile_id").is_none() && g.get("nickname").is_none() && g.get("photo").is_none(),
        "{g}"
    );
    assert!(g["alias"].as_str().unwrap().len() >= 4);
    assert!(
        !v.to_string().contains(&ts[4].profile_id),
        "ghost id leaked: {v}"
    );
    assert!(!v.to_string().contains("Lv5"), "ghost nickname leaked");
    // Non-members cannot look.
    let outsider = env.user("Outsider");
    let (s, _) = env.call(&outsider, "GET", "/lobbies/room-9", None).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn lobby_view_hides_blocked_players_both_ways() {
    let env = Env::start().await;
    let a = target(&env, "La", 1).await;
    let b = target(&env, "Lb", 1).await;
    env.service(
        "POST",
        "/internal/lobbies",
        Some(lobby_body("blk", &[&a, &b])),
    )
    .await;
    env.call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    for me in [&a, &b] {
        let (s, v) = env.call(me, "GET", "/lobbies/blk", None).await;
        assert_eq!(s, StatusCode::OK);
        assert_eq!(
            v["members"].as_array().unwrap().len(),
            1,
            "only myself: {v}"
        );
    }
}

#[tokio::test]
async fn ghost_alias_is_stable_within_a_lobby_and_new_per_lobby() {
    let env = Env::start().await;
    let ghost = target(&env, "Ghosty", 5).await;
    let other = target(&env, "Other1", 1).await;
    let mut aliases = Vec::new();
    for id in ["s1", "s2"] {
        env.service(
            "POST",
            "/internal/lobbies",
            Some(lobby_body(id, &[&ghost, &other])),
        )
        .await;
        let a = env.get(&other, &format!("/lobbies/{id}")).await;
        let a1 = a["members"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["ghost"] == true)
            .unwrap()["alias"]
            .clone();
        let b = env.get(&other, &format!("/lobbies/{id}")).await;
        let a2 = b["members"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["ghost"] == true)
            .unwrap()["alias"]
            .clone();
        assert_eq!(a1, a2);
        aliases.push(a1);
    }
    assert_ne!(aliases[0], aliases[1]);
}

// ------------------------------------------------------------------ recent players

async fn recent(env: &Env, u: &User) -> Vec<Value> {
    env.get(u, "/recent").await["recent"]
        .as_array()
        .unwrap()
        .clone()
}

#[tokio::test]
async fn recent_players_per_tier_and_ghost_leaves_no_trace() {
    let env = Env::start().await;
    let me = target(&env, "Racer0", 1).await;
    let mut ts = Vec::new();
    for t in 1..=5 {
        ts.push(target(&env, &format!("Rc{t}"), t).await);
    }
    let mut ids: Vec<String> = vec![me.profile_id.clone()];
    ids.extend(ts.iter().map(|u| u.profile_id.clone()));
    let (s, v) = env
        .service(
            "POST",
            "/encounters",
            Some(json!({"profile_ids": ids, "title_id": TITLE, "match_id": "m-1"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let r = recent(&env, &me).await;
    for t in 1..=4 {
        let e = r
            .iter()
            .find(|e| e["profile"]["profile_id"] == ts[t - 1].profile_id.as_str());
        let e = e.unwrap_or_else(|| panic!("tier {t} should appear"));
        assert_eq!(e["profile"]["nickname"], format!("Rc{t}"));
        assert_eq!(e["title_id"], TITLE);
        assert_eq!(e["match_id"], "m-1");
        // Game friend requests: tier 3 only via the shared lobby, tier 4 blocked.
        assert_eq!(e["can_friend_request"], t != 4, "tier {t}");
    }
    assert_eq!(r.len(), 4, "ghost absent: {r:?}");
    assert!(
        recent(&env, &ts[4]).await.is_empty(),
        "ghost has no trace either way"
    );
    // Tier-3 target can be asked right after the match (the encounter is the lobby proof).
    let (s, v) = env
        .call(
            &me,
            "POST",
            "/friend-requests",
            Some(json!({"profile_id": ts[2].profile_id})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
}

#[tokio::test]
async fn recent_hides_a_player_who_turns_ghost_later_and_blocked_players() {
    let env = Env::start().await;
    let me = target(&env, "Rme", 1).await;
    let a = target(&env, "Ra", 1).await;
    let b = target(&env, "Rb", 1).await;
    env.service("POST", "/encounters", Some(json!({"profile_ids": [me.profile_id, a.profile_id, b.profile_id], "title_id": TITLE, "match_id": "m"}))).await;
    assert_eq!(recent(&env, &me).await.len(), 2);
    env.set_tier(&a, 5);
    env.call(&me, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert!(recent(&env, &me).await.is_empty());
    assert!(recent(&env, &b).await.is_empty());
}

#[tokio::test]
async fn recent_dedupes_keeps_latest_orders_and_expires_after_30_days() {
    let env = Env::start().await;
    let me = target(&env, "Rd", 1).await;
    let a = target(&env, "Rda", 1).await;
    let ids = json!([me.profile_id, a.profile_id]);
    env.service(
        "POST",
        "/encounters",
        Some(json!({"profile_ids": ids, "title_id": TITLE, "match_id": "m1"})),
    )
    .await;
    env.clock.advance_secs(10);
    env.service(
        "POST",
        "/encounters",
        Some(json!({"profile_ids": ids, "title_id": TITLE, "match_id": "m2"})),
    )
    .await;
    let r = recent(&env, &me).await;
    assert_eq!(r.len(), 1);
    assert_eq!(r[0]["match_id"], "m2");
    env.clock.advance_secs(31 * 86_400);
    env.sweep().await;
    let me = User {
        token: env.token_for(&me.account_id, &me.device_id),
        ..me
    };
    assert!(recent(&env, &me).await.is_empty());
}

#[tokio::test]
async fn encounters_need_the_service_credential_and_valid_bodies() {
    let env = Env::start().await;
    let u = target(&env, "Enc", 1).await;
    let body = json!({"profile_ids": [u.profile_id, uuid7()], "title_id": TITLE, "match_id": "m"});
    // No credential, wrong credential, a user token: all refused.
    let (s, _) = env
        .call_raw("POST", "/encounters", None, None, Some(body.clone()))
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let (s, _) = env
        .call(&u, "POST", "/encounters", Some(body.clone()))
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let r = env
        .http
        .post(format!("{}/encounters", env.base))
        .header("X-Napple-Service", "wrong")
        .json(&body)
        .send()
        .await
        .unwrap();
    assert_eq!(r.status().as_u16(), 401);
    for b in [
        json!({"profile_ids": [], "title_id": TITLE, "match_id": "m"}),
        json!({"profile_ids": [u.profile_id], "title_id": "zz", "match_id": "m"}),
        json!({"profile_ids": ["x", "y"], "title_id": TITLE, "match_id": "m"}),
        json!({"profile_ids": [u.profile_id, u.profile_id], "title_id": TITLE, "match_id": ""}),
    ] {
        let (s, v) = env.service("POST", "/encounters", Some(b.clone())).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{b}: {v}");
    }
    // Unknown profile ids in an otherwise valid body are ignored.
    let (s, v) = env.service("POST", "/encounters", Some(body)).await;
    assert_eq!(s, StatusCode::OK, "{v}");
}

#[tokio::test]
async fn service_endpoints_are_off_without_a_configured_token() {
    let env = Env::start_with(|c| c.service_token = None).await;
    let (s, _) = env
        .service(
            "POST",
            "/encounters",
            Some(json!({"profile_ids": [uuid7(), uuid7()], "title_id": TITLE, "match_id": "m"})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

// ------------------------------------------------------------------ reports

#[tokio::test]
async fn reports_accepted_validated_and_rate_limited() {
    let env = Env::start_with(|c| c.reports_per_hour = 3).await;
    let me = target(&env, "Reporter", 1).await;
    let bad = target(&env, "Griefer", 4).await;
    let (s, v) = env
        .call(
            &me,
            "POST",
            "/reports",
            Some(json!({"profile_id": bad.profile_id, "reason": "cheating", "match_id": "m-7"})),
        )
        .await;
    assert_eq!(s, StatusCode::ACCEPTED, "{v}");
    assert!(v["report_id"].is_string());
    for b in [
        json!({"profile_id": me.profile_id, "reason": "x"}),
        json!({"profile_id": bad.profile_id, "reason": ""}),
        json!({"profile_id": bad.profile_id, "reason": "x".repeat(501)}),
        json!({"reason": "no target"}),
    ] {
        let (s, v) = env.call(&me, "POST", "/reports", Some(b.clone())).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{b}: {v}");
    }
    let (s, _) = env
        .call(
            &me,
            "POST",
            "/reports",
            Some(json!({"profile_id": uuid7(), "reason": "x"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    // The three rate-limit tokens are spent (1 accepted + validation passes that reached storage).
    let mut limited = false;
    for _ in 0..5 {
        let (s, _) = env
            .call(
                &me,
                "POST",
                "/reports",
                Some(json!({"profile_id": bad.profile_id, "reason": "again"})),
            )
            .await;
        limited |= s == StatusCode::TOO_MANY_REQUESTS;
    }
    assert!(limited);
}

#[tokio::test]
async fn report_a_ghost_by_lobby_alias_resolves_to_the_real_profile() {
    let env = Env::start().await;
    let me = target(&env, "Victim", 1).await;
    let ghost = target(&env, "RealGhost", 5).await;
    env.service(
        "POST",
        "/internal/lobbies",
        Some(lobby_body("rep", &[&me, &ghost])),
    )
    .await;
    let v = env.get(&me, "/lobbies/rep").await;
    let alias = v["members"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["ghost"] == true)
        .unwrap()["alias"]
        .as_str()
        .unwrap()
        .to_string();
    let (s, v) = env
        .call(
            &me,
            "POST",
            "/reports",
            Some(
                json!({"lobby_id": "rep", "alias": alias, "reason": "harassment", "match_id": "m"}),
            ),
        )
        .await;
    assert_eq!(s, StatusCode::ACCEPTED, "{v}");
    // Moderation sees the real profile and account; the reporter never does.
    let (s, list) = env.service("GET", "/internal/reports", None).await;
    assert_eq!(s, StatusCode::OK);
    let r = &list["reports"][0];
    assert_eq!(r["reported_profile_id"], ghost.profile_id.as_str());
    assert_eq!(r["reported_account_id"], ghost.account_id.as_str());
    assert_eq!(r["reporter_profile_id"], me.profile_id.as_str());
    assert_eq!(r["match_id"], "m");
    // Wrong alias or lobby the reporter is not in.
    let (s, _) = env
        .call(
            &me,
            "POST",
            "/reports",
            Some(json!({"lobby_id": "rep", "alias": "Bot-nope", "reason": "x"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let other = env.user("Elsewhere");
    let (s, _) = env
        .call(
            &other,
            "POST",
            "/reports",
            Some(json!({"lobby_id": "rep", "alias": alias, "reason": "x"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn blocked_profile_cannot_be_probed_through_reports() {
    let env = Env::start().await;
    let a = target(&env, "Ra1", 1).await;
    let b = target(&env, "Rb1", 1).await;
    env.call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    // Reports are write-only and uniform: no difference between a blocker and anyone else.
    let (s, _) = env
        .call(
            &b,
            "POST",
            "/reports",
            Some(json!({"profile_id": a.profile_id, "reason": "x"})),
        )
        .await;
    assert_eq!(s, StatusCode::ACCEPTED);
}

// ------------------------------------------------------------------ tier refresh from the account service

#[tokio::test]
async fn target_tier_comes_from_the_account_directory_with_a_ttl_cache() {
    let env = Env::start_with(|c| c.profile_ttl_secs = 30).await;
    let me = env.user("Cacher");
    let t = target(&env, "Cached", 1).await;
    assert_eq!(search(&env, &me, "cached").await.len(), 1);
    let hits = env
        .mock
        .directory_hits
        .load(std::sync::atomic::Ordering::SeqCst);
    // Within the TTL no further lookups, and the stale tier is still used...
    env.set_tier(&t, 4);
    assert_eq!(search(&env, &me, "cached").await.len(), 1);
    assert_eq!(
        env.mock
            .directory_hits
            .load(std::sync::atomic::Ordering::SeqCst),
        hits
    );
    // ...after it the change applies.
    env.clock.advance_secs(31);
    assert!(search(&env, &me, "cached").await.is_empty());
    assert!(
        env.mock
            .directory_hits
            .load(std::sync::atomic::Ordering::SeqCst)
            > hits
    );
}

#[tokio::test]
async fn directory_outage_keeps_the_last_known_tier() {
    // The social service holds a wrong account credential: every lookup fails, the snapshot taken on the
    // profile's own calls stays in force.
    let env = Env::start_with(|c| c.account_service_token = Some("bad".into())).await;
    let me = env.user("Seeker2");
    let t = target(&env, "Keeper", 4).await;
    assert!(search(&env, &me, "keeper").await.is_empty());
    env.set_tier(&t, 1);
    reg(&env, &t).await; // the owner's own call refreshes the snapshot from its profile list
    env.clock.advance_secs(61); // past the 60 s ownership cache
    reg(&env, &t).await;
    assert_eq!(search(&env, &me, "keeper").await.len(), 1);
}
