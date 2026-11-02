//! Health, JWKS, and the few small HTML pages (magic link landing, plans, after-checkout).

use crate::app::AppState;
use axum::Json;
use axum::extract::State;
use axum::http::{StatusCode, header};
use axum::response::{Html, IntoResponse, Response};
use serde_json::Value;

pub async fn healthz() -> &'static str {
    "ok napple-account\n"
}

pub async fn jwks(State(app): State<AppState>) -> Response {
    (
        [(header::CACHE_CONTROL, "public, max-age=300")],
        Json::<Value>(app.jwt.jwks()),
    )
        .into_response()
}

const STYLE: &str = "body{font:16px/1.5 -apple-system,system-ui,sans-serif;max-width:34em;margin:3em auto;padding:0 16px;color:#1d1d1f;background:#fff}\
a.button{display:inline-block;padding:.6em 1.2em;border-radius:8px;background:#0a66ff;color:#fff;text-decoration:none}\
@media(prefers-color-scheme:dark){body{color:#f5f5f7;background:#111}}";

fn page(title: &str, body: &str) -> Html<String> {
    Html(format!(
        "<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>\
         <meta name=referrer content=no-referrer><title>{title}</title><style>{STYLE}</style><body>{body}</body></html>"
    ))
}

/// The magic link lands here. The token is in the URL fragment (never sent to a server); the page hands it to the app.
pub async fn email_link() -> Response {
    let body = "<h1>Sign in to Napple</h1>\
        <p id=msg>Opening Napple&hellip;</p>\
        <p><a class=button id=open href='napple://auth/email'>Open Napple</a></p>\
        <p>If nothing happens, type the 6-digit code from the email into Napple instead.</p>\
        <script>var f=location.hash.slice(1);if(f){var a=document.getElementById('open');a.href='napple://auth/email#'+f;\
        history.replaceState(null,'',location.pathname);location.href=a.href;}else{document.getElementById('msg').textContent='This link is incomplete.';}</script>";
    (
        [(header::CACHE_CONTROL, "no-store")],
        page("Sign in to Napple", body),
    )
        .into_response()
}

pub async fn billing_page(State(app): State<AppState>) -> Response {
    if app.cfg.stripe.is_none() {
        return (StatusCode::SERVICE_UNAVAILABLE, page("Napple Plus", "<h1>Napple Plus</h1><p>Billing is not available on this server. Every account is on Free.</p>")).into_response();
    }
    let gib = |b: u64| {
        format!(
            "{:.0} {}",
            if b >= 1 << 30 {
                b as f64 / (1u64 << 30) as f64
            } else {
                b as f64 / (1u64 << 20) as f64
            },
            if b >= 1 << 30 { "GiB" } else { "MiB" }
        )
    };
    let f = &app.cfg.free;
    let p = &app.cfg.plus;
    let titles = |n: u32| {
        if n == 0 {
            "unlimited titles".to_string()
        } else if n == 1 {
            "1 title".into()
        } else {
            format!("{n} titles")
        }
    };
    let body = format!(
        "<h1>Napple Plus</h1>\
         <p><b>Free</b>: cloud saves for {}, {} versions each, {}. Accounts, passkeys, devices, profiles and online play are always free.</p>\
         <p><b>Napple Plus</b>: cloud saves for {}, {} versions each, {}.</p>\
         <p>To upgrade, open <b>Napple &rsaquo; Account &rsaquo; Upgrade</b> on your Mac or iPhone; it opens a secure Stripe checkout.</p>\
         <p><a class=button href='napple://account/upgrade'>Open Napple</a></p>\
         <p>Your saves always stay downloadable: if a subscription ends, titles beyond the Free plan stay read-only for {} days.</p>",
        titles(f.synced_titles),
        f.versions_per_title,
        gib(f.quota_bytes),
        titles(p.synced_titles),
        p.versions_per_title,
        gib(p.quota_bytes),
        app.cfg.readonly_days
    );
    page("Napple Plus", &body).into_response()
}

pub async fn billing_done() -> Response {
    page("Napple", "<h1>All set</h1><p>You can close this page and return to Napple. Your plan updates within a minute.</p>").into_response()
}
