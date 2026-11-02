//! The end-to-end save-key exchange with real HPKE (RFC 9180 base mode, X25519-HKDF-SHA256 + HKDF-SHA256 +
//! AES-256-GCM) done by "devices" in the test: the server only ever sees public keys and wrapped blobs.

mod common;
use axum::http::StatusCode;
use common::*;
use hpke::aead::AesGcm256;
use hpke::kdf::HkdfSha256;
use hpke::kem::X25519HkdfSha256;
use hpke::{Deserializable, Kem, OpModeR, OpModeS, Serializable};
use napple_account::util::{b64_decode, b64url, random_bytes};
use serde_json::json;

type K = X25519HkdfSha256;

struct Device {
    sk: <K as Kem>::PrivateKey,
    pk: Vec<u8>,
}

fn device() -> Device {
    let (sk, pk) = K::gen_keypair(&mut rand::rngs::OsRng);
    Device {
        sk,
        pk: pk.to_bytes().to_vec(),
    }
}

fn info(account: &str, device: &str) -> Vec<u8> {
    format!("napple-account-key|{account}|{device}").into_bytes()
}

/// The recommended wrapped-key format: `enc (32 bytes) || ciphertext (32 + 16 bytes)`.
fn wrap(account_key: &[u8; 32], to_pk: &[u8], account: &str, device: &str) -> String {
    let pk = <K as Kem>::PublicKey::from_bytes(to_pk).unwrap();
    let (enc, ct) = hpke::single_shot_seal::<AesGcm256, HkdfSha256, K, _>(
        &OpModeS::Base,
        &pk,
        &info(account, device),
        account_key,
        b"",
        &mut rand::rngs::OsRng,
    )
    .unwrap();
    let mut out = enc.to_bytes().to_vec();
    out.extend_from_slice(&ct);
    b64url(&out)
}

fn unwrap(d: &Device, blob: &str, account: &str, device: &str) -> Vec<u8> {
    let raw = b64_decode(blob).unwrap();
    let enc = <K as Kem>::EncappedKey::from_bytes(&raw[..32]).unwrap();
    hpke::single_shot_open::<AesGcm256, HkdfSha256, K>(
        &OpModeR::Base,
        &d.sk,
        &enc,
        &info(account, device),
        &raw[32..],
        b"",
    )
    .unwrap()
}

#[tokio::test]
async fn bootstrap_request_grant_unwrap() {
    let t = T::new();
    let da = device();
    let a = t
        .sign_in_with("e2e@napple.test", "Mac", "macos", &da.pk)
        .await;

    // First device: no key yet -> it creates the account key and stores its own wrapped copy.
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["key_state"], "bootstrap");
    let account_key: [u8; 32] = random_bytes();
    let key_id = "k1-fingerprint";
    let (s, v) = t.req("POST", "/keys/grant", Some(&a.access), Some(json!({"device_id": a.device_id, "wrapped_account_key": wrap(&account_key, &da.pk, &a.account_id, &a.device_id), "key_id": key_id}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");

    // Second device signs in: pending, shows up in A's requests with its public key.
    let db = device();
    let b = t
        .sign_in_with("e2e@napple.test", "iPhone", "ios", &db.pk)
        .await;
    let (_, mine) = t.req("GET", "/keys/mine", Some(&b.access), None).await;
    assert_eq!(mine["error_code"], "key_pending");
    assert_eq!(mine["key_state"], "pending");
    let (_, reqs) = t.req("GET", "/keys/requests", Some(&a.access), None).await;
    let r = &reqs["requests"][0];
    assert_eq!(r["device_id"], b.device_id.as_str());
    assert_eq!(reqs["requests"].as_array().unwrap().len(), 1);

    // A pending device cannot approve a third one.
    let dc = device();
    let c = t
        .sign_in_with("e2e@napple.test", "Mac mini", "macos", &dc.pk)
        .await;
    let (s, v) = t.req("POST", "/keys/grant", Some(&b.access), Some(json!({"device_id": c.device_id, "wrapped_account_key": b64url(&[9u8; 80]), "key_id": key_id}))).await;
    assert_eq!(s, StatusCode::FORBIDDEN);
    assert_eq!(v["error_code"], "not_key_holder");

    // A approves B, wrapping to B's public key from the request list.
    let b_pk = b64_decode(r["pubkey"].as_str().unwrap()).unwrap();
    let (s, _) = t.req("POST", "/keys/grant", Some(&a.access), Some(json!({"device_id": b.device_id, "wrapped_account_key": wrap(&account_key, &b_pk, &a.account_id, &b.device_id), "key_id": key_id}))).await;
    assert_eq!(s, StatusCode::OK);
    let (s, mine) = t.req("GET", "/keys/mine", Some(&b.access), None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(mine["key_id"], key_id);
    assert_eq!(mine["granted_by"], a.device_id.as_str());
    assert_eq!(
        unwrap(
            &db,
            mine["wrapped_account_key"].as_str().unwrap(),
            &a.account_id,
            &b.device_id
        ),
        account_key
    );

    // A different account key (a concurrent bootstrap) is refused.
    let (s, v) = t.req("POST", "/keys/grant", Some(&b.access), Some(json!({"device_id": c.device_id, "wrapped_account_key": b64url(&[1u8; 80]), "key_id": "k2-other"}))).await;
    assert_eq!(s, StatusCode::CONFLICT);
    assert_eq!(v["key_id"], key_id);

    // Revoking B drops its wrapped key; requests list excludes revoked devices.
    t.req(
        "DELETE",
        &format!("/devices/{}", b.device_id),
        Some(&a.access),
        None,
    )
    .await;
    let (_, reqs) = t.req("GET", "/keys/requests", Some(&a.access), None).await;
    assert_eq!(
        reqs["requests"].as_array().unwrap().len(),
        1,
        "only C is still waiting"
    );
}

#[tokio::test]
async fn recovery_blob_round_trip() {
    let t = T::new();
    let da = device();
    let a = t
        .sign_in_with("rec@napple.test", "Mac", "macos", &da.pk)
        .await;
    let key: [u8; 32] = random_bytes();
    // No key yet: cannot set a recovery blob.
    let blob = b64url(&[7u8; 60]);
    let (s, _) = t
        .req(
            "POST",
            "/keys/recovery",
            Some(&a.access),
            Some(json!({"recovery_blob": blob, "key_id": "k1"})),
        )
        .await;
    assert_eq!(s, StatusCode::FORBIDDEN);
    t.req("POST", "/keys/grant", Some(&a.access), Some(json!({"device_id": a.device_id, "wrapped_account_key": wrap(&key, &da.pk, &a.account_id, &a.device_id), "key_id": "k1"}))).await;
    let kdf = json!({"alg": "argon2id", "salt": b64url(&[3u8; 16]), "m": 65536, "t": 3, "p": 1});
    let (s, _) = t
        .req(
            "POST",
            "/keys/recovery",
            Some(&a.access),
            Some(json!({"recovery_blob": blob, "kdf": kdf, "key_id": "k2"})),
        )
        .await;
    assert_eq!(s, StatusCode::CONFLICT, "recovery blob for another key");
    let (s, _) = t
        .req(
            "POST",
            "/keys/recovery",
            Some(&a.access),
            Some(json!({"recovery_blob": blob, "kdf": kdf, "key_id": "k1"})),
        )
        .await;
    assert_eq!(s, StatusCode::OK);
    let (_, me) = t.req("GET", "/me", Some(&a.access), None).await;
    assert_eq!(me["has_recovery"], true);

    // A brand-new device (all old devices lost) fetches the blob, unwraps it locally, then stores its own copy.
    let dn = device();
    let n = t
        .sign_in_with("rec@napple.test", "New Mac", "macos", &dn.pk)
        .await;
    let (s, r) = t.req("GET", "/keys/recovery", Some(&n.access), None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(r["recovery_blob"], blob.as_str());
    assert_eq!(r["kdf"]["alg"], "argon2id");
    let (s, _) = t.req("POST", "/keys/grant", Some(&n.access), Some(json!({"device_id": n.device_id, "wrapped_account_key": wrap(&key, &dn.pk, &n.account_id, &n.device_id), "key_id": "k1"}))).await;
    assert_eq!(s, StatusCode::OK, "self-grant after recovery");
    // Oversized blobs are refused.
    let (s, _) = t
        .req(
            "POST",
            "/keys/recovery",
            Some(&a.access),
            Some(json!({"recovery_blob": b64url(&vec![0u8; 5000]), "key_id": "k1"})),
        )
        .await;
    assert_eq!(s, StatusCode::BAD_REQUEST);
}

/// What the cloud-saves client sends: no `key_id`, and its own flat recovery record (stored and returned verbatim).
#[tokio::test]
async fn client_wire_shapes_without_key_id() {
    let t = T::new();
    let da = device();
    let a = t
        .sign_in_with("wire@napple.test", "Mac", "macos", &da.pk)
        .await;
    let key: [u8; 32] = random_bytes();
    let (s, v) = t.req("POST", "/keys/grant", Some(&a.access), Some(json!({"device_id": a.device_id, "wrapped_account_key": wrap(&key, &da.pk, &a.account_id, &a.device_id)}))).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    let (s, mine) = t.req("GET", "/keys/mine", Some(&a.access), None).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(
        unwrap(
            &da,
            mine["wrapped_account_key"].as_str().unwrap(),
            &a.account_id,
            &a.device_id
        ),
        key
    );
    let (s, _) = t.req("GET", "/keys/recovery", Some(&a.access), None).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let rec = json!({"kdf": "argon2id", "m_kib": 65536, "t": 3, "p": 1, "salt": b64url(&[1u8; 16]), "wrapped": b64url(&[2u8; 60])});
    let (s, _) = t
        .req("POST", "/keys/recovery", Some(&a.access), Some(rec.clone()))
        .await;
    assert_eq!(s, StatusCode::OK);
    let (s, got) = t.req("GET", "/keys/recovery", Some(&a.access), None).await;
    assert_eq!((s, got), (StatusCode::OK, rec));
    let (s, _) = t
        .req(
            "POST",
            "/keys/recovery",
            Some(&a.access),
            Some(json!(["not", "an", "object"])),
        )
        .await;
    assert_ne!(s, StatusCode::OK);
}
