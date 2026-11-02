//! Photo pipeline with hostile inputs, and the upload / serve / tier rules over HTTP.

mod common;
use axum::body::Body;
use axum::http::{Request, StatusCode};
use common::*;
use image::{ImageEncoder, RgbImage, RgbaImage};
use napple_account::photos::{MAX_BYTES, PhotoError, process};
use serde_json::{Value, json};

fn jpeg(w: u32, h: u32) -> Vec<u8> {
    let img = RgbImage::from_fn(w, h, |x, y| {
        image::Rgb([(x % 256) as u8, (y % 256) as u8, 128])
    });
    let mut out = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, 90)
        .encode_image(&img)
        .unwrap();
    out
}

fn png(w: u32, h: u32) -> Vec<u8> {
    let img = RgbaImage::from_fn(w, h, |x, y| {
        image::Rgba([(x % 256) as u8, 40, (y % 256) as u8, 255])
    });
    let mut out = Vec::new();
    image::codecs::png::PngEncoder::new(&mut out)
        .write_image(img.as_raw(), w, h, image::ExtendedColorType::Rgba8)
        .unwrap();
    out
}

fn webp(w: u32, h: u32) -> Vec<u8> {
    let img = RgbaImage::from_fn(w, h, |x, y| {
        image::Rgba([(x % 256) as u8, 40, (y % 256) as u8, 255])
    });
    let mut out = Vec::new();
    image::codecs::webp::WebPEncoder::new_lossless(&mut out)
        .write_image(img.as_raw(), w, h, image::ExtendedColorType::Rgba8)
        .unwrap();
    out
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xEDB8_8320
            } else {
                c >> 1
            };
        }
    }
    !c
}

fn png_chunk(kind: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut v = (data.len() as u32).to_be_bytes().to_vec();
    let mut body = kind.to_vec();
    body.extend_from_slice(data);
    v.extend_from_slice(&body);
    v.extend_from_slice(&crc32(&body).to_be_bytes());
    v
}

/// Same PNG with the IHDR size replaced (a decompression bomb header: tiny file, huge claimed canvas).
fn png_claiming(src: &[u8], w: u32, h: u32) -> Vec<u8> {
    let mut v = src.to_vec();
    v[16..20].copy_from_slice(&w.to_be_bytes());
    v[20..24].copy_from_slice(&h.to_be_bytes());
    let crc = crc32(&v[12..29]);
    v[29..33].copy_from_slice(&crc.to_be_bytes());
    v
}

fn decoded(jpg: &[u8]) -> image::DynamicImage {
    image::load_from_memory_with_format(jpg, image::ImageFormat::Jpeg).unwrap()
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}

#[test]
fn accepts_jpeg_png_webp_and_makes_256_square_jpeg() {
    for (name, src) in [
        ("jpeg", jpeg(640, 480)),
        ("png", png(300, 900)),
        ("webp", webp(500, 200)),
        ("tiny", png(1, 1)),
    ] {
        let out = process(&src).unwrap_or_else(|e| panic!("{name}: {e:?}"));
        assert!(out.starts_with(&[0xFF, 0xD8, 0xFF]), "{name}");
        let img = decoded(&out);
        assert_eq!((img.width(), img.height()), (256, 256), "{name}");
    }
}

#[test]
fn centre_crop_keeps_the_middle() {
    // Left third red, middle blue, right third green: the crop of a wide image is the middle (blue).
    let img = RgbImage::from_fn(300, 100, |x, _| match x {
        0..=99 => image::Rgb([255, 0, 0]),
        100..=199 => image::Rgb([0, 0, 255]),
        _ => image::Rgb([0, 255, 0]),
    });
    let mut src = Vec::new();
    image::codecs::png::PngEncoder::new(&mut src)
        .write_image(img.as_raw(), 300, 100, image::ExtendedColorType::Rgb8)
        .unwrap();
    let out = decoded(&process(&src).unwrap()).to_rgb8();
    for (x, y) in [(10, 10), (128, 128), (245, 245)] {
        let p = out.get_pixel(x, y).0;
        assert!(p[2] > 200 && p[0] < 60 && p[1] < 60, "({x},{y}) = {p:?}");
    }
}

#[test]
fn transparent_png_goes_onto_white() {
    let img = RgbaImage::from_pixel(64, 64, image::Rgba([0, 0, 0, 0]));
    let mut src = Vec::new();
    image::codecs::png::PngEncoder::new(&mut src)
        .write_image(img.as_raw(), 64, 64, image::ExtendedColorType::Rgba8)
        .unwrap();
    let p = decoded(&process(&src).unwrap())
        .to_rgb8()
        .get_pixel(100, 100)
        .0;
    assert!(p.iter().all(|&c| c > 245), "{p:?}");
}

#[test]
fn exif_gps_and_other_metadata_are_stripped_from_the_bytes() {
    // JPEG with APP1 Exif (GPS marker text), APP2 ICC-ish, COM and XMP segments spliced in after SOI.
    let base = jpeg(200, 200);
    let seg = |marker: u8, payload: &[u8]| {
        let mut v = vec![0xFF, marker];
        v.extend_from_slice(&((payload.len() + 2) as u16).to_be_bytes());
        v.extend_from_slice(payload);
        v
    };
    let mut src = vec![0xFF, 0xD8];
    src.extend(seg(
        0xE1,
        b"Exif\0\0MM\0*GPSLatitude=51.5007N GPSLongitude=0.1246W SECRET-GPS",
    ));
    src.extend(seg(0xE2, b"ICC_PROFILE\0SECRET-ICC"));
    src.extend(seg(0xFE, b"SECRET-COMMENT <?php system($_GET[1]); ?>"));
    src.extend(seg(
        0xE1,
        b"http://ns.adobe.com/xap/1.0/\0<x:xmpmeta>SECRET-XMP</x:xmpmeta>",
    ));
    src.extend_from_slice(&base[2..]);
    assert!(contains(&src, b"SECRET-GPS"));
    let out = process(&src).unwrap();
    for marker in [
        &b"SECRET"[..],
        b"GPS",
        b"Exif",
        b"ICC_PROFILE",
        b"xmpmeta",
        b"<?php",
        b"adobe",
    ] {
        assert!(
            !contains(&out, marker),
            "{}",
            String::from_utf8_lossy(marker)
        );
    }
    // PNG text chunks (tEXt, iTXt, eXIf) inserted after IHDR.
    let p = png(100, 100);
    let mut src = p[..33].to_vec();
    src.extend(png_chunk(b"tEXt", b"Comment\0SECRET-PNGTEXT"));
    src.extend(png_chunk(b"eXIf", b"MM\0*GPSLatitude SECRET-PNGEXIF"));
    src.extend_from_slice(&p[33..]);
    let out = process(&src).unwrap();
    assert!(!contains(&out, b"SECRET") && !contains(&out, b"GPS"));
}

#[test]
fn exif_orientation_is_applied_before_cropping() {
    // 200x100 image: left half red, right half blue, tagged "rotate 90 CW" (orientation 6). After applying it the
    // image is 100x200 with red on top, blue below.
    let img = RgbImage::from_fn(200, 100, |x, _| {
        if x < 100 {
            image::Rgb([255, 0, 0])
        } else {
            image::Rgb([0, 0, 255])
        }
    });
    let mut plain = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut plain, 95)
        .encode_image(&img)
        .unwrap();
    let tiff: Vec<u8> = [
        &b"Exif\0\0MM\0\x2a\0\0\0\x08"[..],
        &[0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0],
    ]
    .concat();
    let mut src = vec![0xFF, 0xD8, 0xFF, 0xE1];
    src.extend_from_slice(&((tiff.len() + 2) as u16).to_be_bytes());
    src.extend_from_slice(&tiff);
    src.extend_from_slice(&plain[2..]);
    let out = decoded(&process(&src).unwrap()).to_rgb8();
    let top = out.get_pixel(128, 20).0;
    let bottom = out.get_pixel(128, 235).0;
    assert!(top[0] > 200 && top[2] < 60, "top {top:?}");
    assert!(bottom[2] > 200 && bottom[0] < 60, "bottom {bottom:?}");
}

#[test]
fn huge_dimensions_and_bombs_are_refused_before_decoding() {
    // Real 8193-wide PNG (small file): too big.
    assert!(matches!(
        process(&png(8193, 1)),
        Err(PhotoError::Dimensions)
    ));
    assert!(matches!(
        process(&png(1, 8193)),
        Err(PhotoError::Dimensions)
    ));
    // A 5 KB file that claims 60000x60000 (3.6 gigapixels): refused from the header, no allocation.
    let tiny = png(4, 4);
    let t = std::time::Instant::now();
    for (w, h) in [(60_000, 60_000), (8192, 8192), (u32::MAX >> 1, 1)] {
        let e = process(&png_claiming(&tiny, w, h));
        assert!(e.is_err(), "{w}x{h}");
    }
    assert!(t.elapsed().as_secs() < 5, "bombs must fail fast");
    // Zero dimension.
    assert!(process(&png_claiming(&tiny, 0, 4)).is_err());
}

#[test]
fn truncated_and_corrupt_images_are_decode_failures() {
    for (name, src) in [
        ("jpeg", jpeg(300, 300)),
        ("png", png(300, 300)),
        ("webp", webp(300, 300)),
    ] {
        for cut in [src.len() / 2, 40, 12] {
            let r = process(&src[..cut]);
            assert!(r.is_err(), "{name} truncated at {cut} decoded");
        }
        // Magic bytes intact, body replaced by noise.
        let mut noisy = src[..16].to_vec();
        noisy.extend((0..4000u32).map(|i| (i.wrapping_mul(2654435761) >> 13) as u8));
        assert!(process(&noisy).is_err(), "{name} noise");
    }
    // Just the magic.
    assert!(process(&[0xFF, 0xD8, 0xFF]).is_err());
    assert!(process(b"\x89PNG\r\n\x1a\n").is_err());
    assert!(process(b"RIFF\0\0\0\0WEBP").is_err());
}

#[test]
fn polyglots_are_rebuilt_from_pixels() {
    // Valid JPEG + appended script, zip and PHP: the appended part never reaches the output.
    let mut j = jpeg(120, 120);
    j.extend_from_slice(b"<script>alert(1)</script>PK\x03\x04<?php evil(); ?>GIF89a");
    let out = process(&j).unwrap();
    for m in [&b"<script"[..], b"PK\x03\x04", b"<?php", b"GIF89a"] {
        assert!(!contains(&out, m));
    }
    assert_eq!(decoded(&out).width(), 256);
    // PNG + trailing HTML.
    let mut p = png(50, 50);
    p.extend_from_slice(b"<html><script>x</script></html>");
    assert!(!contains(&process(&p).unwrap(), b"<html"));
    // The magic bytes decide, not what follows: GIF/HTML wearing no valid magic is unsupported.
    assert_eq!(
        process(b"GIF89a<script>alert(1)</script>").unwrap_err(),
        PhotoError::Unsupported
    );
    assert_eq!(
        process(b"<!DOCTYPE html><html>\xFF\xD8\xFF</html>").unwrap_err(),
        PhotoError::Unsupported
    );
    // RIFF that is not WebP (WAV/AVI).
    assert_eq!(
        process(b"RIFF\x24\0\0\0WAVEfmt ").unwrap_err(),
        PhotoError::Unsupported
    );
}

#[test]
fn svg_gif_heic_bmp_and_friends_are_rejected() {
    let cases: Vec<(&str, Vec<u8>)> = vec![
        (
            "svg",
            br#"<svg xmlns="http://www.w3.org/2000/svg" onload="alert(1)"><circle r="5"/></svg>"#
                .to_vec(),
        ),
        (
            "svg-xml",
            br#"<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg"/>"#.to_vec(),
        ),
        ("gif87", b"GIF87a\x01\0\x01\0\0\0\0;".to_vec()),
        ("gif89", b"GIF89a\x01\0\x01\0\0\0\0;".to_vec()),
        ("heic", b"\0\0\0\x18ftypheic\0\0\0\0mif1heic".to_vec()),
        ("avif", b"\0\0\0\x1cftypavif\0\0\0\0mif1avif".to_vec()),
        ("bmp", b"BM\x1e\0\0\0\0\0\0\0\x1a\0\0\0".to_vec()),
        ("tiff", b"II*\0\x08\0\0\0".to_vec()),
        ("pdf", b"%PDF-1.7\n".to_vec()),
        ("ico", b"\0\0\x01\0\x01\0".to_vec()),
        ("exe", b"MZ\x90\0".to_vec()),
        ("text", b"hello".to_vec()),
    ];
    for (name, bytes) in cases {
        assert_eq!(
            process(&bytes).unwrap_err(),
            PhotoError::Unsupported,
            "{name}"
        );
    }
}

#[test]
fn zero_and_oversized_inputs() {
    assert_eq!(process(&[]).unwrap_err(), PhotoError::Empty);
    let mut big = jpeg(100, 100);
    big.resize(MAX_BYTES + 1, 0);
    assert_eq!(process(&big).unwrap_err(), PhotoError::TooLarge);
    // Exactly at the limit still decodes (trailing bytes are ignored).
    let mut ok = jpeg(100, 100);
    ok.resize(MAX_BYTES, 0);
    assert!(process(&ok).is_ok());
}

// ---- over HTTP ----

fn multipart(name: &str, filename: &str, ctype: &str, data: &[u8]) -> (String, Vec<u8>) {
    let boundary = "----napple-test-boundary-7f3a";
    let mut body = format!(
        "--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; filename=\"{filename}\"\r\nContent-Type: {ctype}\r\n\r\n"
    )
    .into_bytes();
    body.extend_from_slice(data);
    body.extend_from_slice(format!("\r\n--{boundary}--\r\n").as_bytes());
    (format!("multipart/form-data; boundary={boundary}"), body)
}

async fn upload(
    t: &T,
    s: &Session,
    pid: &str,
    filename: &str,
    ctype: &str,
    data: &[u8],
) -> (StatusCode, Value) {
    let (ct, body) = multipart("photo", filename, ctype, data);
    let (st, _, b) = t
        .raw(
            Request::builder()
                .method("POST")
                .uri(format!("/account/v1/profiles/{pid}/photo"))
                .header("authorization", format!("Bearer {}", s.access))
                .header("content-type", ct)
                .body(Body::from(body))
                .unwrap(),
        )
        .await;
    (st, serde_json::from_slice(&b).unwrap_or(Value::Null))
}

async fn fetch(t: &T, s: &Session, pid: &str) -> (StatusCode, axum::http::HeaderMap, Vec<u8>) {
    t.raw(
        Request::builder()
            .uri(format!("/account/v1/profiles/{pid}/photo"))
            .header("authorization", format!("Bearer {}", s.access))
            .body(Body::empty())
            .unwrap(),
    )
    .await
}

#[tokio::test]
async fn upload_serve_replace_delete() {
    let t = T::new();
    let a = t.sign_in("pic@napple.test", "Mac").await;
    let pid = t.first_profile(&a).await;
    let (s, _, _) = fetch(&t, &a, &pid).await;
    assert_eq!(s, StatusCode::NOT_FOUND, "no photo yet");

    // Wrong extension and wrong content type do not matter: the bytes do.
    let (s, v) = upload(&t, &a, &pid, "me.gif", "text/plain", &png(400, 300)).await;
    assert_eq!(s, StatusCode::OK, "{v}");
    assert_eq!(
        v["photo"]["url"],
        format!("/account/v1/profiles/{pid}/photo")
    );
    let (s, h, body) = fetch(&t, &a, &pid).await;
    assert_eq!(s, StatusCode::OK);
    assert_eq!(h["content-type"], "image/jpeg");
    assert_eq!(h["x-content-type-options"], "nosniff");
    assert_eq!(h["cache-control"], "private, max-age=300");
    assert_eq!(decoded(&body).width(), 256);

    // Stored privately under the photo dir, original not kept anywhere.
    let dir = t.app.cfg.data_dir.join("photos");
    let files: Vec<_> = walk(&dir);
    assert_eq!(files.len(), 1, "{files:?}");
    assert_eq!(std::fs::read(&files[0]).unwrap(), body);

    // Replace: the old object is removed.
    let (s, _) = upload(&t, &a, &pid, "me.jpg", "image/jpeg", &jpeg(300, 300)).await;
    assert_eq!(s, StatusCode::OK);
    let files = walk(&dir);
    assert_eq!(files.len(), 1);
    assert_ne!(std::fs::read(&files[0]).unwrap(), body);

    // Delete.
    let (s, _) = t
        .req(
            "DELETE",
            &format!("/profiles/{pid}/photo"),
            Some(&a.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert!(walk(&dir).is_empty());
    let (s, _, _) = fetch(&t, &a, &pid).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (_, p) = t
        .req("GET", &format!("/profiles/{pid}"), Some(&a.access), None)
        .await;
    assert_eq!(p["photo"], Value::Null);
}

fn walk(d: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut v = vec![];
    if let Ok(rd) = std::fs::read_dir(d) {
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                v.extend(walk(&p));
            } else if p.extension().is_none_or(|x| x != "tmp") {
                v.push(p);
            }
        }
    }
    v
}

#[tokio::test]
async fn http_rejections() {
    let t = T::new();
    let a = t.sign_in("hostile@napple.test", "Mac").await;
    let pid = t.first_profile(&a).await;
    let mut big = jpeg(100, 100);
    big.resize(MAX_BYTES + 1, 0);
    let tiny = png(4, 4);
    let cases: Vec<(&str, Vec<u8>, StatusCode, &str)> = vec![
        (
            "empty",
            vec![],
            StatusCode::UNPROCESSABLE_ENTITY,
            "empty_photo",
        ),
        (
            "svg",
            b"<svg xmlns='http://www.w3.org/2000/svg'/>".to_vec(),
            StatusCode::UNPROCESSABLE_ENTITY,
            "unsupported_type",
        ),
        (
            "gif",
            b"GIF89a\x01\0\x01\0\0\0\0;".to_vec(),
            StatusCode::UNPROCESSABLE_ENTITY,
            "unsupported_type",
        ),
        (
            "heic",
            b"\0\0\0\x18ftypheic\0\0\0\0mif1heic".to_vec(),
            StatusCode::UNPROCESSABLE_ENTITY,
            "unsupported_type",
        ),
        (
            "truncated",
            jpeg(300, 300)[..500].to_vec(),
            StatusCode::UNPROCESSABLE_ENTITY,
            "decode_failed",
        ),
        (
            "bomb",
            png_claiming(&tiny, 60_000, 60_000),
            StatusCode::UNPROCESSABLE_ENTITY,
            "",
        ),
        (
            "too-wide",
            png(9000, 2),
            StatusCode::UNPROCESSABLE_ENTITY,
            "photo_dimensions",
        ),
        ("big", big, StatusCode::PAYLOAD_TOO_LARGE, "photo_too_large"),
    ];
    for (name, data, want, code) in cases {
        // Claim JPEG for everything: the declared type is irrelevant.
        let (s, v) = upload(&t, &a, &pid, "photo.jpg", "image/jpeg", &data).await;
        assert_eq!(s, want, "{name}: {v}");
        if !code.is_empty() {
            assert_eq!(v["error"], code, "{name}");
        }
    }
    // A huge body is cut off by the body limit, not buffered.
    let mut huge = jpeg(100, 100);
    huge.resize(MAX_BYTES * 3, 0);
    let (s, _) = upload(&t, &a, &pid, "photo.jpg", "image/jpeg", &huge).await;
    assert_eq!(s, StatusCode::PAYLOAD_TOO_LARGE);
    // No multipart at all / no parts.
    let (s, _) = t
        .req(
            "POST",
            &format!("/profiles/{pid}/photo"),
            Some(&a.access),
            Some(json!({"x": 1})),
        )
        .await;
    assert!(
        s == StatusCode::BAD_REQUEST || s == StatusCode::UNSUPPORTED_MEDIA_TYPE,
        "{s}"
    );
    // Nothing was stored by any failure, and the profile still has no photo.
    assert!(walk(&t.app.cfg.data_dir.join("photos")).is_empty());
    // Not yours / not signed in.
    let b = t.sign_in("stranger@napple.test", "Mac").await;
    let (s, _) = upload(&t, &b, &pid, "p.png", "image/png", &png(10, 10)).await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (s, _) = t
        .req(
            "DELETE",
            &format!("/profiles/{pid}/photo"),
            Some(&b.access),
            None,
        )
        .await;
    assert_eq!(s, StatusCode::NOT_FOUND);
    let (st, _, _) = t
        .raw(
            Request::builder()
                .method("POST")
                .uri(format!("/account/v1/profiles/{pid}/photo"))
                .body(Body::empty())
                .unwrap(),
        )
        .await;
    assert_eq!(st, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn tier_rules_decide_who_can_fetch_the_photo() {
    let t = T::new();
    let a = t.sign_in("owner@napple.test", "Mac").await;
    let b = t.sign_in("viewer@napple.test", "Mac").await;
    let pid = t.first_profile(&a).await;
    let (s, _) = upload(&t, &a, &pid, "p.png", "image/png", &png(64, 64)).await;
    assert_eq!(s, StatusCode::OK);
    // This endpoint has no friend data: tiers 1-2 are visible to any signed-in account, the rest only to the owner.
    for (tier, other_sees) in [(1, true), (2, true), (3, false), (4, false), (5, false)] {
        let (s, _) = t
            .req(
                "PATCH",
                &format!("/profiles/{pid}"),
                Some(&a.access),
                Some(json!({"tier": tier})),
            )
            .await;
        assert_eq!(s, StatusCode::OK);
        let (s, _, _) = fetch(&t, &b, &pid).await;
        assert_eq!(s == StatusCode::OK, other_sees, "tier {tier}");
        if !other_sees {
            assert_eq!(s, StatusCode::NOT_FOUND, "hidden looks like absent");
        }
        let (s, _, _) = fetch(&t, &a, &pid).await;
        assert_eq!(s, StatusCode::OK, "owner always, tier {tier}");
    }
}

#[tokio::test]
async fn deleting_profile_or_account_removes_the_photo() {
    let t = T::new();
    let a = t.sign_in("gone@napple.test", "Mac").await;
    let (_, p2) = t
        .req(
            "POST",
            "/profiles",
            Some(&a.access),
            Some(json!({"nickname": "Second"})),
        )
        .await;
    let p2 = p2["profile_id"].as_str().unwrap().to_string();
    let p1 = t.first_profile(&a).await;
    for p in [&p1, &p2] {
        let (s, _) = upload(&t, &a, p, "x.png", "image/png", &png(32, 32)).await;
        assert_eq!(s, StatusCode::OK);
    }
    let dir = t.app.cfg.data_dir.join("photos");
    assert_eq!(walk(&dir).len(), 2);
    let (s, _) = t
        .req("DELETE", &format!("/profiles/{p2}"), Some(&a.access), None)
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert_eq!(walk(&dir).len(), 1);
    let (s, _) = t
        .req(
            "DELETE",
            "/me",
            Some(&a.access),
            Some(json!({"confirm_email": "gone@napple.test"})),
        )
        .await;
    assert_eq!(s, StatusCode::NO_CONTENT);
    assert!(walk(&dir).is_empty());
}

#[tokio::test]
async fn s3_store_signs_private_requests() {
    use napple_account::aws::{CredentialProvider, Credentials};
    use napple_account::config::S3Config;
    use napple_account::outbound::{MockOutbound, OutResponse};
    use napple_account::photos::{PhotoStore, S3PhotoStore};
    use std::sync::Arc;
    let store_bytes = std::sync::Mutex::new(Vec::<u8>::new());
    let store_bytes = Arc::new(store_bytes);
    let sb = store_bytes.clone();
    let out = Arc::new(MockOutbound::new(move |r| {
        let ok = |status, body| OutResponse {
            status,
            headers: vec![],
            body,
        };
        match r.method.as_str() {
            "PUT" => {
                *sb.lock().unwrap() = r.body.clone();
                ok(200, vec![])
            }
            "GET" => {
                let b = sb.lock().unwrap().clone();
                if b.is_empty() {
                    ok(404, vec![])
                } else {
                    ok(200, b)
                }
            }
            _ => {
                sb.lock().unwrap().clear();
                ok(204, vec![])
            }
        }
    }));
    let creds = Arc::new(CredentialProvider::fixed(
        Credentials {
            access_key: "AKIDNAPPLETESTONLY".into(),
            secret_key: "napple-test-secret-not-a-real-key".into(),
            session_token: None,
            expires_at: None,
        },
        out.clone(),
    ));
    let s3 = S3PhotoStore::new(
        S3Config {
            bucket: "napple-photos-test".into(),
            region: "us-east-1".into(),
            endpoint: None,
        },
        creds,
        out.clone(),
    );
    assert_eq!(s3.get("photos/a/b/c").await.unwrap(), None);
    s3.put("photos/a/b/c", vec![1, 2, 3]).await.unwrap();
    assert_eq!(s3.get("photos/a/b/c").await.unwrap(), Some(vec![1, 2, 3]));
    s3.delete("photos/a/b/c").await.unwrap();
    assert_eq!(s3.get("photos/a/b/c").await.unwrap(), None);
    assert!(s3.get("../etc/passwd").await.is_err(), "keys are validated");
    let reqs = out.requests();
    let put = reqs.iter().find(|r| r.method == "PUT").unwrap();
    assert_eq!(
        put.url,
        "https://napple-photos-test.s3.us-east-1.amazonaws.com/photos/a/b/c"
    );
    let hdr = |n: &str| {
        put.headers
            .iter()
            .find(|(k, _)| k == n)
            .map(|(_, v)| v.clone())
    };
    assert!(hdr("authorization").unwrap().contains("AWS4-HMAC-SHA256"));
    assert!(hdr("authorization").unwrap().contains("content-type"));
    assert_eq!(hdr("content-type").as_deref(), Some("image/jpeg"));
    assert_eq!(
        hdr("x-amz-content-sha256").unwrap(),
        napple_account::util::sha256_hex(&[1, 2, 3])
    );
    // Never presigned: no query string on any request.
    assert!(reqs.iter().all(|r| !r.url.contains('?')));
}
