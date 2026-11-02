//! Presigned S3 URLs against a local mock S3 endpoint (path-style, 127.0.0.1) that checks what real S3 checks:
//! the SigV4 query signature and expiry, the signed Content-Length / Content-Type / x-amz-checksum-sha256 headers and
//! the body checksum on PUT, and header-signed HEAD/DELETE from the server. Loopback only, no AWS.

mod common;
use axum::Router;
use axum::body::Bytes;
use axum::extract::State;
use axum::http::{HeaderMap, Method, StatusCode, Uri};
use axum::response::{IntoResponse, Response};
use common::*;
use napple_account::aws::{
    CredentialProvider, Credentials, EMPTY_SHA256, SignInput, sign_headers, verify_presigned,
};
use napple_account::blob::{BlobStore, S3BlobStore};
use napple_account::config::S3Config;
use napple_account::outbound::{Outbound, ReqwestOutbound};
use napple_account::util::{b64std, sha256, sha256_hex};
use percent_encoding::percent_decode_str;
use serde_json::json;
use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};

const BUCKET: &str = "napple-saves-123456789012";

fn creds() -> Credentials {
    Credentials {
        access_key: "AKIDNAPPLETESTONLY".into(),
        secret_key: "napple-test-secret-not-a-real-key".into(),
        session_token: Some("test-session-token".into()),
        expires_at: None,
    }
}

#[derive(Default)]
struct MockS3 {
    objects: Mutex<HashMap<String, Vec<u8>>>,
    now: AtomicI64,
    log: Mutex<Vec<String>>,
}

fn header_pairs(h: &HeaderMap) -> Vec<(String, String)> {
    h.iter()
        .map(|(k, v)| (k.to_string(), v.to_str().unwrap_or("").to_string()))
        .collect()
}

async fn handle(
    State(s): State<Arc<MockS3>>,
    method: Method,
    uri: Uri,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let path = uri.path().to_string();
    let Some(key) = path
        .strip_prefix(&format!("/{BUCKET}/"))
        .map(str::to_string)
    else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let now = s.now.load(Ordering::SeqCst);
    let query: Vec<(String, String)> = uri
        .query()
        .unwrap_or("")
        .split('&')
        .filter(|p| !p.is_empty())
        .map(|p| {
            let (k, v) = p.split_once('=').unwrap_or((p, ""));
            (
                percent_decode_str(k).decode_utf8_lossy().into_owned(),
                percent_decode_str(v).decode_utf8_lossy().into_owned(),
            )
        })
        .collect();
    s.log.lock().unwrap().push(format!("{method} {key}"));
    let presigned = query.iter().any(|(k, _)| k == "X-Amz-Signature");
    if presigned {
        if !verify_presigned(
            &creds(),
            method.as_str(),
            &path,
            &query,
            &header_pairs(&headers),
            "us-east-1",
            "s3",
            now,
        ) {
            return (StatusCode::FORBIDDEN, "SignatureDoesNotMatch or expired").into_response();
        }
        let signed = query
            .iter()
            .find(|(k, _)| k == "X-Amz-SignedHeaders")
            .unwrap()
            .1
            .clone();
        match method {
            Method::PUT => {
                assert!(
                    signed.contains("content-length")
                        && signed.contains("x-amz-checksum-sha256")
                        && signed.contains("content-type"),
                    "{signed}"
                );
                let want = headers
                    .get("x-amz-checksum-sha256")
                    .and_then(|v| v.to_str().ok())
                    .unwrap_or("");
                if want != b64std(&sha256(&body)) {
                    return (StatusCode::BAD_REQUEST, "BadDigest").into_response();
                }
                s.objects.lock().unwrap().insert(key, body.to_vec());
                StatusCode::OK.into_response()
            }
            Method::GET => match s.objects.lock().unwrap().get(&key) {
                Some(b) => b.clone().into_response(),
                None => StatusCode::NOT_FOUND.into_response(),
            },
            _ => StatusCode::METHOD_NOT_ALLOWED.into_response(),
        }
    } else {
        // Header-signed request from the server: recompute the Authorization header.
        let date = headers
            .get("x-amz-date")
            .and_then(|v| v.to_str().ok())
            .unwrap_or("");
        let t = napple_account::aws::parse_amz_date(date).unwrap_or(0);
        let host = headers
            .get("host")
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_string();
        let expect = sign_headers(
            &creds(),
            SignInput {
                method: method.as_str(),
                path: &path,
                query: vec![],
                headers: vec![("host".into(), host)],
                payload_hash: EMPTY_SHA256,
                region: "us-east-1",
                service: "s3",
                now: t,
            },
        );
        let want = &expect.iter().find(|(k, _)| k == "authorization").unwrap().1;
        if headers.get("authorization").and_then(|v| v.to_str().ok()) != Some(want.as_str())
            || headers.get("x-amz-security-token").is_none()
        {
            return (StatusCode::FORBIDDEN, "SignatureDoesNotMatch").into_response();
        }
        match method {
            Method::HEAD => match s.objects.lock().unwrap().get(&key) {
                Some(b) => ([("content-length", b.len().to_string())]).into_response(),
                None => StatusCode::NOT_FOUND.into_response(),
            },
            Method::DELETE => {
                s.objects.lock().unwrap().remove(&key);
                StatusCode::NO_CONTENT.into_response()
            }
            _ => StatusCode::METHOD_NOT_ALLOWED.into_response(),
        }
    }
}

async fn start_mock() -> (Arc<MockS3>, String) {
    let s = Arc::new(MockS3::default());
    s.now.store(T0, Ordering::SeqCst);
    let app = Router::new().fallback(handle).with_state(s.clone());
    let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = l.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(l, app).await.unwrap() });
    (s, format!("http://{addr}"))
}

#[tokio::test]
async fn presigned_put_get_head_delete_against_mock_s3() {
    let (mock, endpoint) = start_mock().await;
    let out: Arc<dyn Outbound> = Arc::new(ReqwestOutbound::new());
    let store: Arc<dyn BlobStore> = Arc::new(S3BlobStore::new(
        S3Config {
            bucket: BUCKET.into(),
            region: "us-east-1".into(),
            endpoint: Some(endpoint.clone()),
        },
        Arc::new(CredentialProvider::fixed(creds(), out.clone())),
        out,
    ));
    let t = T::with(&[], Some(store));
    let a = t.sign_in("s3@napple.test", "Mac").await;
    let p = t.first_profile(&a).await;
    let http = reqwest::Client::new();
    let data = b"NAPPLESV\x01 encrypted bytes from the client".to_vec();

    let (s, c) = t.req("POST", &format!("/saves/{LAS}/{p}/versions"), Some(&a.access), Some(json!({"base_version": 0, "size": data.len(), "sha256_ciphertext": sha256_hex(&data)}))).await;
    assert_eq!(s, StatusCode::CREATED, "{c}");
    let url = c["upload_url"].as_str().unwrap();
    assert!(
        url.starts_with(&format!(
            "{endpoint}/{BUCKET}/saves/{}/{LAS}/{p}/",
            a.account_id
        )),
        "{url}"
    );
    assert!(url.contains("X-Amz-Expires=900") && url.contains("X-Amz-Security-Token="));
    assert_eq!(c["expires_at"], T0 + 900);
    let put = |body: Vec<u8>| {
        let mut r = http.put(url);
        for (k, v) in c["upload_headers"].as_object().unwrap() {
            if k != "content-length" {
                r = r.header(k.as_str(), v.as_str().unwrap());
            }
        }
        r.body(body)
    };

    // A different body (same length) fails S3's checksum; a different length fails the signature.
    let mut evil = data.clone();
    evil[0] ^= 1;
    assert_eq!(put(evil).send().await.unwrap().status().as_u16(), 400);
    assert_eq!(
        put(b"short".to_vec())
            .send()
            .await
            .unwrap()
            .status()
            .as_u16(),
        403
    );
    // Commit before a successful upload is refused (server HEADs the object).
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/1/commit"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(
        (s, v["error_code"].as_str()),
        (StatusCode::BAD_REQUEST, Some("upload_missing"))
    );
    assert_eq!(
        put(data.clone()).send().await.unwrap().status().as_u16(),
        200
    );
    let (s, v) = t
        .req(
            "POST",
            &format!("/saves/{LAS}/{p}/versions/1/commit"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK, "{v}");

    let (s, g) = t
        .req(
            "GET",
            &format!("/saves/{LAS}/{p}/versions/1"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let got = http
        .get(g["download_url"].as_str().unwrap())
        .send()
        .await
        .unwrap()
        .bytes()
        .await
        .unwrap();
    assert_eq!(got.to_vec(), data);
    // The URL stops working after 15 minutes.
    mock.now.store(T0 + 901, Ordering::SeqCst);
    assert_eq!(
        http.get(g["download_url"].as_str().unwrap())
            .send()
            .await
            .unwrap()
            .status()
            .as_u16(),
        403
    );
    mock.now.store(T0, Ordering::SeqCst);

    // Account deletion removes the object from the bucket.
    assert_eq!(mock.objects.lock().unwrap().len(), 1);
    let (s, _) = t
        .req(
            "DELETE",
            "/me",
            Some(&a.access),
            Some(json!({"confirm_email": "s3@napple.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert_eq!(mock.objects.lock().unwrap().len(), 0);
    let log = mock.log.lock().unwrap().clone();
    assert!(
        log.iter().any(|l| l.starts_with("HEAD ")) && log.iter().any(|l| l.starts_with("DELETE ")),
        "{log:?}"
    );
}
