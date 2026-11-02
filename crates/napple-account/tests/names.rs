//! Reserved names through the API (422 reserved_name on display_name and nicknames, re-checked on rename),
//! admin grants (bootstrap admins from NAPPLE_ADMIN_EMAILS, `admin` grant), and the `napple-account admin` CLI.

mod common;
use axum::http::StatusCode;
use common::*;
use serde_json::json;

const ADMIN: &str = "boss@napple.test";

fn t() -> T {
    T::with(&[("NAPPLE_ADMIN_EMAILS", ADMIN)], None)
}

#[tokio::test]
async fn reserved_names_are_422_on_display_name_and_nicknames() {
    let t = t();
    let a = t.sign_in("player@napple.test", "Mac").await;
    for name in ["Napple", "xN4PPLEx", "nаpple", "nap-official", "n4p_x"] {
        let (s, v) = t
            .req(
                "PATCH",
                "/me",
                Some(&a.access),
                Some(json!({"display_name": name})),
            )
            .await;
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY, "{name}: {v}");
        assert_eq!(v["reason"], "reserved_name");
        assert_eq!(v["field"], "display_name");
        let (s, v) = t
            .req(
                "POST",
                "/profiles",
                Some(&a.access),
                Some(json!({"nickname": name})),
            )
            .await;
        assert_eq!(
            (s, v["reason"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some("reserved_name")),
            "{name}"
        );
    }
    let p = t.first_profile(&a).await;
    let (s, _) = t
        .req(
            "PATCH",
            &format!("/profiles/{p}"),
            Some(&a.access),
            Some(json!({"nickname": "NAPP|E"})),
        )
        .await;
    assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&a.access),
            Some(json!({"display_name": "nap\u{200B}ple"})),
        )
        .await;
    assert_eq!(
        s,
        StatusCode::BAD_REQUEST,
        "invisible characters are a plain 400"
    );
    let (s, v) = t
        .req(
            "PATCH",
            "/me",
            Some(&a.access),
            Some(json!({"display_name": "Naples fan"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");

    // An email whose local part is reserved gets the neutral default name.
    let n = t.sign_in("napple.fan@napple.test", "Mac").await;
    let (_, me) = t.req("GET", "/me", Some(&n.access), None).await;
    assert_eq!(me["display_name"], "Player");
    let (_, ps) = t.req("GET", "/profiles", Some(&n.access), None).await;
    assert_eq!(ps["profiles"][0]["nickname"], "Player");
}

#[tokio::test]
async fn admin_grants_unlock_names_and_rename_rechecks() {
    let t = t();
    let boss = t.sign_in(ADMIN, "Mac").await;
    let owner = t.sign_in("owner@napple.test", "Mac").await;
    let dev = t.sign_in("dev@napple.test", "Mac").await;

    // Non-admins cannot grant.
    let (s, _) = t
        .req(
            "POST",
            "/admin/grants",
            Some(&dev.access),
            Some(json!({"email": "dev@napple.test", "grant": "reserved_prefix"})),
        )
        .await;
    assert_eq!(s, StatusCode::FORBIDDEN);
    let (s, _) = t
        .req(
            "GET",
            "/admin/grants?email=dev@napple.test",
            Some(&dev.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::FORBIDDEN);

    // Bootstrap admin grants owner_napple by email and reserved_prefix by account id.
    let (s, v) = t
        .req(
            "POST",
            "/admin/grants",
            Some(&boss.access),
            Some(json!({"email": "OWNER@napple.test", "grant": "owner_napple"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["grants"], json!(["owner_napple"]));
    let (s, _) = t
        .req(
            "POST",
            "/admin/grants",
            Some(&boss.access),
            Some(json!({"account_id": dev.account_id, "grant": "reserved_prefix"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, _) = t
        .req(
            "POST",
            "/admin/grants",
            Some(&boss.access),
            Some(json!({"email": "dev@napple.test", "grant": "superuser"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST, "unknown grant");
    let (s, _) = t
        .req(
            "POST",
            "/admin/grants",
            Some(&boss.access),
            Some(json!({"email": "nobody@napple.test", "grant": "admin"})),
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);

    // The owner may be exactly "napple" (any case), nothing else containing it.
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&owner.access),
            Some(json!({"display_name": "Napple"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, _) = t
        .req(
            "POST",
            "/profiles",
            Some(&owner.access),
            Some(json!({"nickname": "napple"})),
        )
        .await;
    assert_eq!(s, StatusCode::CREATED);
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&owner.access),
            Some(json!({"display_name": "Napple HQ"})),
        )
        .await;
    assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);
    // Others still cannot.
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&dev.access),
            Some(json!({"display_name": "napple"})),
        )
        .await;
    assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);

    // reserved_prefix: nap- names work until the grant is removed; the stored name stays, a rename is checked again.
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&dev.access),
            Some(json!({"display_name": "nap-dev"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (_, me) = t.req("GET", "/me", Some(&dev.access), None).await;
    assert_eq!(me["grants"], json!(["reserved_prefix"]));
    let (s, v) = t
        .req(
            "DELETE",
            "/admin/grants",
            Some(&boss.access),
            Some(json!({"email": "dev@napple.test", "grant": "reserved_prefix"})),
        )
        .await;
    assert_eq!((s, v["grants"].clone()), (StatusCode::OK, json!([])));
    let (_, me) = t.req("GET", "/me", Some(&dev.access), None).await;
    assert_eq!(me["display_name"], "nap-dev");
    let (s, _) = t
        .req(
            "PATCH",
            "/me",
            Some(&dev.access),
            Some(json!({"display_name": "nap-dev2"})),
        )
        .await;
    assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);

    // The admin grant makes another admin.
    t.req(
        "POST",
        "/admin/grants",
        Some(&boss.access),
        Some(json!({"email": "owner@napple.test", "grant": "admin"})),
    )
    .await;
    let (s, v) = t
        .req(
            "GET",
            "/admin/grants?email=dev@napple.test",
            Some(&owner.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
}

#[test]
fn admin_cli_grants_in_the_database() {
    let dir = temp_dir("cli");
    {
        let db = napple_account::db::open(&dir.join("account.sqlite3")).unwrap();
        db.execute("INSERT INTO accounts (id, email, display_name, created_at, last_seen) VALUES ('01900000-0000-7000-8000-000000000001', 'owner@napple.test', 'x', 0, 0)", [])
            .unwrap();
    }
    let run = |args: &[&str]| {
        std::process::Command::new(env!("CARGO_BIN_EXE_napple-account"))
            .args(args)
            .env("NAPPLE_ACCOUNT_DATA", &dir)
            .env("NAPPLE_ACCOUNT_QUIET", "1")
            .output()
            .unwrap()
    };
    let o = run(&["admin", "grant", "Owner@napple.test", "owner_napple"]);
    assert!(o.status.success(), "{}", String::from_utf8_lossy(&o.stderr));
    assert!(String::from_utf8_lossy(&o.stdout).contains("[owner_napple]"));
    let o = run(&["admin", "grant", "owner@napple.test", "reserved_prefix"]);
    assert!(String::from_utf8_lossy(&o.stdout).contains("[owner_napple, reserved_prefix]"));
    let o = run(&["admin", "revoke", "owner@napple.test", "owner_napple"]);
    assert!(String::from_utf8_lossy(&o.stdout).contains("[reserved_prefix]"));
    let o = run(&["admin", "list", "owner@napple.test"]);
    assert!(String::from_utf8_lossy(&o.stdout).contains("[reserved_prefix]"));
    assert!(
        !run(&["admin", "grant", "owner@napple.test", "bogus"])
            .status
            .success()
    );
    assert!(
        !run(&["admin", "grant", "nobody@napple.test", "admin"])
            .status
            .success()
    );
    std::fs::remove_dir_all(dir).ok();
}
