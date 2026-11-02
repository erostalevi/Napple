//! Session invites over the WebSocket, their expiry, and the stream's own protocol (ping, heartbeat, reauth).

mod common;

use axum::http::StatusCode;
use common::*;
use futures_util::StreamExt;
use serde_json::{Value, json};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;

fn code(v: &Value) -> &str {
    v["error"]["code"].as_str().unwrap_or("<none>")
}

#[tokio::test]
async fn invite_delivered_over_websocket_and_accepted() {
    let env = Env::start().await;
    let (host, guest) = (env.user("Host"), env.user("Guest"));
    env.befriend(&host, &guest).await;
    let mut ws_host = env.ws(&host).await;
    let mut ws_guest = env.ws(&guest).await;

    // No joinable session yet.
    let (s, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": guest.profile_id})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "not_joinable"));

    env.call(&host, "PUT", "/presence", Some(json!({"status": "playing", "title_id": "0100152000022000", "title_name": "Kart Racer", "mode": "Lobby", "join_token": "room:xyz"}))).await;
    let (s, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": guest.profile_id, "message": "race?"})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    let inv = &v["invite"];
    let id = inv["invite_id"].as_str().unwrap().to_string();
    assert_eq!(inv["join_token"], "room:xyz");
    assert_eq!(inv["title_name"], "Kart Racer");
    assert_eq!(inv["mode"], "Lobby");
    assert_eq!(inv["message"], "race?");
    assert_eq!(
        inv["expires_at_ms"].as_i64().unwrap() - inv["created_at_ms"].as_i64().unwrap(),
        600_000
    );

    let ev = next_event(&mut ws_guest, "invite").await;
    assert_eq!(ev["invite"]["invite_id"], id.as_str());
    assert_eq!(ev["invite"]["from"]["nickname"], "Host");
    assert_eq!(ev["invite"]["join_token"], "room:xyz");

    let lg = env.get(&guest, "/invites").await;
    assert_eq!(lg["incoming"].as_array().unwrap().len(), 1);
    assert_eq!(
        env.get(&host, "/invites").await["outgoing"]
            .as_array()
            .unwrap()
            .len(),
        1
    );

    // Only the recipient can answer.
    let (s, _) = env
        .call(&host, "POST", &format!("/invites/{id}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, v) = env
        .call(&guest, "POST", &format!("/invites/{id}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["invite"]["join_token"], "room:xyz");
    let ev = next_event(&mut ws_host, "invite_response").await;
    assert_eq!(
        (ev["invite_id"].as_str(), ev["accepted"].as_bool()),
        (Some(id.as_str()), Some(true))
    );
    assert_eq!(ev["profile_id"], guest.profile_id.as_str());
    let ev = next_event(&mut ws_guest, "invite_closed").await;
    assert_eq!(ev["reason"], "accepted");
    assert!(
        env.get(&guest, "/invites").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    let (s, _) = env
        .call(&guest, "POST", &format!("/invites/{id}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn decline_cancel_replace_and_rules() {
    let env = Env::start().await;
    let (host, guest, stranger) = (env.user("Host"), env.user("Guest"), env.user("Stranger"));
    env.befriend(&host, &guest).await;
    let mut ws_host = env.ws(&host).await;
    let mut ws_guest = env.ws(&guest).await;

    // Explicit join_token without presence works.
    let send = |tok: &str| json!({"to_profile_id": guest.profile_id, "join_token": tok});
    let (s, v) = env
        .call(&host, "POST", "/invites", Some(send("room:1")))
        .await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    let id1 = v["invite"]["invite_id"].as_str().unwrap().to_string();
    assert!(v["invite"]["title_id"].is_null());

    // Decline.
    let (s, _) = env
        .call(&guest, "POST", &format!("/invites/{id1}/decline"), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let ev = next_event(&mut ws_host, "invite_response").await;
    assert_eq!(ev["accepted"], false);
    // The guest's own devices learn it is answered.
    let ev = next_event(&mut ws_guest, "invite_closed").await;
    assert_eq!(
        (ev["invite_id"].as_str(), ev["reason"].as_str()),
        (Some(id1.as_str()), Some("declined"))
    );

    // A new invite to the same friend replaces the previous one.
    let (_, v) = env
        .call(&host, "POST", "/invites", Some(send("room:2")))
        .await;
    let id2 = v["invite"]["invite_id"].as_str().unwrap().to_string();
    let (_, v) = env
        .call(&host, "POST", "/invites", Some(send("room:3")))
        .await;
    let id3 = v["invite"]["invite_id"].as_str().unwrap().to_string();
    let ev = next_event(&mut ws_guest, "invite_closed").await;
    assert_eq!(
        (ev["invite_id"].as_str(), ev["reason"].as_str()),
        (Some(id2.as_str()), Some("replaced"))
    );
    assert_eq!(
        env.get(&guest, "/invites").await["incoming"]
            .as_array()
            .unwrap()
            .len(),
        1
    );

    // Cancel by the sender.
    let (s, _) = env
        .call(&guest, "DELETE", &format!("/invites/{id3}"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = env
        .call(&host, "DELETE", &format!("/invites/{id3}"), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let ev = next_event(&mut ws_guest, "invite_closed").await;
    assert_eq!(
        (ev["invite_id"].as_str(), ev["reason"].as_str()),
        (Some(id3.as_str()), Some("cancelled"))
    );

    // Only friends; not yourself; message length.
    let (s, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": stranger.profile_id, "join_token": "r"})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::FORBIDDEN, "not_friends"));
    let (s, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": host.profile_id, "join_token": "r"})),
        )
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::BAD_REQUEST, "cannot_invite_self")
    );
    let (s, v) = env.call(&host, "POST", "/invites", Some(json!({"to_profile_id": guest.profile_id, "join_token": "r", "message": "m".repeat(141)}))).await;
    assert_eq!((s, code(&v)), (StatusCode::BAD_REQUEST, "invalid_invite"));
    // Reserved names: an invite message may not contain "napple" (look-alikes included); the nap- prefix is only for names.
    let (s, v) = env.call(&host, "POST", "/invites", Some(json!({"to_profile_id": guest.profile_id, "join_token": "r", "message": "join my Nаpple room"}))).await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::UNPROCESSABLE_ENTITY, "reserved_name")
    );

    // Removing the friend closes live invites.
    let (_, v) = env
        .call(&host, "POST", "/invites", Some(send("room:4")))
        .await;
    let id4 = v["invite"]["invite_id"].as_str().unwrap().to_string();
    env.call(
        &guest,
        "DELETE",
        &format!("/friends/{}", host.profile_id),
        None,
    )
    .await;
    let ev = next_event_where(&mut ws_host, "invite_closed", "invite_id", &id4).await;
    assert_eq!(ev["reason"], "removed");
    assert!(
        env.get(&host, "/invites").await["outgoing"]
            .as_array()
            .unwrap()
            .is_empty()
    );
}

#[tokio::test]
async fn invites_expire_after_10_minutes() {
    let env = Env::start().await;
    let (host, guest) = (env.user("Host"), env.user("Guest"));
    env.befriend(&host, &guest).await;
    let (_, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": guest.profile_id, "join_token": "room:1"})),
        )
        .await;
    let id = v["invite"]["invite_id"].as_str().unwrap().to_string();

    // A guest that connects later still gets the live invite on connect.
    env.clock.advance_secs(9 * 60);
    let guest2 = User {
        token: env.token_for(&guest.account_id, &guest.device_id),
        ..guest.clone()
    };
    let mut ws_guest = env.ws(&guest2).await;
    let ev = next_event(&mut ws_guest, "invite").await;
    assert_eq!(ev["invite"]["invite_id"], id.as_str());

    // Past 10 minutes it cannot be accepted, and the sweeper tells both sides.
    env.clock.advance_secs(61);
    let guest3 = User {
        token: env.token_for(&guest.account_id, &guest.device_id),
        ..guest.clone()
    };
    let (s, v) = env
        .call(&guest3, "POST", &format!("/invites/{id}/accept"), None)
        .await;
    assert_eq!((s, code(&v)), (StatusCode::GONE, "invite_expired"));
    assert!(
        env.get(&guest3, "/invites").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    env.sweep().await;
    let ev = next_event(&mut ws_guest, "invite_closed").await;
    assert_eq!(
        (ev["invite_id"].as_str(), ev["reason"].as_str()),
        (Some(id.as_str()), Some("expired"))
    );
    let (s, _) = env
        .call(&guest3, "POST", &format!("/invites/{id}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
}

#[tokio::test]
async fn blocked_profiles_lose_invites() {
    let env = Env::start().await;
    let (host, guest) = (env.user("Host"), env.user("Guest"));
    env.befriend(&host, &guest).await;
    let mut ws_host = env.ws(&host).await;
    env.call(
        &host,
        "POST",
        "/invites",
        Some(json!({"to_profile_id": guest.profile_id, "join_token": "room:1"})),
    )
    .await;
    env.call(&guest, "PUT", &format!("/blocks/{}", host.profile_id), None)
        .await;
    next_event(&mut ws_host, "invite_closed").await;
    let (s, v) = env
        .call(
            &host,
            "POST",
            "/invites",
            Some(json!({"to_profile_id": guest.profile_id, "join_token": "room:1"})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::FORBIDDEN, "not_friends"));
}

#[tokio::test]
async fn stream_protocol() {
    let env = Env::start().await;
    let a = env.user("A");

    // Upgrade without credentials is refused like any request.
    let req = format!("{}/stream", env.ws_base)
        .into_client_request()
        .unwrap();
    assert!(tokio_tungstenite::connect_async(req).await.is_err());

    let mut ws = env.ws(&a).await;
    ws_send(&mut ws, json!({"type": "ping"})).await;
    assert!(next_event(&mut ws, "pong").await["server_time_ms"].is_i64());

    ws_send(&mut ws, json!({"type": "heartbeat"})).await;
    assert_eq!(
        next_event(&mut ws, "error").await["code"],
        "presence_expired"
    );
    env.call(&a, "PUT", "/presence", Some(json!({"status": "online"})))
        .await;
    // Own devices see their own presence change too.
    assert_eq!(
        next_event(&mut ws, "presence").await["presence"]["status"],
        "online"
    );
    ws_send(&mut ws, json!({"type": "heartbeat"})).await;
    assert_eq!(
        next_event(&mut ws, "heartbeat_ack").await["presence"]["status"],
        "online"
    );

    ws_send(&mut ws, json!({"type": "dance"})).await;
    assert_eq!(
        next_event(&mut ws, "error").await["code"],
        "invalid_message"
    );

    // Reauth: same account works, another account's token is refused, garbage is refused.
    ws_send(
        &mut ws,
        json!({"type": "reauth", "token": env.token_for(&a.account_id, "dev-2")}),
    )
    .await;
    assert!(next_event(&mut ws, "reauthed").await["token_expires_at"].is_i64());
    let other = env.user("Other");
    ws_send(&mut ws, json!({"type": "reauth", "token": other.token})).await;
    assert_eq!(
        next_event(&mut ws, "error").await["code"],
        "account_mismatch"
    );
    ws_send(&mut ws, json!({"type": "reauth", "token": "x.y.z"})).await;
    assert_eq!(next_event(&mut ws, "error").await["code"], "invalid_token");

    // Two devices of one profile both get events.
    let mut ws2 = env.ws(&a).await;
    let b = env.user("B");
    let ac = env.code_of(&a).await;
    env.call(
        &b,
        "POST",
        "/friend-requests",
        Some(json!({"friend_code": ac})),
    )
    .await;
    next_event(&mut ws, "friend_request").await;
    next_event(&mut ws2, "friend_request").await;
    assert_eq!(env.state.hub.connections(&a.profile_id), 2);
    drop(ws2);
}

#[tokio::test]
async fn stream_closes_when_token_expires() {
    let env = Env::start().await;
    let a = env.user("A");
    // A token that expires (with the 30 s leeway) about one second from now.
    let mut c = env.claims(&a.account_id, &a.device_id);
    c["exp"] = json!(env.now_s() - 29);
    let short = User {
        token: mint(&env.key, &env.kid, &c),
        ..a.clone()
    };
    let mut ws = env.ws(&short).await;
    let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        match tokio::time::timeout_at(deadline, ws.next())
            .await
            .expect("socket should close")
        {
            Some(Ok(Message::Close(Some(f)))) => {
                assert_eq!(u16::from(f.code), 4001);
                break;
            }
            Some(Ok(_)) => continue,
            other => panic!("unexpected {other:?}"),
        }
    }
}
