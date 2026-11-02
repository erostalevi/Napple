//! Presence: setting it, heartbeats, the 3-missed-heartbeats timeout, privacy rules.

mod common;

use axum::http::StatusCode;
use common::*;
use serde_json::{Value, json};

fn code(v: &Value) -> &str {
    v["error"]["code"].as_str().unwrap_or("<none>")
}

fn playing(token: Option<&str>) -> Value {
    let mut v = json!({"status": "playing", "title_id": "0100152000022000", "title_name": "Kart Racer", "mode": "Online race"});
    if let Some(t) = token {
        v["join_token"] = json!(t);
    }
    v
}

async fn friend_presence(env: &Env, viewer: &User, of: &User) -> Value {
    let f = env.get(viewer, "/friends").await;
    f["friends"]
        .as_array()
        .unwrap()
        .iter()
        .find(|x| x["profile"]["profile_id"] == of.profile_id.as_str())
        .expect("friend")["presence"]
        .clone()
}

#[tokio::test]
async fn presence_is_shared_with_friends_only() {
    let env = Env::start().await;
    let (a, b, stranger) = (env.user("A"), env.user("B"), env.user("S"));
    env.befriend(&a, &b).await;
    let mut ws_b = env.ws(&b).await;
    let mut ws_s = env.ws(&stranger).await;

    let (s, v) = env
        .call(&a, "PUT", "/presence", Some(json!({"status": "online"})))
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["presence"]["status"], "online");
    assert_eq!(v["heartbeat_interval_secs"], 60);
    assert_eq!(v["offline_after_secs"], 180);
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["profile_id"], a.profile_id.as_str());
    assert_eq!(ev["presence"]["status"], "online");

    let (s, v) = env
        .call(&a, "PUT", "/presence", Some(playing(Some("room:abc"))))
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let ev = next_event(&mut ws_b, "presence").await;
    let p = &ev["presence"];
    assert_eq!(p["status"], "playing");
    assert_eq!(p["title_id"], "0100152000022000");
    assert_eq!(p["title_name"], "Kart Racer");
    assert_eq!(p["mode"], "Online race");
    assert_eq!(p["joinable"], true);
    assert_eq!(p["join_token"], "room:abc");
    assert_eq!(friend_presence(&env, &b, &a).await["status"], "playing");

    // Re-sending the same state is a heartbeat, not a change: friends get no event.
    env.call(&a, "PUT", "/presence", Some(playing(Some("room:abc"))))
        .await;
    no_event(&mut ws_b, "presence", 300).await;
    // Strangers never see anything.
    no_event(&mut ws_s, "presence", 100).await;

    // Explicit sign-out.
    let (s, _) = env.call(&a, "DELETE", "/presence", None).await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["presence"]["status"], "offline");
    assert!(ev["presence"]["last_seen_ms"].as_i64().is_some());
    assert!(ev["presence"].get("join_token").is_none());
}

#[tokio::test]
async fn presence_validation() {
    let env = Env::start().await;
    let a = env.user("A");
    for body in [
        json!({"status": "away"}),
        json!({"status": "playing"}),
        json!({"status": "playing", "title_id": "xyz"}),
        json!({"status": "playing", "title_id": "0100152000022000", "title_name": "x".repeat(129)}),
        json!({"status": "playing", "title_id": "0100152000022000", "mode": "bad\nline"}),
        json!({"status": "playing", "title_id": "0100152000022000", "join_token": "j".repeat(1025)}),
    ] {
        let (s, v) = env.call(&a, "PUT", "/presence", Some(body.clone())).await;
        assert_eq!(
            (s, code(&v)),
            (StatusCode::BAD_REQUEST, "invalid_presence"),
            "{body}"
        );
    }
    let (s, v) = env
        .call(
            &a,
            "PUT",
            "/presence",
            Some(json!({"status": "online", "extra": 1})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::BAD_REQUEST, "invalid_json"));
    // Title ids are normalised to upper case; "online" drops title fields.
    let (_, v) = env
        .call(
            &a,
            "PUT",
            "/presence",
            Some(json!({"status": "playing", "title_id": "0100abc000022000"})),
        )
        .await;
    assert_eq!(v["presence"]["title_id"], "0100ABC000022000");
    let (_, v) = env
        .call(
            &a,
            "PUT",
            "/presence",
            Some(json!({"status": "online", "title_id": "0100abc000022000"})),
        )
        .await;
    assert!(v["presence"].get("title_id").is_none());
    // status offline == DELETE.
    let (s, _) = env
        .call(&a, "PUT", "/presence", Some(json!({"status": "offline"})))
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert_eq!(
        env.get(&a, "/presence").await["presence"]["status"],
        "offline"
    );
}

#[tokio::test]
async fn offline_after_three_missed_heartbeats() {
    let env = Env::start().await;
    let (a, b) = (env.user("A"), env.user("B"));
    env.befriend(&a, &b).await;
    let mut ws_b = env.ws(&b).await;

    // Heartbeat without presence: the client must PUT first.
    let (s, v) = env.call(&a, "POST", "/presence/heartbeat", None).await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "presence_expired"));

    env.call(&a, "PUT", "/presence", Some(playing(None))).await;
    next_event(&mut ws_b, "presence").await;

    // Heartbeats every 60 s keep it alive indefinitely.
    for _ in 0..5 {
        env.clock.advance_secs(60);
        let a2 = User {
            token: env.token_for(&a.account_id, &a.device_id),
            ..a.clone()
        };
        let (s, v) = env.call(&a2, "POST", "/presence/heartbeat", None).await;
        assert_eq!(s, StatusCode::OK, "{v}");
        assert_eq!(v["presence"]["status"], "playing");
        env.sweep().await;
    }
    no_event(&mut ws_b, "presence", 200).await;
    let since = env
        .get(
            &User {
                token: env.token_for(&a.account_id, &a.device_id),
                ..a.clone()
            },
            "/presence",
        )
        .await["presence"]["last_seen_ms"]
        .as_i64()
        .unwrap();

    // Two, then nearly three missed heartbeats: still online.
    env.clock.advance_secs(179);
    env.sweep().await;
    no_event(&mut ws_b, "presence", 200).await;
    let b2 = User {
        token: env.token_for(&b.account_id, &b.device_id),
        ..b.clone()
    };
    assert_eq!(friend_presence(&env, &b2, &a).await["status"], "playing");

    // Third missed heartbeat: offline, pushed by the sweeper, last seen = the last heartbeat.
    env.clock.advance_secs(2);
    assert_eq!(
        friend_presence(&env, &b2, &a).await["status"],
        "offline",
        "reads are consistent even before the sweep"
    );
    env.sweep().await;
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["presence"]["status"], "offline");
    assert_eq!(ev["presence"]["last_seen_ms"].as_i64().unwrap(), since);

    // A late heartbeat is refused; the client re-sends its state.
    let a3 = User {
        token: env.token_for(&a.account_id, &a.device_id),
        ..a.clone()
    };
    let (s, v) = env.call(&a3, "POST", "/presence/heartbeat", None).await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "presence_expired"));
    env.call(&a3, "PUT", "/presence", Some(json!({"status": "online"})))
        .await;
    assert_eq!(
        next_event(&mut ws_b, "presence").await["presence"]["status"],
        "online"
    );
}

#[tokio::test]
async fn heartbeat_interval_is_configurable() {
    let env = Env::start_with(|c| c.heartbeat_secs = 10).await;
    let a = env.user("A");
    let (_, v) = env
        .call(&a, "PUT", "/presence", Some(json!({"status": "online"})))
        .await;
    assert_eq!(v["offline_after_secs"], 30);
    env.clock.advance_secs(31);
    env.sweep().await;
    assert_eq!(
        env.get(&a, "/presence").await["presence"]["status"],
        "offline"
    );
}

#[tokio::test]
async fn privacy_nobody_and_appear_offline() {
    let env = Env::start().await;
    let (a, b) = (env.user("A"), env.user("B"));
    env.befriend(&a, &b).await;
    let mut ws_b = env.ws(&b).await;
    let mut ws_a = env.ws(&a).await;

    env.call(&a, "PUT", "/presence", Some(playing(Some("room:1"))))
        .await;
    next_event(&mut ws_b, "presence").await;
    let seen_at = friend_presence(&env, &b, &a).await["last_seen_ms"]
        .as_i64()
        .unwrap();

    // Appear offline: friends see offline with the last visible time; A itself still sees its real state.
    env.clock.advance_secs(30);
    let (s, v) = env
        .call(
            &a,
            "PUT",
            "/settings",
            Some(json!({"appear_offline": true})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["appear_offline"], true);
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["presence"]["status"], "offline");
    assert_eq!(ev["presence"]["last_seen_ms"].as_i64().unwrap(), seen_at);
    assert!(ev["presence"].get("join_token").is_none());
    let st = next_event(&mut ws_a, "settings").await;
    assert_eq!(st["appear_offline"], true);
    let mine = env.get(&a, "/presence").await;
    assert_eq!(mine["presence"]["status"], "playing");
    assert_eq!(mine["as_friends_see_it"]["status"], "offline");

    // Changes and heartbeats while hidden leak nothing (not even last seen).
    env.clock.advance_secs(30);
    env.call(&a, "PUT", "/presence", Some(playing(Some("room:2"))))
        .await;
    env.call(&a, "POST", "/presence/heartbeat", None).await;
    no_event(&mut ws_b, "presence", 300).await;
    let fp = friend_presence(&env, &b, &a).await;
    assert_eq!(
        (fp["status"].as_str(), fp["last_seen_ms"].as_i64()),
        (Some("offline"), Some(seen_at))
    );

    // Visible again: friends get the live state.
    env.call(
        &a,
        "PUT",
        "/settings",
        Some(json!({"appear_offline": false})),
    )
    .await;
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["presence"]["join_token"], "room:2");

    // Visibility "nobody": offline and no last seen at all.
    let (s, _) = env
        .call(
            &a,
            "PUT",
            "/settings",
            Some(json!({"presence_visibility": "nobody"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let ev = next_event(&mut ws_b, "presence").await;
    assert_eq!(ev["presence"]["status"], "offline");
    assert!(ev["presence"]["last_seen_ms"].is_null());
    assert!(friend_presence(&env, &b, &a).await["last_seen_ms"].is_null());
    let (s, v) = env
        .call(
            &a,
            "PUT",
            "/settings",
            Some(json!({"presence_visibility": "everyone"})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::BAD_REQUEST, "invalid_setting"));
    let st = env.get(&a, "/settings").await;
    assert_eq!(st["presence_visibility"], "nobody");

    // Back to friends.
    env.call(
        &a,
        "PUT",
        "/settings",
        Some(json!({"presence_visibility": "friends"})),
    )
    .await;
    assert_eq!(
        next_event(&mut ws_b, "presence").await["presence"]["status"],
        "playing"
    );
}

#[tokio::test]
async fn friends_list_orders_online_first() {
    let env = Env::start().await;
    let (me, x, y, z) = (
        env.user("Me"),
        env.user("Xavier"),
        env.user("Yvonne"),
        env.user("Aaron"),
    );
    for f in [&x, &y, &z] {
        env.befriend(&me, f).await;
    }
    env.call(&y, "PUT", "/presence", Some(json!({"status": "online"})))
        .await;
    let list = env.get(&me, "/friends").await;
    let names: Vec<&str> = list["friends"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["profile"]["nickname"].as_str().unwrap())
        .collect();
    assert_eq!(names, ["Yvonne", "Aaron", "Xavier"]);
}
