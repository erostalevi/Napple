//! API error type: every failure is `{"error": {"code": "...", "message": "..."}}` with a matching HTTP status.

use axum::extract::rejection::JsonRejection;
use axum::http::{HeaderValue, StatusCode, header};
use axum::response::{IntoResponse, Response};
use serde_json::json;

#[derive(Debug, Clone)]
pub struct ApiError {
    pub status: StatusCode,
    pub code: &'static str,
    pub message: String,
    pub retry_after_secs: Option<u64>,
}

impl ApiError {
    pub fn new(status: StatusCode, code: &'static str, message: impl Into<String>) -> Self {
        ApiError {
            status,
            code,
            message: message.into(),
            retry_after_secs: None,
        }
    }
    pub fn bad_request(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::BAD_REQUEST, code, message)
    }
    pub fn unauthorized(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::UNAUTHORIZED, code, message)
    }
    pub fn forbidden(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::FORBIDDEN, code, message)
    }
    pub fn not_found(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::NOT_FOUND, code, message)
    }
    pub fn conflict(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::CONFLICT, code, message)
    }
    pub fn unprocessable(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::UNPROCESSABLE_ENTITY, code, message)
    }
    pub fn gone(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::GONE, code, message)
    }
    pub fn too_many(retry_after_secs: u64) -> Self {
        ApiError {
            status: StatusCode::TOO_MANY_REQUESTS,
            code: "rate_limited",
            message: format!("too many requests, retry in {retry_after_secs} s"),
            retry_after_secs: Some(retry_after_secs),
        }
    }
    pub fn unavailable(code: &'static str, message: impl Into<String>) -> Self {
        Self::new(StatusCode::SERVICE_UNAVAILABLE, code, message)
    }
    pub fn internal(message: impl Into<String>) -> Self {
        let message = message.into();
        tracing::error!(%message, "internal error");
        Self::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "internal",
            "internal error",
        )
    }
}

impl std::fmt::Display for ApiError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "{} {}: {}",
            self.status.as_u16(),
            self.code,
            self.message
        )
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        let body = json!({ "error": { "code": self.code, "message": self.message } });
        let mut resp = (self.status, axum::Json(body)).into_response();
        if self.status == StatusCode::UNAUTHORIZED {
            let v = format!(
                "Bearer realm=\"napple\", error=\"invalid_token\", error_description=\"{}\"",
                self.code
            );
            if let Ok(v) = HeaderValue::from_str(&v) {
                resp.headers_mut().insert(header::WWW_AUTHENTICATE, v);
            }
        }
        if let Some(s) = self.retry_after_secs {
            resp.headers_mut()
                .insert(header::RETRY_AFTER, HeaderValue::from(s));
        }
        resp
    }
}

impl From<rusqlite::Error> for ApiError {
    fn from(e: rusqlite::Error) -> Self {
        ApiError::internal(format!("sqlite: {e}"))
    }
}

impl From<axum::extract::rejection::PathRejection> for ApiError {
    fn from(e: axum::extract::rejection::PathRejection) -> Self {
        ApiError::bad_request("invalid_path", e.body_text())
    }
}

impl From<JsonRejection> for ApiError {
    fn from(e: JsonRejection) -> Self {
        ApiError::bad_request("invalid_json", e.body_text())
    }
}
