//! Napple Plus billing through Stripe: Checkout and the Customer Portal (the apps open the returned URL in the
//! browser; nothing is paid in-app), and the webhook that mirrors the subscription into the account's plan.
//! Webhooks are verified (`Stripe-Signature`, HMAC-SHA256, 5 minute tolerance, constant time) and idempotent
//! (event ids recorded in the same transaction as their effect). Without Stripe config every route here is 503.

use crate::app::{App, AppState, Authed};
use crate::config::StripeConfig;
use crate::error::{ApiError, ApiResult};
use crate::outbound::OutRequest;
use crate::plans::{apply_plan_change, status_grants_plus};
use crate::util::{ct_eq, hmac_sha256};
use axum::Json;
use axum::body::Bytes;
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, utf8_percent_encode};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};

pub const SIGNATURE_TOLERANCE: i64 = 300;

/// Form/path encoding: everything but RFC 3986 unreserved characters.
const ENC: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'_')
    .remove(b'.')
    .remove(b'~');

fn stripe(app: &App) -> ApiResult<&StripeConfig> {
    app.cfg.stripe.as_ref().ok_or_else(|| {
        ApiError::unavailable(
            "billing_unavailable",
            "billing is not configured on this server; every account is on Free",
        )
    })
}

fn form(pairs: &[(&str, String)]) -> Vec<u8> {
    pairs
        .iter()
        .map(|(k, v)| {
            format!(
                "{}={}",
                utf8_percent_encode(k, ENC),
                utf8_percent_encode(v, ENC)
            )
        })
        .collect::<Vec<_>>()
        .join("&")
        .into_bytes()
}

async fn stripe_call(
    app: &App,
    method: &str,
    path: &str,
    body: Vec<u8>,
    idempotency_key: Option<String>,
) -> ApiResult<Value> {
    let s = stripe(app)?;
    let mut headers = vec![
        (
            "authorization".to_string(),
            format!("Bearer {}", s.secret_key),
        ),
        (
            "content-type".to_string(),
            "application/x-www-form-urlencoded".to_string(),
        ),
    ];
    if let Some(k) = idempotency_key {
        headers.push(("idempotency-key".into(), k));
    }
    let r = app
        .out
        .send(OutRequest {
            method: method.into(),
            url: format!("{}{path}", s.api_base),
            headers,
            body,
        })
        .await
        .map_err(|e| {
            crate::log::error(&format!("stripe {path}: {e}"));
            ApiError::new(
                StatusCode::BAD_GATEWAY,
                "billing_unreachable",
                "could not reach the payment provider, try again",
            )
        })?;
    let v = r.json().unwrap_or(Value::Null);
    if r.status / 100 != 2 {
        crate::log::error(&format!(
            "stripe {path}: HTTP {} {}",
            r.status,
            v["error"]["type"].as_str().unwrap_or("")
        ));
        return Err(ApiError::new(
            StatusCode::BAD_GATEWAY,
            "billing_error",
            "the payment provider refused the request",
        ));
    }
    Ok(v)
}

#[derive(Deserialize)]
pub struct CheckoutIn {
    /// `monthly` (default) or `yearly`.
    #[serde(default)]
    interval: Option<String>,
}

pub async fn checkout(
    State(app): State<AppState>,
    a: Authed,
    body: Bytes,
) -> ApiResult<Json<Value>> {
    let s = stripe(&app)?.clone();
    let b: CheckoutIn = if body.is_empty() {
        CheckoutIn { interval: None }
    } else {
        serde_json::from_slice(&body).map_err(|_| ApiError::bad_request("body: {interval}"))?
    };
    let price = match b.interval.as_deref().unwrap_or("monthly") {
        "monthly" => s.price_monthly.clone(),
        "yearly" => s.price_yearly.clone(),
        _ => return Err(ApiError::bad_request("interval: monthly or yearly")),
    };
    let (email, plan, customer): (String, String, Option<String>) = app.db().query_row(
        "SELECT email, plan, stripe_customer_id FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
    )?;
    if plan == "plus" {
        return Err(ApiError::conflict(
            "already_subscribed",
            "this account already has Napple Plus; manage it in the billing portal",
        ));
    }
    let base = &app.cfg.public_base;
    let mut f = vec![
        ("mode", "subscription".to_string()),
        ("line_items[0][price]", price),
        ("line_items[0][quantity]", "1".into()),
        ("success_url", format!("{base}/billing/success")),
        ("cancel_url", format!("{base}/billing/cancel")),
        ("client_reference_id", a.account_id.clone()),
        ("metadata[account_id]", a.account_id.clone()),
        (
            "subscription_data[metadata][account_id]",
            a.account_id.clone(),
        ),
        ("allow_promotion_codes", "true".into()),
    ];
    match customer {
        Some(c) => f.push(("customer", c)),
        None => f.push(("customer_email", email)),
    }
    // One checkout per account per minute: double taps reuse the same session.
    let idem = format!("checkout-{}-{}", a.account_id, app.now() / 60);
    let v = stripe_call(&app, "POST", "/v1/checkout/sessions", form(&f), Some(idem)).await?;
    let url = v["url"]
        .as_str()
        .ok_or_else(|| ApiError::internal("stripe checkout session without url"))?;
    Ok(Json(json!({"url": url, "id": v["id"]})))
}

pub async fn portal(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    stripe(&app)?;
    let customer: Option<String> = app.db().query_row(
        "SELECT stripe_customer_id FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| r.get(0),
    )?;
    let customer = customer.ok_or_else(|| {
        ApiError::not_found("no billing account yet; subscribe first")
            .with("error_code", "no_customer")
    })?;
    let v = stripe_call(
        &app,
        "POST",
        "/v1/billing_portal/sessions",
        form(&[
            ("customer", customer),
            (
                "return_url",
                format!("{}/billing/success", app.cfg.public_base),
            ),
        ]),
        None,
    )
    .await?;
    let url = v["url"]
        .as_str()
        .ok_or_else(|| ApiError::internal("stripe portal session without url"))?;
    Ok(Json(json!({"url": url})))
}

/// Account deletion: delete the Stripe customer (cancels its subscriptions immediately), or cancel a lone subscription.
/// Without Stripe config there is nothing to cancel against, so deletion proceeds.
pub async fn cancel_everything(
    app: &App,
    customer: Option<&str>,
    sub: Option<&str>,
) -> ApiResult<()> {
    if app.cfg.stripe.is_none() {
        crate::log::warn(
            "account deletion without Stripe config: subscription (if any) not cancelled",
        );
        return Ok(());
    }
    let enc = |s: &str| utf8_percent_encode(s, ENC).to_string();
    match (customer, sub) {
        (Some(c), _) => stripe_call(
            app,
            "DELETE",
            &format!("/v1/customers/{}", enc(c)),
            vec![],
            None,
        )
        .await
        .map(|_| ()),
        (None, Some(s)) => stripe_call(
            app,
            "DELETE",
            &format!("/v1/subscriptions/{}", enc(s)),
            vec![],
            None,
        )
        .await
        .map(|_| ()),
        (None, None) => Ok(()),
    }
}

/// `t=<unix>,v1=<hex>[,v1=...]` over `"<t>.<body>"`.
pub fn verify_signature(
    secret: &str,
    header: &str,
    body: &[u8],
    now: i64,
) -> Result<(), &'static str> {
    let mut t: Option<i64> = None;
    let mut sigs = Vec::new();
    for part in header.split(',') {
        match part.trim().split_once('=') {
            Some(("t", v)) => t = v.parse().ok(),
            Some(("v1", v)) => sigs.push(v.to_string()),
            _ => {}
        }
    }
    let t = t.ok_or("missing timestamp")?;
    if (now - t).abs() > SIGNATURE_TOLERANCE {
        return Err("timestamp outside tolerance");
    }
    let mut signed = format!("{t}.").into_bytes();
    signed.extend_from_slice(body);
    let expect = hex::encode(hmac_sha256(secret.as_bytes(), &signed));
    if sigs.iter().any(|s| ct_eq(s.as_bytes(), expect.as_bytes())) {
        Ok(())
    } else {
        Err("no matching signature")
    }
}

/// Test helper (and documentation of the scheme): the header Stripe would send for `body` at `t`.
pub fn sign_payload(secret: &str, body: &[u8], t: i64) -> String {
    let mut signed = format!("{t}.").into_bytes();
    signed.extend_from_slice(body);
    format!(
        "t={t},v1={}",
        hex::encode(hmac_sha256(secret.as_bytes(), &signed))
    )
}

pub async fn webhook(
    State(app): State<AppState>,
    headers: HeaderMap,
    body: Bytes,
) -> ApiResult<Json<Value>> {
    let s = stripe(&app)?;
    let sig = headers
        .get("stripe-signature")
        .and_then(|v| v.to_str().ok())
        .ok_or_else(|| ApiError::bad_request("missing Stripe-Signature"))?;
    verify_signature(&s.webhook_secret, sig, &body, app.now()).map_err(|e| {
        ApiError::bad_request(format!("bad signature: {e}")).with("error_code", "bad_signature")
    })?;
    let ev: Value =
        serde_json::from_slice(&body).map_err(|_| ApiError::bad_request("event is not JSON"))?;
    let id = ev["id"]
        .as_str()
        .ok_or_else(|| ApiError::bad_request("event without id"))?
        .to_string();
    let ty = ev["type"].as_str().unwrap_or("").to_string();
    let now = app.now();
    let mut db = app.db();
    let tx = db.transaction()?;
    let seen = tx
        .query_row("SELECT 1 FROM stripe_events WHERE id = ?1", [&id], |_| {
            Ok(())
        })
        .optional()?
        .is_some();
    if seen {
        return Ok(Json(json!({"received": true, "duplicate": true})));
    }
    let outcome = handle_event(&app, &tx, &ev, now)?;
    tx.execute(
        "INSERT INTO stripe_events (id, type, received_at) VALUES (?1, ?2, ?3)",
        params![id, ty, now],
    )?;
    tx.commit()?;
    crate::log::info(&format!("stripe event {id} {ty}: {outcome}"));
    Ok(Json(json!({"received": true, "outcome": outcome})))
}

fn account_for(db: &Connection, obj: &Value) -> rusqlite::Result<Option<String>> {
    for cand in [
        obj["metadata"]["account_id"].as_str(),
        obj["client_reference_id"].as_str(),
    ]
    .into_iter()
    .flatten()
    {
        if let Some(id) = db
            .query_row("SELECT id FROM accounts WHERE id = ?1", [cand], |r| {
                r.get(0)
            })
            .optional()?
        {
            return Ok(Some(id));
        }
    }
    match obj["customer"].as_str() {
        Some(c) => db
            .query_row(
                "SELECT id FROM accounts WHERE stripe_customer_id = ?1",
                [c],
                |r| r.get(0),
            )
            .optional(),
        None => Ok(None),
    }
}

fn set_plan(
    app: &App,
    db: &Connection,
    account_id: &str,
    plus: bool,
    now: i64,
) -> rusqlite::Result<()> {
    let was: String = db.query_row(
        "SELECT plan FROM accounts WHERE id = ?1",
        [account_id],
        |r| r.get(0),
    )?;
    let new = if plus { "plus" } else { "free" };
    if was != new {
        db.execute(
            "UPDATE accounts SET plan = ?1 WHERE id = ?2",
            params![new, account_id],
        )?;
        apply_plan_change(app, db, account_id, plus, now)?;
        crate::log::info(&format!("plan {account_id}: {was} -> {new}"));
    }
    Ok(())
}

fn handle_event(app: &App, db: &Connection, ev: &Value, now: i64) -> ApiResult<&'static str> {
    let obj = &ev["data"]["object"];
    let created = ev["created"].as_i64().unwrap_or(now);
    let ty = ev["type"].as_str().unwrap_or("");
    match ty {
        "checkout.session.completed" => {
            let Some(acc) = account_for(db, obj)? else {
                return Ok("unknown account");
            };
            if let Some(c) = obj["customer"].as_str() {
                db.execute(
                    "UPDATE accounts SET stripe_customer_id = ?1 WHERE id = ?2",
                    params![c, acc],
                )?;
            }
            if let Some(sub) = obj["subscription"].as_str() {
                db.execute(
                    "UPDATE accounts SET sub_id = COALESCE(sub_id, ?1) WHERE id = ?2",
                    params![sub, acc],
                )?;
            }
            // The subscription events carry the state; a paid completed checkout is Plus right away.
            if obj["payment_status"].as_str() == Some("paid")
                && obj["mode"].as_str() == Some("subscription")
            {
                let status: Option<String> = db.query_row(
                    "SELECT sub_status FROM accounts WHERE id = ?1",
                    [&acc],
                    |r| r.get(0),
                )?;
                if status.is_none() {
                    db.execute(
                        "UPDATE accounts SET sub_status = 'active' WHERE id = ?1",
                        [&acc],
                    )?;
                    set_plan(app, db, &acc, true, now)?;
                }
            }
            Ok("customer linked")
        }
        "customer.subscription.created"
        | "customer.subscription.updated"
        | "customer.subscription.deleted" => {
            let Some(acc) = account_for(db, obj)? else {
                return Ok("unknown account");
            };
            let sub_id = obj["id"].as_str().unwrap_or("");
            let status = if ty == "customer.subscription.deleted" {
                "canceled"
            } else {
                obj["status"].as_str().unwrap_or("incomplete")
            };
            let (cur_sub, cur_status, last_at): (Option<String>, Option<String>, Option<i64>) = db
                .query_row(
                    "SELECT sub_id, sub_status, sub_event_at FROM accounts WHERE id = ?1",
                    [&acc],
                    |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
                )?;
            // A different subscription that does not grant Plus must not override a live one.
            if cur_sub.as_deref().is_some_and(|c| c != sub_id)
                && cur_status.as_deref().is_some_and(status_grants_plus)
                && !status_grants_plus(status)
            {
                return Ok("ignored: other subscription");
            }
            // Stripe does not guarantee ordering: ignore anything older than what we applied for this subscription.
            if cur_sub.as_deref() == Some(sub_id) && last_at.is_some_and(|t| created < t) {
                return Ok("ignored: out of order");
            }
            let item = &obj["items"]["data"][0];
            let renews = obj["current_period_end"]
                .as_i64()
                .or_else(|| item["current_period_end"].as_i64());
            let interval = item["price"]["recurring"]["interval"]
                .as_str()
                .or_else(|| obj["plan"]["interval"].as_str())
                .map(|i| if i == "year" { "yearly" } else { "monthly" });
            db.execute(
                "UPDATE accounts SET sub_id = ?1, sub_status = ?2, renews_at = ?3, sub_interval = ?4, cancel_at_period_end = ?5, sub_event_at = ?6,
                 stripe_customer_id = COALESCE(stripe_customer_id, ?7) WHERE id = ?8",
                params![sub_id, status, renews, interval, obj["cancel_at_period_end"].as_bool().unwrap_or(false), created, obj["customer"].as_str(), acc],
            )?;
            set_plan(app, db, &acc, status_grants_plus(status), now)?;
            Ok(if status_grants_plus(status) {
                "plus"
            } else {
                "free"
            })
        }
        "customer.deleted" => {
            let Some(acc) = account_for(db, obj)? else {
                return Ok("unknown account");
            };
            db.execute("UPDATE accounts SET stripe_customer_id = NULL, sub_status = 'canceled', sub_event_at = ?1 WHERE id = ?2", params![created, acc])?;
            set_plan(app, db, &acc, false, now)?;
            Ok("free")
        }
        _ => Ok("ignored"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signature_scheme() {
        let body = br#"{"id":"evt_1"}"#;
        let h = sign_payload("whsec_test", body, 1000);
        assert!(verify_signature("whsec_test", &h, body, 1100).is_ok());
        assert_eq!(
            verify_signature("whsec_test", &h, body, 1400),
            Err("timestamp outside tolerance")
        );
        assert_eq!(
            verify_signature("whsec_other", &h, body, 1100),
            Err("no matching signature")
        );
        assert_eq!(
            verify_signature("whsec_test", &h, br#"{"id":"evt_2"}"#, 1100),
            Err("no matching signature")
        );
        // Key rotation: several v1 entries, one matches.
        let both = format!("{h},v1={}", "0".repeat(64));
        assert!(verify_signature("whsec_test", &both, body, 1000).is_ok());
        assert!(verify_signature("whsec_test", "v1=abc", body, 1000).is_err());
    }
}
