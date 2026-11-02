//! Friend codes, the friend-request lifecycle, removal, blocks and limits.

mod common;

use axum::http::StatusCode;
use common::*;
use napple_social::{codes, db};
use serde_json::{Value, json};

fn code(v: &Value) -> &str {
    v["error"]["code"].as_str().unwrap_or("<none>")
}

#[tokio::test]
async fn friend_codes() {
    let env = Env::start().await;
    let fam = env.account(&["A", "B", "C"]);
    let mut seen = std::collections::HashSet::new();
    for u in &fam {
        let c = env.code_of(u).await;
        assert!(c.starts_with("NP-") && c.len() == 17, "{c}");
        assert!(codes::parse(&c).is_some());
        assert!(seen.insert(c.clone()), "codes are unique per profile");
        // Stable across calls.
        assert_eq!(env.code_of(u).await, c);
    }
    // Regenerate gives a new valid code; the old one stops resolving.
    let old = env.code_of(&fam[0]).await;
    let (s, v) = env
        .call(&fam[0], "POST", "/me/friend-code/regenerate", None)
        .await;
    assert_eq!(s, StatusCode::OK);
    let new = v["friend_code"].as_str().unwrap().to_string();
    assert_ne!(new, old);
    assert_eq!(env.code_of(&fam[0]).await, new);
    let other = env.user("Other");
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": old})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "code_not_found"));
    // Regeneration is rate limited (3 per day).
    env.call(&fam[0], "POST", "/me/friend-code/regenerate", None)
        .await;
    env.call(&fam[0], "POST", "/me/friend-code/regenerate", None)
        .await;
    let (s, _) = env
        .call(&fam[0], "POST", "/me/friend-code/regenerate", None)
        .await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS);

    // Input forms: without prefix / dashes works; typos and garbage are 400.
    let target = env.code_of(&fam[1]).await;
    let digits: String = target.chars().filter(char::is_ascii_digit).collect();
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": digits})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    let mut typo = digits.clone().into_bytes();
    typo[3] = b'0' + ((typo[3] - b'0' + 1) % 10);
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": String::from_utf8(typo).unwrap()})),
        )
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::BAD_REQUEST, "invalid_friend_code")
    );
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": "hello"})),
        )
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::BAD_REQUEST, "invalid_friend_code")
    );
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"code": "x"})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::BAD_REQUEST, "invalid_json"));
    // Own code.
    let mine = env.code_of(&other).await;
    let (s, v) = env
        .call(
            &other,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": mine})),
        )
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::BAD_REQUEST, "cannot_friend_self")
    );
}

#[tokio::test]
async fn request_lifecycle() {
    let env = Env::start().await;
    let (a, b, c) = (env.user("Alice"), env.user("Bob"), env.user("Carol"));
    let mut ws_a = env.ws(&a).await;
    let mut ws_b = env.ws(&b).await;

    // Send.
    let bob_code = env.code_of(&b).await;
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bob_code})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    assert_eq!(v["status"], "pending");
    let req = v["request"].clone();
    let rid = req["request_id"].as_str().unwrap().to_string();
    assert_eq!(req["from"]["nickname"], "Alice");
    assert_eq!(req["to"]["profile_id"], b.profile_id.as_str());
    let ev = next_event(&mut ws_b, "friend_request").await;
    assert_eq!(ev["request"]["request_id"], rid.as_str());

    // Duplicate.
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bob_code})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "request_exists"));

    // Lists.
    let la = env.get(&a, "/friend-requests").await;
    assert_eq!(la["outgoing"].as_array().unwrap().len(), 1);
    assert_eq!(la["incoming"].as_array().unwrap().len(), 0);
    let lb = env.get(&b, "/friend-requests").await;
    assert_eq!(lb["incoming"][0]["request_id"], rid.as_str());

    // Only the recipient accepts; only the sender cancels.
    let (s, v) = env
        .call(&a, "POST", &format!("/friend-requests/{rid}/accept"), None)
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "request_not_found"));
    let (s, _) = env
        .call(&b, "DELETE", &format!("/friend-requests/{rid}"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = env
        .call(&c, "POST", &format!("/friend-requests/{rid}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);

    // Accept.
    let (s, v) = env
        .call(&b, "POST", &format!("/friend-requests/{rid}/accept"), None)
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["friend"]["profile"]["profile_id"], a.profile_id.as_str());
    // The request closes first, then the friend appears.
    next_event(&mut ws_a, "friend_request_closed").await;
    let ev = next_event(&mut ws_a, "friend_added").await;
    assert_eq!(ev["friend"]["profile"]["nickname"], "Bob");
    let fa = env.get(&a, "/friends").await;
    assert_eq!(fa["count"], 1);
    assert_eq!(fa["limit"], 300);
    assert_eq!(
        fa["friends"][0]["profile"]["profile_id"],
        b.profile_id.as_str()
    );
    assert_eq!(fa["friends"][0]["presence"]["status"], "offline");
    assert!(
        env.get(&b, "/friend-requests").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bob_code})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "already_friends"));

    // Decline.
    let carol_code = env.code_of(&c).await;
    let (_, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": carol_code})),
        )
        .await;
    let rid2 = v["request"]["request_id"].as_str().unwrap().to_string();
    let (s, _) = env
        .call(
            &c,
            "POST",
            &format!("/friend-requests/{rid2}/decline"),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let ev = next_event(&mut ws_a, "friend_request_closed").await;
    assert_eq!(ev["request_id"], rid2.as_str());
    assert!(
        env.get(&a, "/friend-requests").await["outgoing"]
            .as_array()
            .unwrap()
            .is_empty()
    );

    // Cancel.
    let (_, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": carol_code})),
        )
        .await;
    let rid3 = v["request"]["request_id"].as_str().unwrap().to_string();
    let (s, _) = env
        .call(&a, "DELETE", &format!("/friend-requests/{rid3}"), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert!(
        env.get(&c, "/friend-requests").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );

    // Crossed requests: sending to someone who already asked you makes you friends at once.
    let alice_code = env.code_of(&a).await;
    let (s, _) = env
        .call(
            &c,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": alice_code})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED);
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": carol_code})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["status"], "friends");
    assert_eq!(env.get(&c, "/friends").await["count"], 1);
    assert!(
        env.get(&c, "/friend-requests").await["outgoing"]
            .as_array()
            .unwrap()
            .is_empty()
    );

    // Remove friend.
    let (s, _) = env
        .call(&b, "DELETE", &format!("/friends/{}", a.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let ev = next_event(&mut ws_a, "friend_removed").await;
    assert_eq!(ev["profile_id"], b.profile_id.as_str());
    assert_eq!(env.get(&a, "/friends").await["count"], 1);
    let (s, v) = env
        .call(&b, "DELETE", &format!("/friends/{}", a.profile_id), None)
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "not_friends"));
    let (s, v) = env.call(&b, "DELETE", "/friends/not-a-uuid", None).await;
    assert_eq!((s, code(&v)), (StatusCode::BAD_REQUEST, "invalid_id"));
}

#[tokio::test]
async fn profiles_on_one_account_are_separate_identities() {
    let env = Env::start().await;
    let fam = env.account(&["Parent", "Kid"]);
    let friend = env.user("Friend");
    env.befriend(&friend, &fam[1]).await;
    assert_eq!(env.get(&fam[1], "/friends").await["count"], 1);
    assert_eq!(env.get(&fam[0], "/friends").await["count"], 0);
    // Siblings on one account may befriend each other.
    env.befriend(&fam[0], &fam[1]).await;
    assert_eq!(env.get(&fam[0], "/friends").await["count"], 1);
}

#[tokio::test]
async fn not_accepting_requests() {
    let env = Env::start().await;
    let (a, b) = (env.user("A"), env.user("B"));
    let (s, v) = env
        .call(
            &b,
            "PUT",
            "/settings",
            Some(json!({"accept_friend_requests": false})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(v["accept_friend_requests"], false);
    let bc = env.code_of(&b).await;
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bc})),
        )
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::FORBIDDEN, "not_accepting_requests")
    );
    // B can still send.
    let ac = env.code_of(&a).await;
    let (s, _) = env
        .call(
            &b,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": ac})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED);
}

#[tokio::test]
async fn blocks() {
    let env = Env::start().await;
    let (a, b) = (env.user("A"), env.user("B"));
    env.befriend(&a, &b).await;
    let mut ws_b = env.ws(&b).await;

    // Self / unknown.
    let (s, v) = env
        .call(&a, "PUT", &format!("/blocks/{}", a.profile_id), None)
        .await;
    assert_eq!(
        (s, code(&v)),
        (StatusCode::BAD_REQUEST, "cannot_block_self")
    );
    let (s, v) = env
        .call(&a, "PUT", &format!("/blocks/{}", uuid7()), None)
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "profile_not_found"));

    // Blocking a friend ends the friendship.
    let (s, v) = env
        .call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["blocked"]["profile"]["profile_id"], b.profile_id.as_str());
    let ev = next_event(&mut ws_b, "friend_removed").await;
    assert_eq!(ev["profile_id"], a.profile_id.as_str());
    assert_eq!(env.get(&a, "/friends").await["count"], 0);
    assert_eq!(env.get(&b, "/friends").await["count"], 0);
    // Idempotent.
    let (s, _) = env
        .call(&a, "PUT", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::OK);
    let bl = env.get(&a, "/blocks").await;
    assert_eq!(bl["count"], 1);
    assert_eq!(bl["blocks"][0]["profile"]["nickname"], "B");

    // The blocked profile cannot reach the blocker: indistinguishable from a wrong code.
    let ac = env.code_of(&a).await;
    let (s, v) = env
        .call(
            &b,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": ac})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "code_not_found"));
    // The blocker is told why.
    let bc = env.code_of(&b).await;
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bc})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "blocked_by_you"));

    // Blocking also drops pending requests in both directions.
    let c = env.user("C");
    let cc = env.code_of(&c).await;
    env.call(
        &a,
        "POST",
        "/friend-requests",
        Some(json!({"friend_code": cc})),
    )
    .await;
    let mut ws_c = env.ws(&c).await;
    let (s, _) = env
        .call(&a, "PUT", &format!("/blocks/{}", c.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::OK);
    next_event(&mut ws_c, "friend_request_closed").await;
    assert!(
        env.get(&c, "/friend-requests").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );

    // Unblock: no friendship comes back, requests work again.
    let (s, _) = env
        .call(&a, "DELETE", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (s, v) = env
        .call(&a, "DELETE", &format!("/blocks/{}", b.profile_id), None)
        .await;
    assert_eq!((s, code(&v)), (StatusCode::NOT_FOUND, "not_blocked"));
    assert_eq!(env.get(&a, "/friends").await["count"], 0);
    let (s, _) = env
        .call(
            &b,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": ac})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED);
}

/// Seeds `n` friends for a profile straight into the database (the API path is covered elsewhere).
async fn seed_friends(env: &Env, profile: &str, n: usize) {
    let p = profile.to_string();
    env.state
        .db
        .tx(move |c| {
            for i in 0..n {
                let f = uuid7();
                db::ensure_profile(c, "seed-account", &f, &format!("seed{i}"), "", 1)?;
                db::add_friendship(c, &p, &f, 1)?;
            }
            Ok(())
        })
        .await
        .unwrap();
}

#[tokio::test]
async fn friend_limit_300() {
    let env = Env::start().await;
    let (full, b, c, d) = (
        env.user("Full"),
        env.user("B"),
        env.user("C"),
        env.user("D"),
    );
    env.get(&full, "/me").await;
    seed_friends(&env, &full.profile_id, 299).await;

    // A request arrives at 299, then the 300th friend is added another way.
    let fc = env.code_of(&full).await;
    let (s, v) = env
        .call(
            &c,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": fc})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED);
    let pending = v["request"]["request_id"].as_str().unwrap().to_string();
    env.befriend(&d, &full).await;
    assert_eq!(env.get(&full, "/friends").await["count"], 300);

    // Full cannot send, cannot receive, cannot accept.
    let bc = env.code_of(&b).await;
    let (s, v) = env
        .call(
            &full,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bc})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "friend_limit"));
    let (s, v) = env
        .call(
            &b,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": fc})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "target_friend_limit"));
    let (s, v) = env
        .call(
            &full,
            "POST",
            &format!("/friend-requests/{pending}/accept"),
            None,
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "friend_limit"));
    // The failed accept rolled back: the request is still there.
    assert_eq!(
        env.get(&full, "/friend-requests").await["incoming"]
            .as_array()
            .unwrap()
            .len(),
        1
    );

    // Removing one friend frees a slot.
    env.call(&full, "DELETE", &format!("/friends/{}", d.profile_id), None)
        .await;
    let (s, _) = env
        .call(
            &full,
            "POST",
            &format!("/friend-requests/{pending}/accept"),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(env.get(&full, "/me").await["friend_count"], 300);
}

#[tokio::test]
async fn pending_request_limit() {
    let env = Env::start().await;
    let a = env.user("A");
    env.get(&a, "/me").await;
    let me = a.profile_id.clone();
    env.state
        .db
        .tx(move |c| {
            for i in 0..100 {
                let t = uuid7();
                db::ensure_profile(c, "seed", &t, &format!("t{i}"), "", 1)?;
                db::insert_request(c, &uuid7(), &me, &t, 1)?;
            }
            Ok(())
        })
        .await
        .unwrap();
    let b = env.user("B");
    let bc = env.code_of(&b).await;
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": bc})),
        )
        .await;
    assert_eq!((s, code(&v)), (StatusCode::CONFLICT, "too_many_pending"));
}

#[tokio::test]
async fn erase_profile() {
    let env = Env::start().await;
    let (a, b, c) = (env.user("A"), env.user("B"), env.user("C"));
    env.befriend(&a, &b).await;
    let cc = env.code_of(&c).await;
    env.call(
        &a,
        "POST",
        "/friend-requests",
        Some(json!({"friend_code": cc})),
    )
    .await;
    let old_code = env.code_of(&a).await;
    let mut ws_b = env.ws(&b).await;
    let (s, _) = env.call(&a, "DELETE", "/me", None).await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    next_event(&mut ws_b, "friend_removed").await;
    assert_eq!(env.get(&b, "/friends").await["count"], 0);
    assert!(
        env.get(&c, "/friend-requests").await["incoming"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    // Next use registers afresh with a new code.
    let me = env.get(&a, "/me").await;
    assert_ne!(me["friend_code"].as_str().unwrap(), old_code);
    assert_eq!(me["friend_count"], 0);
}
