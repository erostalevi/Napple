//! API errors: `{"error": "<code>", "message": "...", ...extra}` with the matching status.

use axum::http::{HeaderValue, StatusCode, header};
use axum::response::{IntoResponse, Response};
use serde_json::{Map, Value, json};

#[derive(Debug)]
pub struct ApiError {
    pub status: StatusCode,
    pub code: &'static str,
    pub message: String,
    pub extra: Map<String, Value>,
    pub retry_after: Option<u64>,
}

pub type ApiResult<T> = Result<T, ApiError>;

impl ApiError {
    pub fn new(status: StatusCode, code: &'static str, message: impl Into<String>) -> ApiError {
        ApiError {
            status,
            code,
            message: message.into(),
            extra: Map::new(),
            retry_after: None,
        }
    }
    pub fn with(mut self, k: &str, v: impl Into<Value>) -> ApiError {
        self.extra.insert(k.to_string(), v.into());
        self
    }
    pub fn bad_request(msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::BAD_REQUEST, "bad_request", msg)
    }
    pub fn unauthorized(msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::UNAUTHORIZED, "unauthorized", msg)
    }
    pub fn forbidden(msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::FORBIDDEN, "forbidden", msg)
    }
    pub fn not_found(msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::NOT_FOUND, "not_found", msg)
    }
    pub fn conflict(code: &'static str, msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::CONFLICT, code, msg)
    }
    pub fn too_many(retry_after: u64) -> ApiError {
        let mut e = ApiError::new(
            StatusCode::TOO_MANY_REQUESTS,
            "rate_limited",
            "too many requests, slow down",
        );
        e.retry_after = Some(retry_after.max(1));
        e.with("retry_after", retry_after.max(1))
    }
    pub fn unavailable(code: &'static str, msg: impl Into<String>) -> ApiError {
        ApiError::new(StatusCode::SERVICE_UNAVAILABLE, code, msg)
    }
    pub fn internal(msg: impl std::fmt::Display) -> ApiError {
        // The detail goes to the log only; clients get a generic message.
        crate::log::error(&format!("internal error: {msg}"));
        ApiError::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "internal",
            "internal error",
        )
    }
}

impl From<rusqlite::Error> for ApiError {
    fn from(e: rusqlite::Error) -> ApiError {
        ApiError::internal(format!("db: {e}"))
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        let mut body = self.extra;
        body.insert("error".into(), json!(self.code));
        body.insert("message".into(), json!(self.message));
        let mut r = (self.status, axum::Json(Value::Object(body))).into_response();
        if let Some(s) = self.retry_after {
            r.headers_mut()
                .insert(header::RETRY_AFTER, HeaderValue::from(s));
        }
        r
    }
}
