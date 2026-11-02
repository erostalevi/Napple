//! Plans and Stripe: 503 without config, Checkout/Portal through a mock Stripe API, signed + idempotent webhooks,
//! out-of-order events, downgrade to read-only, warning email and deletion after the read-only window.

mod common;
use axum::body::Body;
use axum::http::{Request, StatusCode};
use common::*;
use serde_json::json;

#[tokio::test]
async fn billing_is_503_without_stripe_config() {
    let t = T::new();
    let a = t.sign_in("nobill@napple.test", "Mac").await;
    for (m, p) in [
        ("POST", "/billing/checkout"),
        ("POST", "/billing/portal"),
        ("POST", "/billing/webhook"),
    ] {
        let (s, v) = t.req(m, p, Some(&a.access), None).await;
        assert_eq!(s, StatusCode::SERVICE_UNAVAILABLE, "{p}: {v}");
    }
    let (s, _, _) = t
        .raw(
            Request::builder()
                .uri("/account/v1/billing")
                .body(Body::empty())
                .unwrap(),
        )
        .await;
    assert_eq!(s, StatusCode::SERVICE_UNAVAILABLE);
    assert!(t.out.requests().is_empty(), "nothing reaches Stripe");
}

#[tokio::test]
async fn checkout_and_portal_through_mock_stripe() {
    let t = T::with_stripe();
    let a = t.sign_in("buy@napple.test", "Mac").await;
    let (s, v) = t
        .req(
            "POST",
            "/billing/checkout",
            Some(&a.access),
            Some(json!({"interval": "yearly"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["url"], "https://checkout.stripe.com/c/pay/cs_test_1");
    let r = &t.out.requests()[0];
    let form = String::from_utf8(r.body.clone()).unwrap();
    assert!(form.contains("mode=subscription"));
    assert!(form.contains("price_test_yearly"));
    assert!(form.contains(&format!("client_reference_id={}", a.account_id)));
    assert!(form.contains("customer_email=buy%40napple.test"));
    assert!(
        r.headers
            .iter()
            .any(|(k, v)| k == "authorization" && v == "Bearer sk_test_fixture_not_a_real_key")
    );
    assert!(r.headers.iter().any(|(k, _)| k == "idempotency-key"));
    let (s, _) = t
        .req(
            "POST",
            "/billing/checkout",
            Some(&a.access),
            Some(json!({"interval": "weekly"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);

    // Portal needs a Stripe customer, which the checkout webhook links.
    let (s, _) = t
        .req("POST", "/billing/portal", Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, v) = t
        .webhook(&fixture(
            "checkout_session_completed.json",
            &a.account_id,
            T0,
        ))
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let (s, v) = t
        .req("POST", "/billing/portal", Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert!(
        String::from_utf8(t.out.requests().last().unwrap().body.clone())
            .unwrap()
            .contains("customer=cus_test_napple_0001")
    );
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(
        me["plan"]["name"], "plus",
        "a paid checkout grants Plus right away"
    );
    let (s, v) = t
        .req("POST", "/billing/checkout", Some(&a.access), None)
        .await;
    assert_eq!(
        (s, v["error"].as_str()),
        (StatusCode::CONFLICT, Some("already_subscribed"))
    );
}

#[tokio::test]
async fn webhook_signature_and_idempotency() {
    let t = T::with_stripe();
    let a = t.sign_in("hook@napple.test", "Mac").await;
    let ev = fixture("subscription_created.json", &a.account_id, T0);

    // Unsigned, wrongly signed, stale: all refused and nothing changes.
    let body = ev.to_string();
    for sig in [
        None,
        Some(napple_account::billing::sign_payload(
            "whsec_wrong",
            body.as_bytes(),
            T0,
        )),
        Some(napple_account::billing::sign_payload(
            WEBHOOK_SECRET,
            body.as_bytes(),
            T0 - 600,
        )),
    ] {
        let mut b = Request::builder()
            .method("POST")
            .uri("/account/v1/billing/webhook");
        if let Some(s) = sig {
            b = b.header("stripe-signature", s);
        }
        let (s, _, _) = t.raw(b.body(Body::from(body.clone())).unwrap()).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
    }
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["plan"]["name"], "free");

    let (s, v) = t.webhook(&ev).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(v["outcome"], "plus");
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["plan"]["name"], "plus");
    assert_eq!(me["plan"]["synced_titles_limit"], serde_json::Value::Null);
    assert_eq!(me["plan"]["versions_per_title"], 20);
    assert_eq!(me["plan"]["quota"], 5u64 << 30);
    assert_eq!(me["plan"]["interval"], "monthly");
    assert_eq!(me["plan"]["renews_at"], T0 + 30 * DAY);

    // Redelivery: acknowledged, not applied twice.
    let (s, v) = t.webhook(&ev).await;
    assert_eq!((s, v["duplicate"].as_bool()), (StatusCode::OK, Some(true)));

    // An older event that arrives late does not undo a newer state.
    let (_, v) = t
        .webhook(&fixture(
            "subscription_deleted.json",
            &a.account_id,
            T0 + 100,
        ))
        .await;
    assert_eq!(v["outcome"], "free");
    let mut late = fixture("subscription_updated_past_due.json", &a.account_id, T0 + 50);
    late["id"] = json!("evt_test_late_0099");
    let (_, v) = t.webhook(&late).await;
    assert_eq!(v["outcome"], "ignored: out of order");
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["plan"]["name"], "free");

    // Unknown event types are acknowledged and ignored; events for unknown accounts too.
    let (s, v) = t
        .webhook(&fixture("invoice_payment_failed.json", &a.account_id, T0))
        .await;
    assert_eq!(
        (s, v["outcome"].as_str()),
        (StatusCode::OK, Some("ignored"))
    );
    let mut stranger = fixture(
        "subscription_created.json",
        "01900000-0000-7000-8000-000000000000",
        T0,
    );
    stranger["id"] = json!("evt_test_stranger");
    stranger["data"]["object"]["customer"] = json!("cus_test_unknown");
    let (s, v) = t.webhook(&stranger).await;
    assert_eq!(
        (s, v["outcome"].as_str()),
        (StatusCode::OK, Some("unknown account"))
    );
}

#[tokio::test]
async fn past_due_keeps_plus_then_downgrade_is_read_only_then_deleted_after_warning() {
    let t = T::with_stripe();
    let a = t.sign_in("down@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    t.webhook(&fixture("subscription_created.json", &a.account_id, T0))
        .await;
    for title in [LAS, SMO, MK8D] {
        assert_eq!(
            t.push(&a, title, &p, 0, title.as_bytes()).await.0,
            StatusCode::OK,
            "Plus syncs any number of titles"
        );
        t.clock.advance(60);
    }
    let (_, v) = t
        .webhook(&fixture(
            "subscription_updated_past_due.json",
            &a.account_id,
            T0 + 200,
        ))
        .await;
    assert_eq!(
        v["outcome"], "plus",
        "past_due keeps Plus while Stripe retries"
    );

    t.clock.advance(3600);
    let now = T0 + 180 + 3600;
    let (_, v) = t
        .webhook(&fixture("subscription_deleted.json", &a.account_id, now))
        .await;
    assert_eq!(v["outcome"], "free");
    let (_, r) = t
        .req(
            "POST",
            "/auth/refresh",
            None,
            Some(json!({"refresh_token": a.refresh})),
        )
        .await;
    let tok = r["access_token"].as_str().unwrap().to_string();
    let (_, l) = t.req("GET", "/saves/titles", Some(&tok), None).await;
    let st = |id: &str| {
        l["titles"]
            .as_array()
            .unwrap()
            .iter()
            .find(|x| x["title_id"] == id)
            .unwrap()["state"]
            .as_str()
            .unwrap()
            .to_string()
    };
    assert_eq!(
        st(MK8D),
        "active",
        "the most recently saved title stays synced"
    );
    assert_eq!(st(LAS), "readonly");
    assert_eq!(st(SMO), "readonly");
    // Read-only saves download; uploads to them are 402.
    let (s, _) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions/1"),
            Some(&tok),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, v) = t.req("POST", &format!("/saves/{LAS}/{p}/versions"), Some(&tok), Some(json!({"base_version": 1, "size": 1, "sha256_ciphertext": napple_account::util::sha256_hex(b"x")}))).await;
    assert_eq!(
        (s, v["reason"].as_str()),
        (StatusCode::PAYMENT_REQUIRED, Some("plan_limit"))
    );

    // Day 82: nothing yet. Day 83: one warning per read-only title. Day 90: still kept (warning was < 7 days ago? no: 7 days).
    let advance = |days: i64| t.clock.advance(days * DAY);
    advance(82);
    let rep = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!((rep.warned, rep.deleted_titles), (0, 0));
    advance(1);
    let rep = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!((rep.warned, rep.deleted_titles), (2, 0));
    let warn = t.mail.last_to("down@napple.test").unwrap();
    assert!(warn.subject.contains("will be deleted"), "{}", warn.subject);
    let rep = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!(rep.warned, 0, "warned once");
    advance(7);
    let rep = napple_account::retention::run_once(&t.app).await.unwrap();
    assert_eq!(rep.deleted_titles, 2);
    assert!(rep.blobs_deleted >= 2);

    // 90 days later the refresh token (30 days) is long gone: sign in again.
    let again = t.sign_in("down@napple.test", "Mac").await;
    let (_, l) = t.req("GET", "/saves", Some(&again.access), None).await;
    let titles: Vec<&str> = l["saves"]
        .as_array()
        .unwrap()
        .iter()
        .map(|x| x["title_id"].as_str().unwrap())
        .collect();
    assert_eq!(titles, vec![MK8D]);
}

#[tokio::test]
async fn upgrade_restores_read_only_titles_and_deletion_cancels_stripe() {
    let t = T::with_stripe();
    let a = t.sign_in("up@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    assert_eq!(t.push(&a, LAS, &p, 0, b"las").await.0, StatusCode::OK);
    t.req(
        "POST",
        "/saves/titles",
        Some(&a.access),
        Some(json!({"title_id": SMO})),
    )
    .await;
    t.webhook(&fixture(
        "checkout_session_completed.json",
        &a.account_id,
        T0,
    ))
    .await;
    t.webhook(&fixture("subscription_created.json", &a.account_id, T0 + 1))
        .await;
    let (_, l) = t.req("GET", "/saves/titles", Some(&a.access), None).await;
    assert!(
        l["titles"]
            .as_array()
            .unwrap()
            .iter()
            .all(|x| x["state"] == "active"),
        "{l}"
    );
    assert_eq!(t.push(&a, LAS, &p, 1, b"las 2").await.0, StatusCode::OK);

    let (s, _) = t
        .req(
            "DELETE",
            "/me",
            Some(&a.access),
            Some(json!({"confirm_email": "up@napple.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    let last = t.out.requests().last().unwrap().clone();
    assert_eq!(
        (last.method.as_str(), last.url.as_str()),
        (
            "DELETE",
            "https://stripe.mock/v1/customers/cus_test_napple_0001"
        )
    );
}
