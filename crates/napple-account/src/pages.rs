//! Health and JWKS.

use crate::app::AppState;
use axum::Json;
use axum::extract::State;
use axum::http::header;
use axum::response::{IntoResponse, Response};
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
