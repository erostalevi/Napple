//! Accounts: signup / verify / login / password reset with emailed codes, lockout, rate limits, no enumeration,
//! plus refresh rotation and reuse detection, devices, JWKS, CORS. Passkeys and magic links are gone.

mod common;
use axum::body::Body;
use axum::http::{Request, StatusCode};
use common::*;
use napple_account::util::{b64url, random_bytes};
use serde_json::{Value, json};

fn dev(name: &str) -> Value {
    json!({"name": name, "platform": "macos", "pubkey": b64url(&random_bytes::<32>())})
}

fn signup_body(email: &str, nick: &str) -> Value {
    json!({"email": email, "password": PASSWORD, "nickname": nick, "age_ok": true, "terms_ok": true, "device": dev("Mac")})
}

async fn signup(t: &T, email: &str, nick: &str) -> (StatusCode, Value) {
    t.req("POST", "/auth/signup", None, Some(signup_body(email, nick))).await
}

async fn login(t: &T, email: &str, password: &str) -> (StatusCode, Value) {
    t.req("POST", "/auth/login", None, Some(json!({"email": email, "password": password, "device": dev("Mac")}))).await
}

#[tokio::test]
async fn signup_verify_login_reset_whole_flow() {
    let t = T::new();
    let (s, v) = signup(&t, "Player@Example.com", "Player_One").await;
    assert_eq!(s, StatusCode::ACCEPTED, "{v}");
    assert_eq!(v["expires_in"], 600);

    // The mail is multipart: text with the code, HTML with the code, nothing unrendered.
    let mail = t.mail.last_to("player@example.com").unwrap();
    let code = t.code_for("player@example.com");
    assert!(mail.text.contains(&code) && !mail.text.contains("{{"));
    let html = mail.html.as_ref().unwrap();
    assert!(html.contains(&code) && !html.contains("{{"));
    assert_eq!(mail.subject, "Your Napple confirmation code");

    // Not usable before the code: login says unverified and sends a fresh code.
    let (s, v) = login(&t, "player@example.com", PASSWORD).await;
    assert_eq!(s, StatusCode::FORBIDDEN);
    assert_eq!(v["reason"], "unverified");
    assert_eq!(t.mail.count(), 2);
    let code = t.code_for("player@example.com");

    let wrong = if code == "000000" { "111111" } else { "000000" };
    let (s, v) = t.req("POST", "/auth/verify", None, Some(json!({"email": "player@example.com", "code": wrong}))).await;
    assert_eq!((s, v["error_code"].as_str()), (StatusCode::BAD_REQUEST, Some("invalid_code")));

    let (s, v) = t.req("POST", "/auth/verify", None, Some(json!({"email": "PLAYER@example.com", "code": code}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["new_account"], true);
    assert_eq!(v["key_state"], "bootstrap");
    assert_eq!(v["expires_in"], 900);
    let access = v["access_token"].as_str().unwrap().to_string();
    let account = v["account_id"].as_str().unwrap().to_string();

    // Single use.
    let (s, _) = t.req("POST", "/auth/verify", None, Some(json!({"email": "player@example.com", "code": code}))).await;
    assert_eq!(s, StatusCode::BAD_REQUEST);

    // The first profile carries the signup nickname; /me works; terms were recorded.
    let (_, p) = t.req("GET", "/profiles", Some(&access), None).await;
    assert_eq!(p["profiles"][0]["nickname"], "Player_One");
    let (s, me) = t.req("GET", "/me", Some(&access), None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(me["email"], "player@example.com");
    let (terms, at): (String, i64) = t.app.db().query_row("SELECT terms_version, terms_ok_at FROM accounts WHERE id = ?1", [&account], |r| Ok((r.get(0)?, r.get(1)?))).unwrap();
    assert_eq!((terms.as_str(), at), (napple_account::terms::TERMS_VERSION, T0));
    let age: i64 = t.app.db().query_row("SELECT age_ok_at FROM accounts WHERE id = ?1", [&account], |r| r.get(0)).unwrap();
    assert_eq!(age, T0);

    // Login on a second device: same account, different device.
    let (s, v) = login(&t, "player@example.com", PASSWORD).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["account_id"], account.as_str());
    assert_eq!(v["new_account"], false);
    assert_ne!(v["device_id"], json!(null));
    let other_access = v["access_token"].as_str().unwrap().to_string();

    // Reset: new password, old one dead, every session ended, new one works.
    let (s, _) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "player@example.com"}))).await;
    assert_eq!(s, StatusCode::ACCEPTED);
    let rmail = t.mail.last_to("player@example.com").unwrap();
    assert_eq!(rmail.subject, "Your Napple password reset code");
    let rcode = t.code_for("player@example.com");
    let new_pw = "another long passphrase";
    let (s, v) = t.req("POST", "/auth/password/reset/finish", None, Some(json!({"email": "player@example.com", "code": rcode, "new_password": new_pw}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let (s, _) = t.req("POST", "/auth/password/reset/finish", None, Some(json!({"email": "player@example.com", "code": rcode, "new_password": new_pw}))).await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "reset codes are single use");
    assert_eq!(login(&t, "player@example.com", PASSWORD).await.0, StatusCode::UNAUTHORIZED);
    assert_eq!(login(&t, "player@example.com", new_pw).await.0, StatusCode::OK);
    assert_eq!(t.req("GET", "/me", Some(&access), None).await.0, StatusCode::UNAUTHORIZED);
    assert_eq!(t.req("GET", "/me", Some(&other_access), None).await.0, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn codes_expire_and_die_after_five_wrong_guesses() {
    let t = T::new();
    signup(&t, "c@example.com", "Coder_One").await;
    let code = t.code_for("c@example.com");
    t.clock.advance(11 * 60);
    let (s, v) = t.req("POST", "/auth/verify", None, Some(json!({"email": "c@example.com", "code": code}))).await;
    assert_eq!((s, v["error_code"].as_str()), (StatusCode::BAD_REQUEST, Some("invalid_code")));

    // New code (re-signup of the unverified address); five wrong guesses burn it, even the right one after.
    signup(&t, "c@example.com", "Coder_One").await;
    let code = t.code_for("c@example.com");
    let wrong = if code == "123456" { "654321" } else { "123456" };
    for _ in 0..5 {
        let (s, _) = t.req("POST", "/auth/verify", None, Some(json!({"email": "c@example.com", "code": wrong}))).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
    }
    let (s, _) = t.req("POST", "/auth/verify", None, Some(json!({"email": "c@example.com", "code": code}))).await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "locked out after 5 attempts");
}

#[tokio::test]
async fn bad_signup_input_is_refused() {
    let t = T::new();
    let mut cases: Vec<(Value, &str)> = Vec::new();
    let base = signup_body("ok@example.com", "Good_Name");
    let with = |k: &str, v: Value| {
        let mut b = base.clone();
        b[k] = v;
        b
    };
    cases.push((with("email", json!("nope")), "bad_request"));
    cases.push((with("password", json!("short")), "weak_password"));
    cases.push((with("password", json!("Password123")), "weak_password"));
    cases.push((with("password", json!("ok@example.com")), "weak_password"));
    cases.push((with("age_ok", json!(false)), "terms_required"));
    cases.push((with("terms_ok", json!(false)), "terms_required"));
    cases.push((with("nickname", json!("ab")), "bad_nickname"));
    cases.push((with("nickname", json!("has space")), "bad_nickname"));
    cases.push((with("nickname", json!("a".repeat(21))), "bad_nickname"));
    cases.push((with("nickname", json!("My_Napple")), "reserved_nickname"));
    for (body, want) in cases {
        let (s, v) = t.req("POST", "/auth/signup", None, Some(body.clone())).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{body} -> {v}");
        let got = v["error_code"].as_str().unwrap_or(v["error"].as_str().unwrap());
        assert_eq!(got, want, "{body}");
    }
    assert_eq!(t.mail.count(), 0, "nothing mailed for refused signups");
    // age/terms omitted entirely
    let (s, v) = t.req("POST", "/auth/signup", None, Some(json!({"email": "ok@example.com", "password": PASSWORD, "nickname": "Good_Name", "device": dev("Mac")}))).await;
    assert_eq!((s, v["error_code"].as_str()), (StatusCode::BAD_REQUEST, Some("terms_required")));
}

#[tokio::test]
async fn nicknames_are_unique_case_insensitively() {
    let t = T::new();
    let (s, _) = signup(&t, "a@example.com", "Racer_X").await;
    assert_eq!(s, StatusCode::ACCEPTED);
    // pending (unverified) nickname already reserved
    let (s, v) = signup(&t, "b@example.com", "racer_x").await;
    assert_eq!((s, v["error"].as_str()), (StatusCode::CONFLICT, Some("nickname_taken")));
    let code = t.code_for("a@example.com");
    t.req("POST", "/auth/verify", None, Some(json!({"email": "a@example.com", "code": code}))).await;
    let (s, v) = signup(&t, "b@example.com", "RACER_X").await;
    assert_eq!((s, v["error"].as_str()), (StatusCode::CONFLICT, Some("nickname_taken")));
}

#[tokio::test]
async fn no_enumeration_unknown_or_known_look_the_same() {
    let t = T::new();
    t.sign_in("known@example.com", "Mac").await;
    let before = t.mail.count();

    // login: unknown email and wrong password give the identical answer
    let (s1, v1) = login(&t, "known@example.com", "not the password at all").await;
    let (s2, v2) = login(&t, "ghost@example.com", "not the password at all").await;
    assert_eq!((s1, &v1), (s2, &v2));
    assert_eq!(s1, StatusCode::UNAUTHORIZED);
    assert_eq!(v1["error"], "invalid_credentials");

    // reset start: always 202 with the same body; mail only for the real account
    let (s1, v1) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "known@example.com"}))).await;
    let (s2, v2) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "ghost@example.com"}))).await;
    assert_eq!((s1, &v1), (s2, &v2));
    assert_eq!(s1, StatusCode::ACCEPTED);
    assert_eq!(t.mail.count(), before + 1);
    assert!(t.mail.last_to("ghost@example.com").is_none());

    // signup for an address that already has an account: same 202, no mail, password unchanged
    let (s, v) = signup(&t, "known@example.com", "Intruder_1").await;
    assert_eq!(s, StatusCode::ACCEPTED, "{v}");
    assert_eq!(t.mail.count(), before + 1);
    assert_eq!(login(&t, "known@example.com", PASSWORD).await.0, StatusCode::OK);

    // reset finish for an unknown address: same answer as a wrong code
    let (s1, v1) = t.req("POST", "/auth/password/reset/finish", None, Some(json!({"email": "ghost@example.com", "code": "123456", "new_password": "a brand new passphrase"}))).await;
    let (s2, v2) = t.req("POST", "/auth/password/reset/finish", None, Some(json!({"email": "known@example.com", "code": "000001", "new_password": "a brand new passphrase"}))).await;
    assert_eq!((s1, &v1), (s2, &v2));
}

#[tokio::test]
async fn ten_failed_logins_lock_the_address_for_fifteen_minutes() {
    let t = T::new();
    t.sign_in("lock@example.com", "Mac").await;
    assert_eq!(login(&t, "lock@example.com", PASSWORD).await.0, StatusCode::OK); // a success resets the harness's probe failure
    for i in 0..10 {
        let (s, _) = login(&t, "lock@example.com", "wrong password here").await;
        assert_eq!(s, StatusCode::UNAUTHORIZED, "attempt {i}");
        t.clock.advance(20); // stay under the per-IP minute limit
    }
    // locked: even the right password is refused, and the answer is a 429 with retry_after
    let (s, v) = login(&t, "lock@example.com", PASSWORD).await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS, "{v}");
    assert!(v["retry_after"].as_u64().unwrap() > 0);
    // an address with no account locks the same way
    for _ in 0..10 {
        login(&t, "nobody@example.com", "wrong password here").await;
        t.clock.advance(20);
    }
    let (s, _) = login(&t, "nobody@example.com", "wrong password here").await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS);
    // after 15 minutes the lock is gone and a success clears the counter
    t.clock.advance(16 * 60);
    assert_eq!(login(&t, "lock@example.com", PASSWORD).await.0, StatusCode::OK);
}

#[tokio::test]
async fn reset_lifts_a_lockout() {
    let t = T::new();
    t.sign_in("l2@example.com", "Mac").await;
    assert_eq!(login(&t, "l2@example.com", PASSWORD).await.0, StatusCode::OK);
    for _ in 0..10 {
        login(&t, "l2@example.com", "wrong password here").await;
        t.clock.advance(20);
    }
    assert_eq!(login(&t, "l2@example.com", PASSWORD).await.0, StatusCode::TOO_MANY_REQUESTS);
    t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "l2@example.com"}))).await;
    let code = t.code_for("l2@example.com");
    let (s, _) = t.req("POST", "/auth/password/reset/finish", None, Some(json!({"email": "l2@example.com", "code": code, "new_password": "fresh passphrase 42"}))).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(login(&t, "l2@example.com", "fresh passphrase 42").await.0, StatusCode::OK);
}

#[tokio::test]
async fn code_mails_are_limited_per_address_and_per_ip() {
    let t = T::new();
    // 5 per hour per address (reset start counts for known and unknown alike)
    for i in 0..5 {
        let (s, v) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "spam@example.com"}))).await;
        assert_eq!(s, StatusCode::ACCEPTED, "{i}: {v}");
        t.clock.advance(5);
    }
    let (s, v) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "spam@example.com"}))).await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS, "{v}");
    t.clock.advance(3600);
    let (s, _) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "spam@example.com"}))).await;
    assert_eq!(s, StatusCode::ACCEPTED);

    // 20 per hour per IP across addresses
    let t = T::new();
    for i in 0..20 {
        let (s, v) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": format!("u{i}@example.com")}))).await;
        assert_eq!(s, StatusCode::ACCEPTED, "{i}: {v}");
        t.clock.advance(3);
    }
    let (s, _) = t.req("POST", "/auth/password/reset/start", None, Some(json!({"email": "u99@example.com"}))).await;
    assert_eq!(s, StatusCode::TOO_MANY_REQUESTS);
}

#[tokio::test]
async fn undeliverable_addresses_are_suppressed_and_not_retried() {
    let t = T::new();
    t.mail.suppress("bounce@example.com");
    // The caller sees the usual 202 (no oracle); the address is recorded and later signups send nothing.
    let (s, _) = signup(&t, "bounce@example.com", "Bouncer_1").await;
    assert_eq!(s, StatusCode::ACCEPTED);
    let n: i64 = t.app.db().query_row("SELECT COUNT(*) FROM mail_suppressions WHERE email = 'bounce@example.com'", [], |r| r.get(0)).unwrap();
    assert_eq!(n, 1);
    assert_eq!(t.mail.count(), 0);
    let codes: i64 = t.app.db().query_row("SELECT COUNT(*) FROM auth_codes", [], |r| r.get(0)).unwrap();
    assert_eq!(codes, 0, "no live code for an undeliverable address");
    t.mail.suppressed.lock().unwrap().clear();
    let (s, _) = signup(&t, "bounce@example.com", "Bouncer_1").await;
    assert_eq!(s, StatusCode::ACCEPTED);
    assert_eq!(t.mail.count(), 0, "suppressed address is never mailed again");
}

#[tokio::test]
async fn deleting_the_account_removes_everything_and_frees_the_address() {
    let t = T::new();
    let a = t.sign_in("del@example.com", "Mac").await;
    let (s, _) = t.req("DELETE", "/me", Some(&a.access), Some(json!({"confirm_email": "del@example.com"}))).await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert_eq!(t.req("GET", "/me", Some(&a.access), None).await.0, StatusCode::UNAUTHORIZED);
    assert_eq!(login(&t, "del@example.com", PASSWORD).await.0, StatusCode::UNAUTHORIZED);
    let b = t.sign_in("del@example.com", "Mac").await;
    assert_ne!(a.account_id, b.account_id);
}

#[tokio::test]
async fn retention_removes_unconfirmed_signups() {
    let t = T::new();
    signup(&t, "stale@example.com", "Stale_1").await;
    t.clock.advance(2 * DAY);
    let r = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!(r.unverified_removed, 1);
    let n: i64 = t.app.db().query_row("SELECT COUNT(*) FROM accounts", [], |r| r.get(0)).unwrap();
    assert_eq!(n, 0);
}

#[tokio::test]
async fn old_magic_link_and_passkey_routes_are_gone() {
    let t = T::new();
    for (m, p) in [
        ("POST", "/auth/email/start"),
        ("POST", "/auth/email/finish"),
        ("GET", "/auth/email/link"),
        ("POST", "/auth/passkey/login/begin"),
        ("POST", "/auth/passkey/register/begin"),
        ("GET", "/passkeys"),
    ] {
        let (s, _) = t.req(m, p, None, Some(json!({}))).await;
        assert!(s == StatusCode::NOT_FOUND || s == StatusCode::METHOD_NOT_ALLOWED, "{p} -> {s}");
    }
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

#[tokio::test]
async fn signup_yields_a_valid_unique_first_profile_nickname() {
    let t = T::new();
    // Plain case: the profile has the nickname, a friend code, and goes through profiles::insert.
    let (s, _) = signup(&t, "a@example.com", "Racer_X").await;
    assert_eq!(s, StatusCode::ACCEPTED);
    // A second account verifies first and ends up owning the nickname (simulating the race the pending reservation cannot cover).
    let (s, _) = signup(&t, "c@example.com", "Taker").await;
    assert_eq!(s, StatusCode::ACCEPTED);
    let code = t.code_for("c@example.com");
    let (s, v) = t.req("POST", "/auth/verify", None, Some(json!({"email": "c@example.com", "code": code}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    t.app.db().execute("UPDATE profiles SET nickname = 'racer_x' WHERE nickname = 'Taker'", []).unwrap();

    let code = t.code_for("a@example.com");
    let (s, v) = t.req("POST", "/auth/verify", None, Some(json!({"email": "a@example.com", "code": code}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let access = v["access_token"].as_str().unwrap();
    let (_, p) = t.req("GET", "/profiles", Some(access), None).await;
    let first = &p["profiles"][0];
    let nick = first["nickname"].as_str().unwrap();
    assert_ne!(nick.to_lowercase(), "racer_x", "the taken nickname must not be reused");
    assert!(nick.starts_with("Racer_X"), "{nick}");
    assert!(napple_account::nicknames::valid_charset(nick), "{nick}");
    assert!(first["friend_code"].as_str().unwrap().starts_with("NP-"), "{first}");
    let dup: i64 = t.app.db().query_row("SELECT COUNT(*) FROM profiles WHERE nickname = ?1 COLLATE NOCASE", [nick], |r| r.get(0)).unwrap();
    assert_eq!(dup, 1);
}
