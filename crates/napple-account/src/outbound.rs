//! Outbound HTTP (Stripe, SES, S3 HEAD/DELETE, EC2 instance metadata) behind a trait so tests run with no network.

use std::future::Future;
use std::pin::Pin;
use std::sync::Mutex;

pub type BoxFut<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

#[derive(Clone, Debug)]
pub struct OutRequest {
    pub method: String,
    pub url: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct OutResponse {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl OutResponse {
    pub fn json(&self) -> Option<serde_json::Value> {
        serde_json::from_slice(&self.body).ok()
    }
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }
}

pub trait Outbound: Send + Sync {
    fn send(&self, req: OutRequest) -> BoxFut<'_, Result<OutResponse, String>>;
}

/// Real HTTP via reqwest + rustls (webpki roots).
pub struct ReqwestOutbound {
    client: reqwest::Client,
}

impl ReqwestOutbound {
    pub fn new() -> ReqwestOutbound {
        ReqwestOutbound {
            client: reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(20))
                .redirect(reqwest::redirect::Policy::none())
                .build()
                .expect("reqwest client"),
        }
    }
}

impl Default for ReqwestOutbound {
    fn default() -> Self {
        Self::new()
    }
}

impl Outbound for ReqwestOutbound {
    fn send(&self, req: OutRequest) -> BoxFut<'_, Result<OutResponse, String>> {
        Box::pin(async move {
            let method =
                reqwest::Method::from_bytes(req.method.as_bytes()).map_err(|e| e.to_string())?;
            let mut b = self.client.request(method, &req.url);
            for (k, v) in &req.headers {
                b = b.header(k, v);
            }
            let r = b
                .body(req.body)
                .send()
                .await
                .map_err(|e| format!("{}: {e}", redact_url(&req.url)))?;
            let status = r.status().as_u16();
            let headers = r
                .headers()
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_str().unwrap_or("").to_string()))
                .collect();
            let body = r.bytes().await.map_err(|e| e.to_string())?.to_vec();
            Ok(OutResponse {
                status,
                headers,
                body,
            })
        })
    }
}

/// Drops the query string (presigned signatures, tokens) before a URL reaches a log line.
pub fn redact_url(u: &str) -> &str {
    u.split('?').next().unwrap_or(u)
}

type Handler = dyn Fn(&OutRequest) -> OutResponse + Send + Sync;

/// Test double: records every request and answers through a closure.
pub struct MockOutbound {
    pub requests: Mutex<Vec<OutRequest>>,
    handler: Box<Handler>,
}

impl MockOutbound {
    pub fn new(
        handler: impl Fn(&OutRequest) -> OutResponse + Send + Sync + 'static,
    ) -> MockOutbound {
        MockOutbound {
            requests: Mutex::new(Vec::new()),
            handler: Box::new(handler),
        }
    }
    pub fn requests(&self) -> Vec<OutRequest> {
        self.requests.lock().unwrap().clone()
    }
}

impl Outbound for MockOutbound {
    fn send(&self, req: OutRequest) -> BoxFut<'_, Result<OutResponse, String>> {
        let resp = (self.handler)(&req);
        self.requests.lock().unwrap().push(req);
        Box::pin(async move { Ok(resp) })
    }
}

pub fn json_response(status: u16, v: serde_json::Value) -> OutResponse {
    OutResponse {
        status,
        headers: vec![("content-type".into(), "application/json".into())],
        body: v.to_string().into_bytes(),
    }
}
