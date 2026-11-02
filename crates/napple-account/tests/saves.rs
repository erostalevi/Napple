//! Profiles, save versions (create / conflict / commit / list / prune), quota, the Free title rule, account deletion.

mod common;
use axum::http::StatusCode;
use common::*;
use napple_account::blob::SAVE_CONTENT_TYPE;
use napple_account::util::sha256_hex;
use serde_json::json;

#[tokio::test]
async fn profiles_crud_limit_and_token_claims() {
    let t = T::new();
    let a = t.sign_in("prof@napple.test", "Mac").await;
    let (_, v) = t.req("GET", "/profiles", Some(&a.access), None).await;
    assert_eq!(
        v["profiles"].as_array().unwrap().len(),
        1,
        "one profile is created with the account"
    );
    assert_eq!(v["profiles"][0]["nickname"], "prof");
    for i in 1..8 {
        let (s, p) = t
            .req(
                "POST",
                "/profiles",
                Some(&a.access),
                Some(json!({"nickname": format!("Player {i}"), "icon_id": i})),
            )
            .await;
        assert_eq!(s, StatusCode::CREATED, "{p}");
    }
    let (s, v) = t
        .req(
            "POST",
            "/profiles",
            Some(&a.access),
            Some(json!({"nickname": "Ninth"})),
        )
        .await;
    assert_eq!(s, StatusCode::CONFLICT);
    assert_eq!(v["error"], "profile_limit");
    let (s, _) = t
        .req(
            "POST",
            "/profiles",
            Some(&a.access),
            Some(json!({"nickname": "bad\u{0007}name"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);

    let pid = t.first_profile(&a).await;
    let (s, p) = t
        .req(
            "PATCH",
            &format!("/profiles/{pid}"),
            Some(&a.access),
            Some(json!({"nickname": "Neto", "icon_id": 42})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(p["nickname"], "Neto");
    assert_eq!(p["icon_id"], 42);

    // A refreshed token lists all 8 profiles (napple-online maps profile_id -> online identity).
    let (_, r) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    let jwks = t.app.jwt.jwks();
    let c = napple_account::jwt::verify_with_jwks(
        &jwks,
        r["access_token"].as_str().unwrap(),
        &t.app.cfg.issuer,
        T0 + 1,
    )
    .unwrap();
    assert_eq!(c.prf.len(), 8);
    assert!(c.prf.contains(&pid));

    // Other accounts cannot see or touch it.
    let b = t.sign_in("intruder@napple.test", "Mac").await;
    let (s, _) = t
        .req("GET", &format!("/profiles/{pid}"), Some(&b.access), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = t
        .req(
            "PATCH",
            &format!("/profiles/{pid}"),
            Some(&b.access),
            Some(json!({"nickname": "pwned"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{pid}/versions"),
            Some(&b.access),
            Some(json!({"base_version": 0, "size": 10, "sha256_ciphertext": sha256_hex(b"x")})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);

    // Deleting a profile with saves needs the explicit flag.
    let (s, _) = t.push(&a, LAS, &pid, 0, b"ciphertext-1").await;
    assert_eq!(s, StatusCode::OK);
    let (s, v) = t
        .req("DELETE", &format!("/profiles/{pid}"), Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::CONFLICT);
    assert_eq!(v["error"], "profile_has_saves");
    let (s, _) = t
        .req(
            "DELETE",
            &format!("/profiles/{pid}?delete_saves=true"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (_, l) = t.req("GET", "/saves", Some(&a.access), None).await;
    assert_eq!(l["saves"].as_array().unwrap().len(), 0);
}

#[tokio::test]
async fn version_create_conflict_commit_list_download() {
    let t = T::new();
    let a = t.sign_in("saves@napple.test", "Mac").await;
    let b = t.sign_in("saves@napple.test", "iPhone").await;
    let p = t.first_profile(&a).await;

    let data1 = b"NAPPLESV\x01 first ciphertext".to_vec();
    let (s, v1) = t.push(&a, LAS, &p, 0, &data1).await;
    assert_eq!(s, StatusCode::OK, "{v1}");
    assert_eq!(v1["version"], 1);
    assert_eq!(v1["device_name"], "Mac");

    // B still thinks there is nothing in the cloud: 409 with the head so it can keep both.
    let (s, c) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&b.access),
            Some(json!({"base_version": 0, "size": 5, "sha256_ciphertext": sha256_hex(b"hello")})),
        )
        .await;
    assert_eq!(s, StatusCode::CONFLICT);
    assert_eq!(c["error"], "version_conflict");
    assert_eq!(c["head"]["version"], 1);
    assert_eq!(c["head"]["sha256_ciphertext"], sha256_hex(&data1).as_str());

    // Both devices race on base 1: the first commit wins, the second gets 409 at commit.
    let mk = |s: &Session, d: &[u8]| {
        let d = d.to_vec();
        let tok = s.access.clone();
        let p = p.clone();
        let t = &t;
        async move {
            t.req("POST", &format!("/saves/{LAS}/{p}/versions"), Some(&tok), Some(json!({"base_version": 1, "size": d.len(), "sha256_ciphertext": sha256_hex(&d)}))).await
        }
    };
    let (sa, ca) = mk(&a, b"from A v2").await;
    let (sb, cb) = mk(&b, b"from B v2").await;
    assert_eq!((sa, sb), (StatusCode::CREATED, StatusCode::CREATED));
    assert!(
        ca["upload_url"]
            .as_str()
            .unwrap()
            .contains("/dev-blobs/saves/")
    );
    assert_eq!(ca["upload_headers"]["content-type"], SAVE_CONTENT_TYPE);
    assert_eq!(
        t.dev_put(&ca, b"from A v2", SAVE_CONTENT_TYPE).await,
        StatusCode::OK
    );
    assert_eq!(
        t.dev_put(&cb, b"from B v2", SAVE_CONTENT_TYPE).await,
        StatusCode::OK
    );
    let (s, _) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/2/commit"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/2/commit"),
            Some(&b.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::CONFLICT);
    assert_eq!(v["head"]["device_name"], "Mac");

    // Commit before upload: refused. Wrong body / wrong type at the store: refused.
    let (_, c3) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&b.access),
            Some(json!({"base_version": 2, "size": 4, "sha256_ciphertext": sha256_hex(b"v3!!")})),
        )
        .await;
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/3/commit"),
            Some(&b.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
    assert_eq!(v["error_code"], "upload_missing");
    assert_eq!(
        t.dev_put(&c3, b"v3??", SAVE_CONTENT_TYPE).await,
        StatusCode::BAD_REQUEST,
        "checksum mismatch"
    );
    assert_eq!(
        t.dev_put(&c3, b"v3!!", "application/zip").await,
        StatusCode::BAD_REQUEST,
        "content type"
    );
    assert_eq!(
        t.dev_put(&c3, b"v3!!", SAVE_CONTENT_TYPE).await,
        StatusCode::OK
    );
    let (s, _) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/3/commit"),
            Some(&b.access),
            Some(json!({"upload_id": c3["upload_id"]})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);

    // List + history + download of an older version.
    let (_, l) = t.req("GET", "/saves", Some(&a.access), None).await;
    let e = &l["saves"][0];
    assert_eq!(
        (
            e["title_id"].as_str(),
            e["latest_version"].as_i64(),
            e["device_name"].as_str(),
            e["read_only"].as_bool()
        ),
        (Some(LAS), Some(3), Some("iPhone"), Some(false))
    );
    let (_, h) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&a.access),
            None,
        )
        .await;
    let vs: Vec<i64> = h["versions"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v["version"].as_i64().unwrap())
        .collect();
    assert_eq!(vs, vec![3, 2, 1]);
    let (s, g) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions/1"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, body) = t.dev_get(g["download_url"].as_str().unwrap()).await;
    assert_eq!((s, body), (StatusCode::OK, data1.clone()));
    // Download URLs expire.
    t.clock.advance(16 * 60);
    let (s, _) = t.dev_get(g["download_url"].as_str().unwrap()).await;
    assert_eq!(s, StatusCode::FORBIDDEN);

    // Limits: size and title id.
    let (s, _) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(
        s,
        StatusCode::UNAUTHORIZED,
        "access token expired with the clock move"
    );
    let (_, r) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    let tok = r["access_token"].as_str().unwrap();
    let (s, _) = t.req("POST", &format!("/saves/{LAS}/{p}/versions"), Some(tok), Some(json!({"base_version": 3, "size": 64 * 1024 * 1024 + 1, "sha256_ciphertext": sha256_hex(b"x")}))).await;
    assert_eq!(s, StatusCode::PAYLOAD_TOO_LARGE);
    let (s, _) = t
        .req(
            "POST",
            &format!("/saves/not-a-title/{p}/versions"),
            Some(tok),
            Some(json!({"base_version": 3, "size": 1, "sha256_ciphertext": sha256_hex(b"x")})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
}

#[tokio::test]
async fn prune_keeps_the_plan_version_count() {
    let t = T::with(&[("NAPPLE_PLAN_FREE_VERSIONS", "3")], None);
    let a = t.sign_in("prune@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    for v in 0..6 {
        let (s, c) = t
            .push(&a, SMO, &p, v, format!("v{}", v + 1).as_bytes())
            .await;
        assert_eq!(s, StatusCode::OK, "{c}");
    }
    let (_, h) = t
        .req(
            "GET",
            &format!("/saves/{SMO}/{p}/versions"),
            Some(&a.access),
            None,
        )
        .await;
    let vs: Vec<i64> = h["versions"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v["version"].as_i64().unwrap())
        .collect();
    assert_eq!(vs, vec![6, 5, 4]);
    let files = walk(&t.dir.join("blobs"));
    assert_eq!(files, 3, "pruned objects are deleted from the store");
}

#[tokio::test]
async fn default_prune_is_twenty_on_plus() {
    let t = T::with_stripe();
    let a = t.sign_in("prune20@napple.test", "Mac").await;
    let (s, _) = t
        .webhook(&fixture("subscription_created.json", &a.account_id, T0))
        .await;
    assert_eq!(s, StatusCode::OK);
    let p = t.first_profile(&a).await;
    for v in 0..22 {
        assert_eq!(
            t.push(&a, SMO, &p, v, format!("v{}", v + 1).as_bytes())
                .await
                .0,
            StatusCode::OK
        );
    }
    let (_, h) = t
        .req(
            "GET",
            &format!("/saves/{SMO}/{p}/versions"),
            Some(&a.access),
            None,
        )
        .await;
    let vs = h["versions"].as_array().unwrap();
    assert_eq!(vs.len(), 20);
    assert_eq!(vs.last().unwrap()["version"], 3);
}

fn walk(d: &std::path::Path) -> usize {
    std::fs::read_dir(d)
        .map(|it| {
            it.flatten()
                .map(|e| {
                    if e.path().is_dir() {
                        walk(&e.path())
                    } else {
                        1
                    }
                })
                .sum()
        })
        .unwrap_or(0)
}

#[tokio::test]
async fn quota_returns_402_on_free() {
    let t = T::with(&[("NAPPLE_PLAN_FREE_QUOTA", "100")], None);
    let a = t.sign_in("quota@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    assert_eq!(t.push(&a, LAS, &p, 0, &[1u8; 60]).await.0, StatusCode::OK);
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&a.access),
            Some(
                json!({"base_version": 1, "size": 50, "sha256_ciphertext": sha256_hex(&[2u8; 50])}),
            ),
        )
        .await;
    assert_eq!(s, StatusCode::PAYMENT_REQUIRED);
    assert_eq!(v["reason"], "quota");
    assert_eq!(v["used"], 60);
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["plan"]["quota"], 100);
    assert_eq!(me["plan"]["used"], 60);
}

#[tokio::test]
async fn free_plan_one_title_switch_every_30_days() {
    let t = T::new();
    let a = t.sign_in("free@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    assert_eq!(t.push(&a, LAS, &p, 0, b"las v1").await.0, StatusCode::OK);

    // A second title: 402 plan_limit with the upgrade URL; nothing is created.
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{SMO}/{p}/versions"),
            Some(&a.access),
            Some(json!({"base_version": 0, "size": 6, "sha256_ciphertext": sha256_hex(b"smo v1")})),
        )
        .await;
    assert_eq!(s, StatusCode::PAYMENT_REQUIRED, "{v}");
    assert_eq!(v["reason"], "plan_limit");
    assert_eq!(
        v["upgrade_url"],
        "https://pretend.napple.lat/account/v1/billing"
    );

    // The user switches the synced title to SMO: LAS becomes read-only, still downloadable.
    let (s, v) = t
        .req(
            "POST",
            "/saves/titles",
            Some(&a.access),
            Some(json!({"title_id": SMO})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let states: Vec<(String, String)> = v["titles"]
        .as_array()
        .unwrap()
        .iter()
        .map(|x| {
            (
                x["title_id"].as_str().unwrap().into(),
                x["state"].as_str().unwrap().into(),
            )
        })
        .collect();
    assert!(
        states.contains(&(LAS.into(), "readonly".into()))
            && states.contains(&(SMO.into(), "active".into()))
    );
    assert_eq!(v["next_switch_at"], T0 + 30 * DAY);
    assert_eq!(t.push(&a, SMO, &p, 0, b"smo v1").await.0, StatusCode::OK);
    let (s, g) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions/1"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK, "read-only saves stay downloadable");
    assert_eq!(
        t.dev_get(g["download_url"].as_str().unwrap()).await.1,
        b"las v1"
    );
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&a.access),
            Some(json!({"base_version": 1, "size": 6, "sha256_ciphertext": sha256_hex(b"las v2")})),
        )
        .await;
    assert_eq!(s, StatusCode::PAYMENT_REQUIRED);
    assert_eq!(v["reason"], "plan_limit");
    let (_, l) = t.req("GET", "/saves", Some(&a.access), None).await;
    let las = l["saves"]
        .as_array()
        .unwrap()
        .iter()
        .find(|x| x["title_id"] == LAS)
        .unwrap()
        .clone();
    assert_eq!(las["read_only"], true);
    assert_eq!(las["delete_after"], T0 + 90 * DAY);

    // Switching back within 30 days: refused.
    let (s, v) = t
        .req(
            "POST",
            "/saves/titles",
            Some(&a.access),
            Some(json!({"title_id": LAS})),
        )
        .await;
    assert_eq!(s, StatusCode::PAYMENT_REQUIRED);
    assert_eq!(v["reason"], "switch_cooldown");
    // Deleting the active title does not reset the cooldown either.
    let (s, _) = t
        .req("DELETE", &format!("/saves/{SMO}"), Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{MK8D}/{p}/versions"),
            Some(&a.access),
            Some(json!({"base_version": 0, "size": 1, "sha256_ciphertext": sha256_hex(b"m")})),
        )
        .await;
    assert_eq!(
        (s, v["reason"].as_str()),
        (StatusCode::PAYMENT_REQUIRED, Some("switch_cooldown"))
    );

    // 31 days later the switch back works and LAS history is intact.
    t.clock.advance(31 * DAY);
    let tok = t.sign_in("free@napple.test", "Mac").await.access; // the 30-day refresh token ran out too
    let (s, v) = t
        .req(
            "POST",
            "/saves/titles",
            Some(&tok),
            Some(json!({"title_id": LAS})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let (_, h) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&tok),
            None,
        )
        .await;
    assert_eq!(h["read_only"], false);
    assert_eq!(h["versions"].as_array().unwrap().len(), 1);
}

#[tokio::test]
async fn account_deletion_removes_everything() {
    let t = T::new();
    let a = t.sign_in("bye@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    for v in 0..3 {
        assert_eq!(t.push(&a, LAS, &p, v, b"some save").await.0, StatusCode::OK);
    }
    assert_eq!(walk(&t.dir.join("blobs")), 3);
    let (s, v) = t
        .req(
            "DELETE",
            "/me",
            Some(&a.access),
            Some(json!({"confirm_email": "someone@else.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "{v}");
    let (s, _) = t
        .req(
            "DELETE",
            "/me",
            Some(&a.access),
            Some(json!({"confirm_email": "BYE@napple.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert_eq!(walk(&t.dir.join("blobs")), 0, "every save object deleted");
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
    {
        let db = t.app.db();
        for table in [
            "accounts",
            "devices",
            "profiles",
            "save_versions",
            "refresh_tokens",
            "titles",
            "blob_deletions",
        ] {
            let n: i64 = db
                .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |r| r.get(0))
                .unwrap();
            assert_eq!(n, 0, "{table} is empty");
        }
    }
    let again = t.sign_in("bye@napple.test", "Mac").await;
    assert_ne!(
        again.account_id, a.account_id,
        "signing up again is a new account"
    );
}

#[tokio::test]
async fn expired_pending_uploads_are_collected() {
    let t = T::new();
    let a = t.sign_in("gc@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    let (_, c) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions"),
            Some(&a.access),
            Some(json!({"base_version": 0, "size": 3, "sha256_ciphertext": sha256_hex(b"abc")})),
        )
        .await;
    assert_eq!(
        t.dev_put(&c, b"abc", SAVE_CONTENT_TYPE).await,
        StatusCode::OK
    );
    assert_eq!(walk(&t.dir.join("blobs")), 1);
    t.clock.advance(DAY + 60);
    let rep = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!(rep.expired_uploads, 1);
    assert_eq!(walk(&t.dir.join("blobs")), 0);
}
