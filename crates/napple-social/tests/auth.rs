//! Token verification through the JWKS, and the profile-ownership check against the account service.

mod common;

use std::sync::atomic::Ordering;

use axum::http::StatusCode;
use common::*;
use ed25519_dalek::SigningKey;
use serde_json::json;

#[tokio::test]
async fn healthz_needs_no_auth() {
    let env = Env::start().await;
    let (s, v) = env.call_raw("GET", "/healthz", None, None, None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(v["status"], "ok");
    assert_eq!(v["service"], "napple-social");
    let (s, v) = env.call_raw("GET", "/nope", None, None, None).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    assert_eq!(v["error"]["code"], "not_found");
}

#[tokio::test]
async fn good_token_and_profile() {
    let env = Env::start().await;
    let u = env.user("Tal");
    let me = env.get(&u, "/me").await;
    assert_eq!(me["profile_id"], u.profile_id.as_str());
    assert_eq!(me["account_id"], u.account_id.as_str());
    assert_eq!(me["device_id"], u.device_id.as_str());
    assert_eq!(me["nickname"], "Tal");
    assert_eq!(me["icon_id"], "0");
    assert_eq!(me["friend_limit"], 300);
    assert_eq!(me["presence"]["status"], "offline");
    // Upper-case profile header is normalised.
    let (s, _) = env
        .call_raw(
            "GET",
            "/me",
            Some(&u.token),
            Some(&u.profile_id.to_uppercase()),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
}

#[tokio::test]
async fn token_failures() {
    let env = Env::start().await;
    let u = env.user("Tal");
    let pid = u.profile_id.clone();
    let p = Some(pid.as_str());
    let code = |v: &serde_json::Value| v["error"]["code"].as_str().unwrap().to_string();

    let (s, v) = env.call_raw("GET", "/me", None, p, None).await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "missing_token")
    );
    let (s, v) = env.call_raw("GET", "/me", Some("garbage"), p, None).await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "invalid_token")
    );

    // Expired (beyond the 30 s leeway).
    let mut c = env.claims(&u.account_id, &u.device_id);
    c["exp"] = json!(env.now_s() - 60);
    let (s, v) = env
        .call_raw("GET", "/me", Some(&mint(&env.key, &env.kid, &c)), p, None)
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "token_expired")
    );

    // A valid token becomes expired when the clock passes exp.
    env.clock.advance_secs(900 + 31);
    let (s, v) = env.call(&u, "GET", "/me", None).await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "token_expired")
    );
    let u = User {
        token: env.token_for(&u.account_id, &u.device_id),
        ..u
    };

    // Wrong audience.
    let mut c = env.claims(&u.account_id, &u.device_id);
    c["aud"] = json!("nintendo");
    let (s, v) = env
        .call_raw("GET", "/me", Some(&mint(&env.key, &env.kid, &c)), p, None)
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "wrong_audience")
    );

    // Signed by a key the JWKS does not have, under a known kid.
    let rogue = SigningKey::from_bytes(&[42u8; 32]);
    let (s, v) = env
        .call_raw(
            "GET",
            "/me",
            Some(&mint(
                &rogue,
                &env.kid,
                &env.claims(&u.account_id, &u.device_id),
            )),
            p,
            None,
        )
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "bad_signature")
    );

    // Unknown kid.
    let (s, v) = env
        .call_raw(
            "GET",
            "/me",
            Some(&mint(
                &rogue,
                "nobody-knows",
                &env.claims(&u.account_id, &u.device_id),
            )),
            p,
            None,
        )
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::UNAUTHORIZED, "unknown_kid")
    );
    // 401 responses carry WWW-Authenticate.
    let resp = env
        .http
        .get(format!("{}/me", env.base))
        .send()
        .await
        .unwrap();
    assert!(
        resp.headers()
            .get("www-authenticate")
            .unwrap()
            .to_str()
            .unwrap()
            .starts_with("Bearer")
    );

    // And the good token still works.
    let (s, _) = env.call(&u, "GET", "/me", None).await;
    assert_eq!(s, StatusCode::OK);
}

#[tokio::test]
async fn jwks_is_cached_and_refreshed_on_unknown_kid() {
    let env = Env::start().await;
    let u = env.user("Tal");
    for _ in 0..5 {
        env.get(&u, "/me").await;
    }
    assert_eq!(
        env.mock.jwks_hits.load(Ordering::SeqCst),
        1,
        "JWKS fetched once and cached"
    );

    // Key rotation: the account service publishes a new key and signs with it.
    let k2 = SigningKey::from_bytes(&rand::random::<[u8; 32]>());
    env.mock
        .data
        .lock()
        .unwrap()
        .keys
        .push(("test-key-2".into(), k2.clone()));
    let t2 = mint(&k2, "test-key-2", &env.claims(&u.account_id, &u.device_id));
    // Within 10 s of the last JWKS fetch an unknown kid is refused without hammering the account service...
    let (s, v) = env
        .call_raw("GET", "/me", Some(&t2), Some(&u.profile_id), None)
        .await;
    assert_eq!(
        (s, v["error"]["code"].as_str()),
        (StatusCode::UNAUTHORIZED, Some("unknown_kid"))
    );
    assert_eq!(env.mock.jwks_hits.load(Ordering::SeqCst), 1);
    // ...after that, an unknown kid triggers one refresh and the new key verifies.
    env.clock.advance_secs(11);
    let t2 = mint(&k2, "test-key-2", &env.claims(&u.account_id, &u.device_id));
    let (s, v) = env
        .call_raw("GET", "/me", Some(&t2), Some(&u.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::OK, "unknown kid triggers a refresh: {v}");
    assert_eq!(env.mock.jwks_hits.load(Ordering::SeqCst), 2);
    // A second new kid right after is throttled again.
    let k3 = SigningKey::from_bytes(&rand::random::<[u8; 32]>());
    env.mock
        .data
        .lock()
        .unwrap()
        .keys
        .push(("test-key-3".into(), k3.clone()));
    let t3 = mint(&k3, "test-key-3", &env.claims(&u.account_id, &u.device_id));
    let (s, v) = env
        .call_raw("GET", "/me", Some(&t3), Some(&u.profile_id), None)
        .await;
    assert_eq!(
        (s, v["error"]["code"].as_str()),
        (StatusCode::UNAUTHORIZED, Some("unknown_kid"))
    );
    assert_eq!(env.mock.jwks_hits.load(Ordering::SeqCst), 2);
    env.clock.advance_secs(11);
    let t3 = mint(&k3, "test-key-3", &env.claims(&u.account_id, &u.device_id));
    let (s, _) = env
        .call_raw("GET", "/me", Some(&t3), Some(&u.profile_id), None)
        .await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(env.mock.jwks_hits.load(Ordering::SeqCst), 3);

    // A key removed from the JWKS stops working after the cache refreshes (TTL 10 min).
    env.mock
        .data
        .lock()
        .unwrap()
        .keys
        .retain(|(k, _)| k != "test-key-1");
    env.clock.advance_secs(601);
    let fresh = User {
        token: env.token_for(&u.account_id, &u.device_id),
        ..u.clone()
    };
    let (s, v) = env.call(&fresh, "GET", "/me", None).await;
    assert_eq!(
        (s, v["error"]["code"].as_str()),
        (StatusCode::UNAUTHORIZED, Some("unknown_kid"))
    );
}

#[tokio::test]
async fn profile_must_belong_to_the_account() {
    let env = Env::start().await;
    let family = env.account(&["Parent", "Kid"]);
    let stranger = env.user("Stranger");
    let code = |v: &serde_json::Value| v["error"]["code"].as_str().unwrap().to_string();

    // Both profiles of the account work with the same token.
    env.get(&family[0], "/me").await;
    env.get(&family[1], "/me").await;

    // Someone else's profile with my token.
    let (s, v) = env
        .call_raw(
            "GET",
            "/me",
            Some(&stranger.token),
            Some(&family[0].profile_id),
            None,
        )
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::FORBIDDEN, "profile_not_owned")
    );
    // Missing / malformed header.
    let (s, v) = env
        .call_raw("GET", "/me", Some(&stranger.token), None, None)
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::BAD_REQUEST, "missing_profile")
    );
    let (s, v) = env
        .call_raw("GET", "/me", Some(&stranger.token), Some("me"), None)
        .await;
    assert_eq!(
        (s, code(&v).as_str()),
        (StatusCode::BAD_REQUEST, "invalid_profile")
    );
}

#[tokio::test]
async fn profile_list_is_cached_for_60s() {
    let env = Env::start().await;
    let u = env.user("Tal");
    let hits = || env.mock.profile_hits.load(Ordering::SeqCst);
    for _ in 0..5 {
        env.get(&u, "/me").await;
    }
    assert_eq!(hits(), 1);
    env.clock.advance_secs(59);
    env.get(&u, "/me").await;
    assert_eq!(hits(), 1);
    env.clock.advance_secs(2);
    env.get(&u, "/me").await;
    assert_eq!(hits(), 2);

    // A profile created on another device: refused while the cached list is fresh (<5 s), found after.
    let new_profile = uuid7();
    env.mock
        .data
        .lock()
        .unwrap()
        .profiles
        .get_mut(&u.account_id)
        .unwrap()
        .push(json!({"profile_id": new_profile, "nickname": "New"}));
    let (s, _) = env
        .call_raw("GET", "/me", Some(&u.token), Some(&new_profile), None)
        .await;
    assert_eq!(s, StatusCode::FORBIDDEN);
    env.clock.advance_secs(6);
    let (s, v) = env
        .call_raw(
            "GET",
            "/me",
            Some(&env.token_for(&u.account_id, &u.device_id)),
            Some(&new_profile),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["nickname"], "New");

    // A profile deleted at the account service loses access once the cache expires.
    env.mock
        .data
        .lock()
        .unwrap()
        .profiles
        .get_mut(&u.account_id)
        .unwrap()
        .retain(|p| p["profile_id"] != u.profile_id.as_str());
    env.clock.advance_secs(61);
    let fresh = User {
        token: env.token_for(&u.account_id, &u.device_id),
        ..u.clone()
    };
    let (s, _) = env.call(&fresh, "GET", "/me", None).await;
    assert_eq!(s, StatusCode::FORBIDDEN);
}

#[tokio::test]
async fn account_service_outage_is_503() {
    let env = Env::start().await;
    let u = env.user("Tal");
    env.mock.data.lock().unwrap().fail_profiles = true;
    let (s, v) = env.call(&u, "GET", "/me", None).await;
    assert_eq!(
        (s, v["error"]["code"].as_str()),
        (StatusCode::SERVICE_UNAVAILABLE, Some("account_unavailable"))
    );
}

#[tokio::test]
async fn nickname_snapshot_follows_the_account_service() {
    let env = Env::start().await;
    let a = env.user("Alice");
    let b = env.user("Bob");
    env.befriend(&a, &b).await;
    env.mock
        .data
        .lock()
        .unwrap()
        .profiles
        .get_mut(&a.account_id)
        .unwrap()[0]["nickname"] = json!("Alicia");
    env.clock.advance_secs(61);
    let a = User {
        token: env.token_for(&a.account_id, &a.device_id),
        ..a
    };
    env.get(&a, "/me").await;
    let b = User {
        token: env.token_for(&b.account_id, &b.device_id),
        ..b
    };
    let f = env.get(&b, "/friends").await;
    assert_eq!(f["friends"][0]["profile"]["nickname"], "Alicia");
}

#[tokio::test]
async fn rate_limits() {
    let env = Env::start_with(|c| c.rate_per_min = 10).await; // burst 5
    let u = env.user("Tal");
    for _ in 0..5 {
        env.get(&u, "/me").await;
    }
    let resp = env
        .http
        .get(format!("{}/me", env.base))
        .bearer_auth(&u.token)
        .header("X-Napple-Profile", &u.profile_id)
        .send()
        .await
        .unwrap();
    assert_eq!(resp.status().as_u16(), 429);
    assert!(resp.headers().get("retry-after").is_some());
    // Another account is unaffected.
    let other = env.user("Other");
    env.get(&other, "/me").await;
    // Refill: 10 per minute -> one token every 6 s.
    env.clock.advance_secs(7);
    env.get(&u, "/me").await;

    let env = Env::start_with(|c| c.friend_requests_per_hour = 4).await; // burst 2
    let a = env.user("A");
    let mut codes = Vec::new();
    for i in 0..3 {
        let t = env.user(&format!("T{i}"));
        codes.push(env.code_of(&t).await);
    }
    for c in &codes[..2] {
        let (s, _) = env
            .call(
                &a,
                "POST",
                "/friend-requests",
                Some(json!({"friend_code": c})),
            )
            .await;
        assert_eq!(s, StatusCode::CREATED);
    }
    let (s, v) = env
        .call(
            &a,
            "POST",
            "/friend-requests",
            Some(json!({"friend_code": codes[2]})),
        )
        .await;
    assert_eq!(
        (s, v["error"]["code"].as_str()),
        (StatusCode::TOO_MANY_REQUESTS, Some("rate_limited"))
    );
}
