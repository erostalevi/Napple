//! Sign-in: email code + magic link, passkeys, refresh rotation and reuse detection, devices, JWKS, rate limits, CORS.

mod common;
use axum::body::Body;
use axum::http::{Request, StatusCode};
use common::*;
use napple_account::util::{b64url, random_bytes};
use napple_account::webauthn::soft::SoftAuthenticator;
use serde_json::json;

fn dev(name: &str) -> serde_json::Value {
    json!({"name": name, "platform": "macos", "pubkey": b64url(&random_bytes::<32>())})
}

#[tokio::test]
async fn email_code_is_single_use_and_creates_the_account_once() {
    let t = T::new();
    let (s, v) = t
        .req(
            "POST",
            "/auth/email/start",
            None,
            Some(json!({"email": "Player@Example.com"})),
        )
        .await;
    assert_eq!(s, StatusCode::ACCEPTED, "{v}");
    let mail = t.mail.last_to("player@example.com").unwrap();
    assert!(
        mail.text
            .contains("/auth/email/link#email=player%40example%2Ecom&token="),
        "{}",
        mail.text
    );
    let code = t.code_for("player@example.com");

    let wrong = if code == "000000" { "111111" } else { "000000" };
    let (s, v) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "player@example.com", "code": wrong, "device": dev("Mac")})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
    assert_eq!(v["error_code"], "invalid_code");

    let (s, v) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "PLAYER@example.com", "code": code, "device": dev("Mac")})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["new_account"], true);
    assert_eq!(v["key_state"], "bootstrap");
    assert_eq!(v["expires_in"], 900);
    let account = v["account_id"].as_str().unwrap().to_string();

    // Same code again: refused (single use).
    let (s, _) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "player@example.com", "code": code, "device": dev("Mac")})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);

    // Second sign-in through the magic-link token lands on the same account.
    t.req(
        "POST",
        "/auth/email/start",
        None,
        Some(json!({"email": "player@example.com"})),
    )
    .await;
    let text = t.mail.last_to("player@example.com").unwrap().text;
    let token = text
        .split("&token=")
        .nth(1)
        .unwrap()
        .split_whitespace()
        .next()
        .unwrap()
        .to_string();
    let (s, v) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "player@example.com", "token": token, "device": dev("iPhone")})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["new_account"], false);
    assert_eq!(v["account_id"], account.as_str());

    // GET /me with the access token.
    let (s, me) = t
        .req(
            "GET",
            "/me",
            Some(v["access_token"].as_str().unwrap()),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(me["email"], "player@example.com");
    assert_eq!(me["plan"]["name"], "free");
    assert_eq!(me["plan"]["synced_titles_limit"], 1);
    assert_eq!(me["plan"]["billing_available"], false);
}

#[tokio::test]
async fn codes_expire_and_lock_after_too_many_attempts() {
    let t = T::new();
    t.req(
        "POST",
        "/auth/email/start",
        None,
        Some(json!({"email": "a@napple.test"})),
    )
    .await;
    let code = t.code_for("a@napple.test");
    for _ in 0..5 {
        t.req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "a@napple.test", "code": "abcdef", "device": dev("Mac")})),
        )
        .await;
    }
    let (s, _) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "a@napple.test", "code": code, "device": dev("Mac")})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "locked after 5 wrong attempts");

    t.req(
        "POST",
        "/auth/email/start",
        None,
        Some(json!({"email": "a@napple.test"})),
    )
    .await;
    let code = t.code_for("a@napple.test");
    t.clock.advance(11 * 60);
    let (s, _) = t
        .req(
            "POST",
            "/auth/email/finish",
            None,
            Some(json!({"email": "a@napple.test", "code": code, "device": dev("Mac")})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "expired after 10 minutes");
}

#[tokio::test]
async fn start_is_rate_limited_per_email_and_per_ip() {
    let t = T::new();
    for _ in 0..5 {
        let (s, _) = t
            .req(
                "POST",
                "/auth/email/start",
                None,
                Some(json!({"email": "spam@napple.test"})),
            )
            .await;
        assert_eq!(s, StatusCode::ACCEPTED);
    }
    let req = Request::builder()
        .method("POST")
        .uri("/account/v1/auth/email/start")
        .header("content-type", "application/json")
        .body(Body::from(r#"{"email":"spam@napple.test"}"#))
        .unwrap();
    let (s, h, _) = t.raw(req).await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS);
    assert!(h.get("retry-after").is_some());
    // Per IP: 30/min across all auth endpoints by default.
    let mut limited = false;
    for i in 0..40 {
        let (s, _) = t
            .req(
                "POST",
                "/auth/email/start",
                None,
                Some(json!({"email": format!("u{i}@napple.test")})),
            )
            .await;
        limited |= s == StatusCode::TOO_MANY_REQUESTS;
    }
    assert!(limited);
    assert_eq!(t.mail.count(), 5 + 24, "no mail is sent once limited");
    t.clock.advance(15 * 60);
    let (s, _) = t
        .req(
            "POST",
            "/auth/email/start",
            None,
            Some(json!({"email": "spam@napple.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::ACCEPTED, "window resets");
}

#[tokio::test]
async fn bad_input_is_refused() {
    let t = T::new();
    let (s, _) = t
        .req(
            "POST",
            "/auth/email/start",
            None,
            Some(json!({"email": "not-an-email"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
    t.req(
        "POST",
        "/auth/email/start",
        None,
        Some(json!({"email": "b@napple.test"})),
    )
    .await;
    let code = t.code_for("b@napple.test");
    for d in [
        json!({"name": "Mac", "platform": "switch", "pubkey": b64url(&[1u8; 32])}),
        json!({"name": "Mac", "platform": "macos", "pubkey": "AAAA"}),
        json!({"name": "", "platform": "ios", "pubkey": b64url(&[1u8; 32])}),
    ] {
        let (s, _) = t
            .req(
                "POST",
                "/auth/email/finish",
                None,
                Some(json!({"email": "b@napple.test", "code": code, "device": d})),
            )
            .await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
    }
    let (s, _) = t.req("GET", "/me", None, None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let (s, _) = t.req("GET", "/me", Some("garbage.token.here"), None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn passkey_register_and_login() {
    let t = T::new();
    let a = t.sign_in("pk@napple.test", "Mac").await;
    let (s, begin) = t
        .req(
            "POST",
            "/auth/passkey/register/begin",
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{begin}");
    assert_eq!(begin["publicKey"]["rp"]["id"], "napple.lat");
    assert_eq!(
        begin["publicKey"]["authenticatorSelection"]["userVerification"],
        "required"
    );
    let mut auth = SoftAuthenticator::new("napple.lat", ORIGIN);
    let cred = auth.create(&begin["publicKey"]);
    let (s, v) = t.req("POST", "/auth/passkey/register/finish", Some(&a.access), Some(json!({"challenge_id": begin["challenge_id"], "name": "Mac passkey", "credential": cred}))).await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    // The ceremony is single use.
    let (s, _) = t
        .req(
            "POST",
            "/auth/passkey/register/finish",
            Some(&a.access),
            Some(json!({"challenge_id": begin["challenge_id"], "credential": cred})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);

    // Login on a new device, no email needed.
    let (s, lb) = t.req("POST", "/auth/passkey/login/begin", None, None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(lb["publicKey"]["allowCredentials"], json!([]));
    let assertion = auth.get(&lb["publicKey"], true);
    let (s, v) = t.req("POST", "/auth/passkey/login/finish", None, Some(json!({"challenge_id": lb["challenge_id"], "credential": assertion, "device": dev("iPhone")}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["account_id"], a.account_id.as_str());
    assert_eq!(v["key_state"], "bootstrap");

    // Replaying the same assertion against a new challenge fails (challenge + counter).
    let (_, lb2) = t.req("POST", "/auth/passkey/login/begin", None, None).await;
    let (s, _) = t.req("POST", "/auth/passkey/login/finish", None, Some(json!({"challenge_id": lb2["challenge_id"], "credential": assertion, "device": dev("iPhone")}))).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);

    // Wrong origin (phishing site) fails.
    let mut phish = SoftAuthenticator {
        origin: "https://napple-login.example".into(),
        ..SoftAuthenticator::new("napple.lat", ORIGIN)
    };
    phish.key = auth.key.clone();
    phish.cred_id = auth.cred_id.clone();
    phish.user_handle = auth.user_handle.clone();
    phish.counter = 50;
    let (_, lb3) = t.req("POST", "/auth/passkey/login/begin", None, None).await;
    let (s, _) = t.req("POST", "/auth/passkey/login/finish", None, Some(json!({"challenge_id": lb3["challenge_id"], "credential": phish.get(&lb3["publicKey"], true), "device": dev("x")}))).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);

    // Removing the passkey disables it.
    let (_, list) = t.req("GET", "/passkeys", Some(&a.access), None).await;
    assert_eq!(list["passkeys"].as_array().unwrap().len(), 1);
    let id = list["passkeys"][0]["credential_id"]
        .as_str()
        .unwrap()
        .to_string();
    let (s, _) = t
        .req("DELETE", &format!("/passkeys/{id}"), Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (_, lb4) = t.req("POST", "/auth/passkey/login/begin", None, None).await;
    let (s, _) = t.req("POST", "/auth/passkey/login/finish", None, Some(json!({"challenge_id": lb4["challenge_id"], "credential": auth.get(&lb4["publicKey"], true), "device": dev("x")}))).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn refresh_rotates_and_reuse_ends_the_device_session() {
    let t = T::new();
    let a = t.sign_in("r@napple.test", "Mac").await;
    let (s, r1) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{r1}");
    assert_ne!(r1["refresh_token"], a.refresh.as_str());
    let new_access = r1["access_token"].as_str().unwrap().to_string();
    let (s, _) = t.req("GET", "/me", Some(&new_access), None).await;
    assert_eq!(s, StatusCode::OK);

    // The old refresh token comes back (stolen copy): reuse detected, everything for that device ends.
    let (s, v) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    assert_eq!(v["error"], "refresh_reused");
    let (s, _) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": r1["refresh_token"]})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED, "the rotated token is dead too");
    let (s, _) = t.req("GET", "/me", Some(&new_access), None).await;
    assert_eq!(
        s,
        StatusCode::UNAUTHORIZED,
        "access tokens of the device are dead"
    );

    // Signing in again on the same device key reuses the device.
    let again = t
        .sign_in_with("r@napple.test", "Mac", "macos", &a.pubkey)
        .await;
    assert_eq!(again.device_id, a.device_id);

    // Access tokens expire after 15 minutes; refresh tokens after 30 days.
    t.clock.advance(16 * 60);
    let (s, _) = t.req("GET", "/me", Some(&again.access), None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    t.clock.advance(31 * DAY);
    let (s, _) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": again.refresh})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn logout_and_device_revoke() {
    let t = T::new();
    let a = t.sign_in("d@napple.test", "Mac").await;
    let b = t
        .sign_in_with("d@napple.test", "iPhone", "ios", &random_bytes::<32>())
        .await;
    let (_, list) = t.req("GET", "/devices", Some(&a.access), None).await;
    let devs = list["devices"].as_array().unwrap();
    assert_eq!(devs.len(), 2);
    assert!(
        devs.iter()
            .any(|d| d["platform"] == "ios" && d["current"] == false)
    );

    let (s, _) = t
        .req(
            "DELETE",
            &format!("/devices/{}", b.device_id),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (s, _) = t.req("GET", "/me", Some(&b.access), None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let (s, _) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": b.refresh})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let (_, list) = t.req("GET", "/devices", Some(&a.access), None).await;
    assert_eq!(list["devices"].as_array().unwrap().len(), 1);
    // Another account cannot revoke this device.
    let other = t.sign_in("other@napple.test", "Mac").await;
    let (s, _) = t
        .req(
            "DELETE",
            &format!("/devices/{}", a.device_id),
            Some(&other.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);

    let (s, _) = t.req("POST", "/auth/logout", Some(&a.access), None).await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (s, _) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
    let (s, _) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn jwks_verifies_access_tokens_like_napple_online_will() {
    let t = T::new();
    let a = t.sign_in("j@napple.test", "Mac").await;
    let (s, jwks) = t.req("GET", "/.well-known/jwks.json", None, None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(jwks["keys"][0]["kty"], "OKP");
    assert_eq!(jwks["keys"][0]["crv"], "Ed25519");
    let c = napple_account::jwt::verify_with_jwks(
        &jwks,
        &a.access,
        "https://pretend.napple.lat/account/v1",
        T0 + 1,
    )
    .unwrap();
    assert_eq!(c.sub, a.account_id);
    assert_eq!(c.did, a.device_id);
    assert_eq!(c.aud, "napple");
    assert_eq!(c.prf.len(), 1, "the first profile is in the token");
    let (s, _, body) = t
        .raw(
            Request::builder()
                .uri("/healthz")
                .body(Body::empty())
                .unwrap(),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    assert!(String::from_utf8(body).unwrap().starts_with("ok"));
    let (s, _) = t.req("GET", "/healthz", None, None).await;
    assert_eq!(s, StatusCode::OK);
}

#[tokio::test]
async fn cors_only_for_napple_origins() {
    let t = T::new();
    let pre = |origin: &str| {
        Request::builder()
            .method("OPTIONS")
            .uri("/account/v1/me")
            .header("origin", origin)
            .header("access-control-request-method", "GET")
            .header("access-control-request-headers", "authorization")
            .body(Body::empty())
            .unwrap()
    };
    let (_, h, _) = t.raw(pre("https://napple.lat")).await;
    assert_eq!(
        h.get("access-control-allow-origin").unwrap(),
        "https://napple.lat"
    );
    let (_, h, _) = t.raw(pre("https://evil.example")).await;
    assert!(h.get("access-control-allow-origin").is_none());
}
