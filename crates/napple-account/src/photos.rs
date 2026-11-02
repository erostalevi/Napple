//! Profile photos. Upload pipeline (contract): <= 5 MiB, <= 8192x8192 px, JPEG/PNG/WebP decided by magic bytes only
//! (never the file name or content type), full decode, ALL metadata dropped (the image is rebuilt from pixels),
//! EXIF orientation applied first, centre square crop, 256x256, JPEG q85. The original is never stored or served.
//! Storage is private: `NAPPLE_PHOTO_DIR` (default `<data dir>/photos`) or `NAPPLE_PHOTO_BUCKET` (S3), behind
//! [`PhotoStore`]. Serving goes through the tier rules (`napple-visibility`).

use crate::app::{AppState, Authed, limit};
use crate::aws::{CredentialProvider, EMPTY_SHA256, SignInput, sign_headers, uri_encode};
use crate::blob::valid_key;
use crate::config::S3Config;
use crate::error::{ApiError, ApiResult};
use crate::outbound::{BoxFut, OutRequest, Outbound};
use crate::profiles::get_row;
use crate::util::{new_id, sha256_hex};
use axum::extract::State;
use axum::extract::{Multipart, Path};
use axum::http::{HeaderMap, HeaderValue, StatusCode, header};
use axum::response::{IntoResponse, Response};
use image::{DynamicImage, ImageDecoder, ImageReader, Limits, imageops};
use napple_visibility::{TargetProfile, Tier, ViewKind, Viewer, can_view};
use std::io::Cursor;
use std::path::PathBuf;
use std::sync::Arc;

pub const MAX_BYTES: usize = 5 * 1024 * 1024;
pub const MAX_DIM: u32 = 8192;
pub const OUT_DIM: u32 = 256;
pub const JPEG_QUALITY: u8 = 85;

#[derive(Debug, PartialEq, Eq)]
pub enum PhotoError {
    Empty,
    TooLarge,
    Unsupported,
    Dimensions,
    Decode(String),
}

impl PhotoError {
    fn api(self) -> ApiError {
        let u = StatusCode::UNPROCESSABLE_ENTITY;
        match self {
            PhotoError::Empty => ApiError::new(u, "empty_photo", "the file is empty"),
            PhotoError::TooLarge => ApiError::new(
                StatusCode::PAYLOAD_TOO_LARGE,
                "photo_too_large",
                "photos are at most 5 MiB",
            ),
            PhotoError::Unsupported => {
                ApiError::new(u, "unsupported_type", "photos must be JPEG, PNG or WebP")
            }
            PhotoError::Dimensions => {
                ApiError::new(u, "photo_dimensions", "photos are at most 8192x8192 pixels")
            }
            PhotoError::Decode(_) => {
                ApiError::new(u, "decode_failed", "the image could not be decoded")
            }
        }
    }
}

fn sniff(b: &[u8]) -> Option<image::ImageFormat> {
    if b.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some(image::ImageFormat::Jpeg)
    } else if b.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]) {
        Some(image::ImageFormat::Png)
    } else if b.len() >= 12 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        Some(image::ImageFormat::WebP)
    } else {
        None
    }
}

/// True when the JPEG's marker structure runs cleanly from SOI to EOI. The decoder fills in missing scan data of a
/// truncated file with grey; the contract says a truncated image is a decode failure, so check the framing first.
fn jpeg_complete(b: &[u8]) -> bool {
    let mut i = 2;
    loop {
        // Next marker: 0xFF, optional 0xFF fill bytes, then the code.
        if i >= b.len() || b[i] != 0xFF {
            return false;
        }
        while i < b.len() && b[i] == 0xFF {
            i += 1;
        }
        let Some(&m) = b.get(i) else { return false };
        i += 1;
        match m {
            0xD9 => return true,
            0x00 | 0x01 | 0xD0..=0xD8 => continue,
            _ => {}
        }
        let Some(len) = b
            .get(i..i + 2)
            .map(|l| u16::from_be_bytes([l[0], l[1]]) as usize)
        else {
            return false;
        };
        if len < 2 {
            return false;
        }
        i += len;
        if m == 0xDA {
            // Entropy-coded data up to the next real marker (FF00 is a stuffed byte, FFD0-D7 restart markers).
            loop {
                match b.get(i) {
                    None => return false,
                    Some(0xFF) => match b.get(i + 1) {
                        None => return false,
                        Some(0x00) | Some(0xD0..=0xD7) => i += 2,
                        Some(_) => break,
                    },
                    Some(_) => i += 1,
                }
            }
        }
    }
}

/// The whole pipeline on raw upload bytes: returns the 256x256 JPEG to store.
pub fn process(bytes: &[u8]) -> Result<Vec<u8>, PhotoError> {
    if bytes.is_empty() {
        return Err(PhotoError::Empty);
    }
    if bytes.len() > MAX_BYTES {
        return Err(PhotoError::TooLarge);
    }
    let fmt = sniff(bytes).ok_or(PhotoError::Unsupported)?;
    if fmt == image::ImageFormat::Jpeg && !jpeg_complete(bytes) {
        return Err(PhotoError::Decode("truncated or malformed JPEG".into()));
    }
    let mut reader = ImageReader::with_format(Cursor::new(bytes), fmt);
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_DIM);
    limits.max_image_height = Some(MAX_DIM);
    // 8192x8192 RGBA is 256 MiB; refuse allocations beyond that plus slack before any pixel buffer exists.
    limits.max_alloc = Some(320 * 1024 * 1024);
    reader.limits(limits);
    let mut decoder = reader.into_decoder().map_err(map_img_err)?;
    let (w, h) = decoder.dimensions();
    if w == 0 || h == 0 {
        return Err(PhotoError::Decode("zero dimension".into()));
    }
    if w > MAX_DIM || h > MAX_DIM {
        return Err(PhotoError::Dimensions);
    }
    let orientation = decoder.orientation().ok();
    let mut img = DynamicImage::from_decoder(decoder).map_err(map_img_err)?;
    if let Some(o) = orientation {
        img.apply_orientation(o);
    }
    let (w, h) = (img.width(), img.height());
    let side = w.min(h);
    let cropped = img.crop_imm((w - side) / 2, (h - side) / 2, side, side);
    drop(img);
    let small = cropped.resize_exact(OUT_DIM, OUT_DIM, imageops::FilterType::Lanczos3);
    drop(cropped);
    // Transparent pixels go onto white (JPEG has no alpha).
    let rgba = small.to_rgba8();
    let mut rgb = image::RgbImage::new(OUT_DIM, OUT_DIM);
    for (o, p) in rgb.pixels_mut().zip(rgba.pixels()) {
        let a = p[3] as u32;
        let mix = |c: u8| ((c as u32 * a + 255 * (255 - a) + 127) / 255) as u8;
        *o = image::Rgb([mix(p[0]), mix(p[1]), mix(p[2])]);
    }
    let mut out = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, JPEG_QUALITY)
        .encode_image(&rgb)
        .map_err(|e| PhotoError::Decode(e.to_string()))?;
    Ok(out)
}

fn map_img_err(e: image::ImageError) -> PhotoError {
    match e {
        image::ImageError::Limits(_) => PhotoError::Dimensions,
        e => PhotoError::Decode(e.to_string()),
    }
}

// ---- storage ----

pub trait PhotoStore: Send + Sync {
    fn put<'a>(&'a self, key: &'a str, data: Vec<u8>) -> BoxFut<'a, Result<(), String>>;
    fn get<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<Option<Vec<u8>>, String>>;
    /// Idempotent.
    fn delete<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<(), String>>;
}

pub struct FsPhotoStore {
    dir: PathBuf,
}

impl FsPhotoStore {
    pub fn new(dir: PathBuf) -> FsPhotoStore {
        FsPhotoStore { dir }
    }
    fn path(&self, key: &str) -> Result<PathBuf, String> {
        if !valid_key(key) {
            return Err("invalid photo key".into());
        }
        Ok(self.dir.join(key))
    }
}

impl PhotoStore for FsPhotoStore {
    fn put<'a>(&'a self, key: &'a str, data: Vec<u8>) -> BoxFut<'a, Result<(), String>> {
        Box::pin(async move {
            let p = self.path(key)?;
            let parent = p.parent().ok_or("no parent")?;
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            let tmp = p.with_extension("tmp");
            std::fs::write(&tmp, &data).map_err(|e| e.to_string())?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let _ = std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o600));
            }
            std::fs::rename(&tmp, &p).map_err(|e| e.to_string())
        })
    }
    fn get<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<Option<Vec<u8>>, String>> {
        Box::pin(async move {
            match std::fs::read(self.path(key)?) {
                Ok(b) => Ok(Some(b)),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
                Err(e) => Err(e.to_string()),
            }
        })
    }
    fn delete<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<(), String>> {
        Box::pin(async move {
            match std::fs::remove_file(self.path(key)?) {
                Ok(()) => Ok(()),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(e) => Err(e.to_string()),
            }
        })
    }
}

/// Private S3 bucket, server-side signed requests (objects are never presigned to clients).
pub struct S3PhotoStore {
    cfg: S3Config,
    creds: Arc<CredentialProvider>,
    out: Arc<dyn Outbound>,
}

impl S3PhotoStore {
    pub fn new(cfg: S3Config, creds: Arc<CredentialProvider>, out: Arc<dyn Outbound>) -> Self {
        S3PhotoStore { cfg, creds, out }
    }

    fn now() -> i64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0)
    }

    /// (base url, host header, encoded path)
    fn target(&self, key: &str) -> (String, String, String) {
        match &self.cfg.endpoint {
            Some(ep) => {
                let host = ep
                    .split("://")
                    .nth(1)
                    .unwrap_or(ep)
                    .split('/')
                    .next()
                    .unwrap_or("")
                    .to_string();
                (
                    ep.clone(),
                    host,
                    format!(
                        "/{}/{}",
                        uri_encode(&self.cfg.bucket, false),
                        uri_encode(key, true)
                    ),
                )
            }
            None => {
                let host = format!("{}.s3.{}.amazonaws.com", self.cfg.bucket, self.cfg.region);
                (
                    format!("https://{host}"),
                    host,
                    format!("/{}", uri_encode(key, true)),
                )
            }
        }
    }

    async fn call(
        &self,
        method: &str,
        key: &str,
        body: Vec<u8>,
        content_type: Option<&str>,
    ) -> Result<crate::outbound::OutResponse, String> {
        if !valid_key(key) {
            return Err("invalid photo key".into());
        }
        let now = Self::now();
        let creds = self.creds.get(now).await?;
        let (base, host, path) = self.target(key);
        let payload = if body.is_empty() {
            EMPTY_SHA256.to_string()
        } else {
            sha256_hex(&body)
        };
        let mut signed = vec![("host".to_string(), host)];
        if let Some(ct) = content_type {
            signed.push(("content-type".into(), ct.into()));
        }
        let mut headers = sign_headers(
            &creds,
            SignInput {
                method,
                path: &path,
                query: vec![],
                headers: signed,
                payload_hash: &payload,
                region: &self.cfg.region,
                service: "s3",
                now,
            },
        );
        if let Some(ct) = content_type {
            headers.push(("content-type".into(), ct.into()));
        }
        self.out
            .send(OutRequest {
                method: method.into(),
                url: format!("{base}{path}"),
                headers,
                body,
            })
            .await
    }
}

impl PhotoStore for S3PhotoStore {
    fn put<'a>(&'a self, key: &'a str, data: Vec<u8>) -> BoxFut<'a, Result<(), String>> {
        Box::pin(async move {
            let r = self.call("PUT", key, data, Some("image/jpeg")).await?;
            match r.status {
                200 | 201 | 204 => Ok(()),
                s => Err(format!("S3 PUT: HTTP {s}")),
            }
        })
    }
    fn get<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<Option<Vec<u8>>, String>> {
        Box::pin(async move {
            let r = self.call("GET", key, vec![], None).await?;
            match r.status {
                200 => Ok(Some(r.body)),
                404 => Ok(None),
                s => Err(format!("S3 GET: HTTP {s}")),
            }
        })
    }
    fn delete<'a>(&'a self, key: &'a str) -> BoxFut<'a, Result<(), String>> {
        Box::pin(async move {
            let r = self.call("DELETE", key, vec![], None).await?;
            match r.status {
                200 | 204 | 404 => Ok(()),
                s => Err(format!("S3 DELETE: HTTP {s}")),
            }
        })
    }
}

/// `NAPPLE_PHOTO_BUCKET` -> S3 (region `NAPPLE_PHOTO_REGION`, else `NAPPLE_ACCOUNT_S3_REGION`, else us-east-1;
/// optional `NAPPLE_PHOTO_ENDPOINT` for S3-compatible stores); else `NAPPLE_PHOTO_DIR`, else `<data dir>/photos`.
pub fn default_store(data_dir: &std::path::Path, out: &Arc<dyn Outbound>) -> Arc<dyn PhotoStore> {
    let get = |k: &str| std::env::var(k).ok().filter(|s| !s.is_empty());
    if let Some(bucket) = get("NAPPLE_PHOTO_BUCKET") {
        let cfg = S3Config {
            bucket,
            region: get("NAPPLE_PHOTO_REGION")
                .or_else(|| get("NAPPLE_ACCOUNT_S3_REGION"))
                .unwrap_or_else(|| "us-east-1".into()),
            endpoint: get("NAPPLE_PHOTO_ENDPOINT"),
        };
        let creds = Arc::new(CredentialProvider::from_env(out.clone()));
        return Arc::new(S3PhotoStore::new(cfg, creds, out.clone()));
    }
    Arc::new(FsPhotoStore::new(
        get("NAPPLE_PHOTO_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| data_dir.join("photos")),
    ))
}

// ---- handlers ----

fn decode_permits() -> &'static tokio::sync::Semaphore {
    static S: std::sync::OnceLock<tokio::sync::Semaphore> = std::sync::OnceLock::new();
    S.get_or_init(|| tokio::sync::Semaphore::new(2))
}

fn jpeg_response(bytes: Vec<u8>) -> Response {
    let mut r = (StatusCode::OK, bytes).into_response();
    let h = r.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("image/jpeg"));
    h.insert(
        header::CACHE_CONTROL,
        HeaderValue::from_static("private, max-age=300"),
    );
    h.insert(
        header::X_CONTENT_TYPE_OPTIONS,
        HeaderValue::from_static("nosniff"),
    );
    r
}

pub async fn upload(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
    mut mp: Multipart,
) -> ApiResult<Json> {
    limit(&app, "photo", &a.account_id, 20, 3600)?;
    {
        let db = app.db();
        if !matches!(get_row(&db, &id)?, Some(r) if r.account_id == a.account_id) {
            return Err(ApiError::not_found("no such profile"));
        }
    }
    // The first part is the photo, whatever its name, file name or content type say.
    let field = mp
        .next_field()
        .await
        .map_err(mp_err)?
        .ok_or_else(|| ApiError::bad_request("multipart body with a photo part expected"))?;
    let bytes = field.bytes().await.map_err(mp_err)?;
    if bytes.len() > MAX_BYTES {
        return Err(PhotoError::TooLarge.api());
    }
    let data = bytes.to_vec();
    let _permit = decode_permits()
        .acquire()
        .await
        .map_err(|_| ApiError::internal("decode semaphore closed"))?;
    let jpeg = tokio::task::spawn_blocking(move || process(&data))
        .await
        .map_err(|e| ApiError::internal(format!("decode task: {e}")))?
        .map_err(|e| {
            if let PhotoError::Decode(m) = &e {
                crate::log::warn(&format!("photo decode failed: {m}"));
            }
            e.api()
        })?;
    drop(_permit);

    let key = format!("photos/{}/{}/{}", a.account_id, id, new_id());
    app.photos
        .put(&key, jpeg)
        .await
        .map_err(|e| ApiError::unavailable("photo_store", format!("photo storage: {e}")))?;
    let now = app.now();
    let old: Option<String> = {
        let db = app.db();
        let old = db.query_row("SELECT photo_key FROM profiles WHERE id = ?1", [&id], |r| {
            r.get(0)
        })?;
        db.execute(
            "UPDATE profiles SET photo_key = ?1, photo_updated_at = ?2, updated_at = ?2 WHERE id = ?3",
            rusqlite::params![key, now, id],
        )?;
        old
    };
    if let Some(o) = old
        && let Err(e) = app.photos.delete(&o).await
    {
        crate::log::warn(&format!("old photo delete {o}: {e}"));
    }
    let db = app.db();
    let r = get_row(&db, &id)?.ok_or_else(|| ApiError::internal("profile vanished"))?;
    Ok(axum::Json(r.json()))
}

type Json = axum::Json<serde_json::Value>;

fn mp_err(e: axum::extract::multipart::MultipartError) -> ApiError {
    if e.status() == StatusCode::PAYLOAD_TOO_LARGE {
        PhotoError::TooLarge.api()
    } else {
        ApiError::bad_request("malformed multipart body")
    }
}

pub async fn delete(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<StatusCode> {
    let old: Option<String> = {
        let db = app.db();
        if !matches!(get_row(&db, &id)?, Some(r) if r.account_id == a.account_id) {
            return Err(ApiError::not_found("no such profile"));
        }
        let old = db.query_row("SELECT photo_key FROM profiles WHERE id = ?1", [&id], |r| {
            r.get(0)
        })?;
        db.execute(
            "UPDATE profiles SET photo_key = NULL, photo_updated_at = NULL, updated_at = ?1 WHERE id = ?2",
            rusqlite::params![app.now(), id],
        )?;
        old
    };
    if let Some(o) = old {
        app.photos
            .delete(&o)
            .await
            .map_err(|e| ApiError::unavailable("photo_store", format!("photo storage: {e}")))?;
    }
    Ok(StatusCode::NO_CONTENT)
}

/// The viewer here is an authenticated account without friend data: friends-only tiers answer 404 to non-owners
/// (napple-social, which knows friendships, fetches through the internal route and applies `can_view` itself).
pub async fn get(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    let (key, tier, account) = {
        let db = app.db();
        let r = get_row(&db, &id)?.ok_or_else(|| ApiError::not_found("no photo"))?;
        let key: Option<String> =
            db.query_row("SELECT photo_key FROM profiles WHERE id = ?1", [&id], |r| {
                r.get(0)
            })?;
        (key, r.tier, r.account_id)
    };
    let viewer = Viewer {
        profile_id: None,
        same_account: account == a.account_id,
        ..Default::default()
    };
    let target = TargetProfile {
        profile_id: id,
        tier: Tier::from_i64(tier).unwrap_or(Tier::Private),
    };
    if !can_view(&viewer, &target, ViewKind::NicknameAndPhoto) {
        return Err(ApiError::not_found("no photo"));
    }
    serve(&app, key).await
}

async fn serve(app: &AppState, key: Option<String>) -> ApiResult<Response> {
    let key = key.ok_or_else(|| ApiError::not_found("no photo"))?;
    match app
        .photos
        .get(&key)
        .await
        .map_err(|e| ApiError::unavailable("photo_store", format!("photo storage: {e}")))?
    {
        Some(b) => Ok(jpeg_response(b)),
        None => Err(ApiError::not_found("no photo")),
    }
}

/// napple-social: the raw stored photo, no tier rules (the caller applies them).
pub async fn internal_get(
    State(app): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    crate::profiles::internal_auth(&headers)?;
    let key: Option<String> = {
        let db = app.db();
        db.query_row("SELECT photo_key FROM profiles WHERE id = ?1", [&id], |r| {
            r.get(0)
        })
        .map_err(|_| ApiError::not_found("no photo"))?
    };
    serve(&app, key).await
}

/// Called on account deletion: removes every photo object of the given keys (best effort, logged).
pub async fn delete_all(app: &AppState, keys: Vec<String>) {
    for k in keys {
        if let Err(e) = app.photos.delete(&k).await {
            crate::log::warn(&format!("photo delete {k}: {e}"));
        }
    }
}
