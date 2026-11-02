//! Profiles: unique nicknames, bio, links, icons, tier, friend code, limits, internal lookups.

mod common;
use axum::http::StatusCode;
use common::*;
use serde_json::{Value, json};

async fn post(t: &T, s: &Session, body: Value) -> (StatusCode, Value) {
    t.req("POST", "/profiles", Some(&s.access), Some(body))
        .await
}

#[tokio::test]
async fn nicknames_are_globally_unique_ignoring_case() {
    let t = T::new();
    let a = t.sign_in("alice@napple.test", "Mac").await;
    let b = t.sign_in("bob@napple.test", "Mac").await;
    let (s, v) = post(&t, &a, json!({"nickname": "Mario_Fan"})).await;
    assert_eq!(s, StatusCode::CREATED, "{v}");
    for n in ["mario_fan", "MARIO_FAN", "mArIo_fAn"] {
        let (s, v) = post(&t, &b, json!({"nickname": n})).await;
        assert_eq!(s, StatusCode::CONFLICT, "{n}: {v}");
        assert_eq!(v["error"], "nickname_taken");
    }
    // Own other profile too, and via PATCH.
    let (s, _) = post(&t, &a, json!({"nickname": "MARIO_fan"})).await;
    assert_eq!(s, StatusCode::CONFLICT);
    let pb = t.first_profile(&b).await;
    let (s, v) = t
        .req(
            "PATCH",
            &format!("/profiles/{pb}"),
            Some(&b.access),
            Some(json!({"nickname": "mario_fan"})),
        )
        .await;
    assert_eq!(s, StatusCode::CONFLICT, "{v}");
    // Changing only the case of your own name is fine.
    let (s, v) = t
        .req(
            "PATCH",
            &format!("/profiles/{pb}"),
            Some(&b.access),
            Some(json!({"nickname": "BOBBY"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let (s, v) = t
        .req(
            "PATCH",
            &format!("/profiles/{pb}"),
            Some(&b.access),
            Some(json!({"nickname": "bobby"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["nickname"], "bobby");
    // A freed name can be taken again.
    let (_, ps) = t.req("GET", "/profiles", Some(&a.access), None).await;
    let pid = ps["profiles"][1]["profile_id"]
        .as_str()
        .unwrap()
        .to_string();
    let (s, _) = t
        .req("DELETE", &format!("/profiles/{pid}"), Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let (s, _) = post(&t, &b, json!({"nickname": "mario_fan"})).await;
    assert_eq!(s, StatusCode::CREATED);
}

#[tokio::test]
async fn nickname_shape_and_reserved_names() {
    let t = T::new();
    let a = t.sign_in("shape@napple.test", "Mac").await;
    for bad in [
        "ab",
        "has space",
        "caf\u{e9}_x",
        "a".repeat(21).as_str(),
        "emoji\u{1F600}x",
        "sl/ash",
        "",
    ] {
        let (s, v) = post(&t, &a, json!({"nickname": bad})).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{bad:?}: {v}");
    }
    for bad in ["Napple_x", "n4pple", "nap-team", "xxNaPPLExx"] {
        let (s, v) = post(&t, &a, json!({"nickname": bad})).await;
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY, "{bad}: {v}");
        assert_eq!(v["reason"], "reserved_name");
    }
    for ok in ["abc", "A-b_c.d", "a".repeat(20).as_str()] {
        let (s, v) = post(&t, &a, json!({"nickname": ok})).await;
        assert_eq!(s, StatusCode::CREATED, "{ok}: {v}");
    }
}

#[tokio::test]
async fn fields_defaults_and_validation() {
    let t = T::new();
    let a = t.sign_in("fields@napple.test", "Mac").await;
    let (s, p) = post(&t, &a, json!({"nickname": "Fields1"})).await;
    assert_eq!(s, StatusCode::CREATED, "{p}");
    assert_eq!(p["tier"], 1);
    assert_eq!(p["bio"], "");
    assert_eq!(p["links"], json!([]));
    assert_eq!(p["photo"], Value::Null);
    let code = p["friend_code"].as_str().unwrap();
    assert!(napple_account::profiles::valid_friend_code(code), "{code}");
    let id = p["profile_id"].as_str().unwrap().to_string();
    let url = format!("/profiles/{id}");
    let patch = |b: Value| {
        let t = &t;
        let a = &a;
        let url = url.clone();
        async move { t.req("PATCH", &url, Some(&a.access), Some(b)).await }
    };

    let (s, v) = patch(json!({"bio": "Racing on Sundays", "icon_id": 47, "tier": 3,
        "links": [{"kind": "web", "value": "https://napple.lat/me"}, {"kind": "x", "value": "@neto"}]})).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["tier"], 3);
    assert_eq!(v["icon_id"], 47);
    assert_eq!(v["links"][1], json!({"kind": "x", "value": "neto"}));
    assert_eq!(v["friend_code"], code, "friend code is stable");

    for bad in [
        json!({"bio": "x".repeat(161)}),
        json!({"bio": "zero\u{200B}width"}),
        json!({"icon_id": 48}),
        json!({"icon_id": -1}),
        json!({"tier": 0}),
        json!({"tier": 6}),
        json!({"links": [{"kind": "web", "value": "http://example.com"}]}),
        json!({"links": [{"kind": "web", "value": "javascript:alert(1)"}]}),
        json!({"links": [{"kind": "tiktok", "value": "abc"}]}),
        json!({"links": [{"kind": "x", "value": "aaa"}, {"kind": "x", "value": "bbb"}, {"kind": "x", "value": "ccc"}, {"kind": "x", "value": "ddd"}]}),
    ] {
        let (s, v) = patch(bad.clone()).await;
        assert_eq!(s, StatusCode::BAD_REQUEST, "{bad}: {v}");
    }
    // Nothing from the rejected requests leaked in.
    let (_, v) = t.req("GET", &url, Some(&a.access), None).await;
    assert_eq!(v["tier"], 3);
    assert_eq!(v["bio"], "Racing on Sundays");
    // 160 chars exactly is fine.
    let (s, _) = patch(json!({"bio": "é".repeat(160)})).await;
    assert_eq!(s, StatusCode::OK);
}

#[tokio::test]
async fn up_to_eight_profiles_and_friend_codes_are_unique() {
    let t = T::new();
    let a = t.sign_in("eight@napple.test", "Mac").await;
    let b = t.sign_in("eight2@napple.test", "Mac").await;
    let mut codes = std::collections::HashSet::new();
    for (n, s) in [&a, &b].into_iter().enumerate() {
        for i in 0..7 {
            let (st, p) = post(&t, s, json!({"nickname": format!("acct{n}_{i}")})).await;
            assert_eq!(st, StatusCode::CREATED, "{p}");
        }
        let (_, ps) = t.req("GET", "/profiles", Some(&s.access), None).await;
        assert_eq!(ps["profiles"].as_array().unwrap().len(), 8);
        for p in ps["profiles"].as_array().unwrap() {
            assert!(codes.insert(p["friend_code"].as_str().unwrap().to_string()));
        }
        let (st, v) = post(&t, s, json!({"nickname": "toomany"})).await;
        assert_eq!(st, StatusCode::CONFLICT);
        assert_eq!(v["error"], "profile_limit");
    }
    assert_eq!(codes.len(), 16);
}

#[tokio::test]
async fn profiles_are_private_to_their_account() {
    let t = T::new();
    let a = t.sign_in("own@napple.test", "Mac").await;
    let b = t.sign_in("other@napple.test", "Mac").await;
    let pid = t.first_profile(&a).await;
    for (m, body) in [
        ("GET", None),
        ("PATCH", Some(json!({"tier": 5}))),
        ("DELETE", None),
    ] {
        let (s, _) = t
            .req(m, &format!("/profiles/{pid}"), Some(&b.access), body)
            .await;
        assert_eq!(s, StatusCode::NOT_FOUND, "{m}");
    }
    let (s, _) = t.req("GET", &format!("/profiles/{pid}"), None, None).await;
    assert_eq!(s, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn existing_profiles_get_defaults_and_codes_on_migration() {
    // A database from before this feature: the old 5-column table with rows in it.
    let c = rusqlite::Connection::open_in_memory().unwrap();
    c.execute_batch(
        "CREATE TABLE accounts (id TEXT PRIMARY KEY);
         CREATE TABLE profiles (id TEXT PRIMARY KEY, account_id TEXT NOT NULL, nickname TEXT NOT NULL,
            icon_id INTEGER NOT NULL, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
         INSERT INTO profiles VALUES ('p1','a','Old',3,1,1),('p2','a','Older',4,1,1);",
    )
    .unwrap();
    napple_account::profiles::migrate(&c).unwrap();
    napple_account::profiles::migrate(&c).unwrap(); // idempotent
    let rows: Vec<(String, i64, String)> = c
        .prepare("SELECT friend_code, tier, bio FROM profiles ORDER BY id")
        .unwrap()
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(rows.len(), 2);
    for (code, tier, bio) in &rows {
        assert!(napple_account::profiles::valid_friend_code(code));
        assert_eq!((*tier, bio.as_str()), (1, ""));
    }
    assert_ne!(rows[0].0, rows[1].0);
    // Rows inserted later (any code path) get a code from the trigger.
    c.execute("INSERT INTO profiles (id, account_id, nickname, icon_id, created_at, updated_at) VALUES ('p3','a','New',0,1,1)", []).unwrap();
    let code: String = c
        .query_row("SELECT friend_code FROM profiles WHERE id='p3'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert!(napple_account::profiles::valid_friend_code(&code));
}

#[tokio::test]
async fn internal_lookups_need_the_shared_token() {
    // NAPPLE_INTERNAL_TOKEN is process-wide: this is the only test that sets it.
    unsafe { std::env::set_var("NAPPLE_INTERNAL_TOKEN", "test-internal-token-0123456789") };
    let t = T::new();
    let a = t.sign_in("intern@napple.test", "Mac").await;
    let (_, p) = post(&t, &a, json!({"nickname": "Looked_Up", "tier": 4})).await;
    let id = p["profile_id"].as_str().unwrap();
    let code = p["friend_code"].as_str().unwrap();

    let get = |path: String, tok: Option<&'static str>| {
        let t = &t;
        async move {
            let mut b = axum::http::Request::builder().uri(format!("/account/v1{path}"));
            if let Some(tok) = tok {
                b = b.header("x-napple-internal", tok);
            }
            let (s, _, body) = t.raw(b.body(axum::body::Body::empty()).unwrap()).await;
            (
                s,
                serde_json::from_slice::<Value>(&body).unwrap_or(Value::Null),
            )
        }
    };
    let tok = Some("test-internal-token-0123456789");
    let (s, _) = get(format!("/internal/profiles/{id}"), None).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = get(
        format!("/internal/profiles/{id}"),
        Some("wrong-token-wrong-token"),
    )
    .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, v) = get(format!("/internal/profiles/{id}"), tok).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["tier"], 4);
    assert_eq!(v["account_id"], a.account_id);
    let (s, v) = get(
        format!("/internal/profiles/by-code/{}", code.to_lowercase()),
        tok,
    )
    .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["profile_id"], id);
    let (s, _) = get("/internal/profiles/by-code/NP-123456789012".into(), tok).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, v) = get("/internal/profiles?prefix=looked_".into(), tok).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(v["profiles"].as_array().unwrap().len(), 1);
    // '%' and other wildcards are not a prefix.
    let (_, v) = get("/internal/profiles?prefix=%25".into(), tok).await;
    assert_eq!(v["profiles"].as_array().unwrap().len(), 0);
}
