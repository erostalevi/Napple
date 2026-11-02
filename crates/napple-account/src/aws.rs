//! AWS without the SDK: Signature Version 4 (header-signed requests and presigned URLs) and credentials from the
//! environment or the EC2 instance role (IMDSv2). Keeps the binary small and free of C/OpenSSL dependencies.

use crate::outbound::{OutRequest, Outbound};
use crate::util::{amz_date, hmac_sha256, sha256_hex};
use std::sync::{Arc, Mutex};

pub const EMPTY_SHA256: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

#[derive(Clone)]
pub struct Credentials {
    pub access_key: String,
    pub secret_key: String,
    pub session_token: Option<String>,
    pub expires_at: Option<i64>,
}

impl std::fmt::Debug for Credentials {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Credentials { .. }")
    }
}

/// RFC 3986 encoding as SigV4 wants it (`/` kept when `keep_slash`).
pub fn uri_encode(s: &str, keep_slash: bool) -> String {
    let mut o = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                o.push(b as char)
            }
            b'/' if keep_slash => o.push('/'),
            _ => o.push_str(&format!("%{b:02X}")),
        }
    }
    o
}

fn canonical_query(params: &[(String, String)]) -> String {
    let mut enc: Vec<(String, String)> = params
        .iter()
        .map(|(k, v)| (uri_encode(k, false), uri_encode(v, false)))
        .collect();
    enc.sort();
    enc.iter()
        .map(|(k, v)| format!("{k}={v}"))
        .collect::<Vec<_>>()
        .join("&")
}

fn signing_key(secret: &str, date: &str, region: &str, service: &str) -> [u8; 32] {
    let k = hmac_sha256(format!("AWS4{secret}").as_bytes(), date.as_bytes());
    let k = hmac_sha256(&k, region.as_bytes());
    let k = hmac_sha256(&k, service.as_bytes());
    hmac_sha256(&k, b"aws4_request")
}

pub struct SignInput<'a> {
    pub method: &'a str,
    /// Already-encoded absolute path, e.g. `/bucket/key%20name`.
    pub path: &'a str,
    pub query: Vec<(String, String)>,
    /// Headers to sign (names any case); `host` must be included.
    pub headers: Vec<(String, String)>,
    pub payload_hash: &'a str,
    pub region: &'a str,
    pub service: &'a str,
    pub now: i64,
}

/// Returns (signature hex, signed header list, credential scope).
fn signature(creds: &Credentials, i: &SignInput) -> (String, String, String) {
    let date_time = amz_date(i.now);
    let date = &date_time[..8];
    let mut hs: Vec<(String, String)> = i
        .headers
        .iter()
        .map(|(k, v)| {
            (
                k.to_ascii_lowercase(),
                v.split_whitespace().collect::<Vec<_>>().join(" "),
            )
        })
        .collect();
    hs.sort();
    let canonical_headers: String = hs.iter().map(|(k, v)| format!("{k}:{v}\n")).collect();
    let signed_headers = hs
        .iter()
        .map(|(k, _)| k.as_str())
        .collect::<Vec<_>>()
        .join(";");
    let creq = format!(
        "{}\n{}\n{}\n{}\n{}\n{}",
        i.method,
        i.path,
        canonical_query(&i.query),
        canonical_headers,
        signed_headers,
        i.payload_hash
    );
    let scope = format!("{date}/{}/{}/aws4_request", i.region, i.service);
    let sts = format!(
        "AWS4-HMAC-SHA256\n{date_time}\n{scope}\n{}",
        sha256_hex(creq.as_bytes())
    );
    let sig = hex::encode(hmac_sha256(
        &signing_key(&creds.secret_key, date, i.region, i.service),
        sts.as_bytes(),
    ));
    (sig, signed_headers, scope)
}

/// Query string (without `?`) of a presigned URL. `i.headers` are the headers the client must send verbatim.
pub fn presign_query(creds: &Credentials, mut i: SignInput, expires: i64) -> String {
    let signed_names = {
        let mut n: Vec<String> = i
            .headers
            .iter()
            .map(|(k, _)| k.to_ascii_lowercase())
            .collect();
        n.sort();
        n.join(";")
    };
    let date_time = amz_date(i.now);
    let scope = format!(
        "{}/{}/{}/aws4_request",
        &date_time[..8],
        i.region,
        i.service
    );
    i.query
        .push(("X-Amz-Algorithm".into(), "AWS4-HMAC-SHA256".into()));
    i.query.push((
        "X-Amz-Credential".into(),
        format!("{}/{scope}", creds.access_key),
    ));
    i.query.push(("X-Amz-Date".into(), date_time));
    i.query.push(("X-Amz-Expires".into(), expires.to_string()));
    if let Some(t) = &creds.session_token {
        i.query.push(("X-Amz-Security-Token".into(), t.clone()));
    }
    i.query.push(("X-Amz-SignedHeaders".into(), signed_names));
    let (sig, ..) = signature(creds, &i);
    format!("{}&X-Amz-Signature={sig}", canonical_query(&i.query))
}

/// Header-signed request: returns the headers to add (`x-amz-date`, `x-amz-content-sha256`, token, `authorization`).
pub fn sign_headers(creds: &Credentials, mut i: SignInput) -> Vec<(String, String)> {
    let mut extra = vec![
        ("x-amz-date".to_string(), amz_date(i.now)),
        (
            "x-amz-content-sha256".to_string(),
            i.payload_hash.to_string(),
        ),
    ];
    if let Some(t) = &creds.session_token {
        extra.push(("x-amz-security-token".into(), t.clone()));
    }
    i.headers.extend(extra.iter().cloned());
    let (sig, signed, scope) = signature(creds, &i);
    extra.push((
        "authorization".into(),
        format!(
            "AWS4-HMAC-SHA256 Credential={}/{scope}, SignedHeaders={signed}, Signature={sig}",
            creds.access_key
        ),
    ));
    extra
}

/// Recomputes a presigned URL's signature (used by the S3 mock in the tests and by the dev blob store's self-check).
#[allow(clippy::too_many_arguments)]
pub fn verify_presigned(
    creds: &Credentials,
    method: &str,
    path: &str,
    query: &[(String, String)],
    headers: &[(String, String)],
    region: &str,
    service: &str,
    now: i64,
) -> bool {
    let get = |k: &str| query.iter().find(|(qk, _)| qk == k).map(|(_, v)| v.clone());
    let (Some(sig), Some(date), Some(expires), Some(signed)) = (
        get("X-Amz-Signature"),
        get("X-Amz-Date"),
        get("X-Amz-Expires"),
        get("X-Amz-SignedHeaders"),
    ) else {
        return false;
    };
    let Some(t) = parse_amz_date(&date) else {
        return false;
    };
    let Ok(exp) = expires.parse::<i64>() else {
        return false;
    };
    if now < t - 300 || now > t + exp {
        return false;
    }
    let rest: Vec<(String, String)> = query
        .iter()
        .filter(|(k, _)| k != "X-Amz-Signature")
        .cloned()
        .collect();
    let names: Vec<&str> = signed.split(';').collect();
    let hs: Vec<(String, String)> = names
        .iter()
        .map(|n| {
            (
                n.to_string(),
                headers
                    .iter()
                    .find(|(k, _)| k.eq_ignore_ascii_case(n))
                    .map(|(_, v)| v.clone())
                    .unwrap_or_default(),
            )
        })
        .collect();
    let (expect, ..) = signature(
        creds,
        &SignInput {
            method,
            path,
            query: rest,
            headers: hs,
            payload_hash: "UNSIGNED-PAYLOAD",
            region,
            service,
            now: t,
        },
    );
    crate::util::ct_eq(expect.as_bytes(), sig.as_bytes())
}

/// `20130524T000000Z` or ISO 8601 `2013-05-24T00:00:00Z` -> unix seconds.
pub fn parse_amz_date(s: &str) -> Option<i64> {
    let d: String = s.chars().filter(|c| c.is_ascii_digit()).collect();
    if d.len() < 14 {
        return None;
    }
    let n = |a: usize, b: usize| d[a..b].parse::<i64>().ok();
    let (y, m, day, h, mi, sec) = (
        n(0, 4)?,
        n(4, 6)?,
        n(6, 8)?,
        n(8, 10)?,
        n(10, 12)?,
        n(12, 14)?,
    );
    let y2 = if m <= 2 { y - 1 } else { y };
    let era = y2.div_euclid(400);
    let yoe = y2 - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    Some(days * 86_400 + h * 3600 + mi * 60 + sec)
}

/// Environment credentials, else the EC2 instance role via IMDSv2 (cached until 5 minutes before expiry).
/// In Docker on EC2 the instance needs `HttpPutResponseHopLimit = 2` for IMDSv2 to answer.
pub struct CredentialProvider {
    fixed: Option<Credentials>,
    cache: Mutex<Option<Credentials>>,
    out: Arc<dyn Outbound>,
    imds: String,
}

impl CredentialProvider {
    pub fn from_env(out: Arc<dyn Outbound>) -> CredentialProvider {
        let get = |k: &str| std::env::var(k).ok().filter(|s| !s.is_empty());
        let fixed = match (get("AWS_ACCESS_KEY_ID"), get("AWS_SECRET_ACCESS_KEY")) {
            (Some(a), Some(s)) => Some(Credentials {
                access_key: a,
                secret_key: s,
                session_token: get("AWS_SESSION_TOKEN"),
                expires_at: None,
            }),
            _ => None,
        };
        CredentialProvider {
            fixed,
            cache: Mutex::new(None),
            out,
            imds: get("NAPPLE_ACCOUNT_IMDS").unwrap_or_else(|| "http://169.254.169.254".into()),
        }
    }

    pub fn fixed(c: Credentials, out: Arc<dyn Outbound>) -> CredentialProvider {
        CredentialProvider {
            fixed: Some(c),
            cache: Mutex::new(None),
            out,
            imds: String::new(),
        }
    }

    pub async fn get(&self, now: i64) -> Result<Credentials, String> {
        if let Some(c) = &self.fixed {
            return Ok(c.clone());
        }
        if let Some(c) = self.cache.lock().unwrap().as_ref()
            && c.expires_at.is_none_or(|e| e - 300 > now)
        {
            return Ok(c.clone());
        }
        let c = self.fetch_imds().await?;
        *self.cache.lock().unwrap() = Some(c.clone());
        Ok(c)
    }

    async fn fetch_imds(&self) -> Result<Credentials, String> {
        let tok = self
            .out
            .send(OutRequest {
                method: "PUT".into(),
                url: format!("{}/latest/api/token", self.imds),
                headers: vec![(
                    "x-aws-ec2-metadata-token-ttl-seconds".into(),
                    "21600".into(),
                )],
                body: vec![],
            })
            .await?;
        if tok.status != 200 {
            return Err(format!("IMDS token: HTTP {}", tok.status));
        }
        let token = String::from_utf8_lossy(&tok.body).trim().to_string();
        let get = |path: String| {
            let token = token.clone();
            async move {
                let r = self
                    .out
                    .send(OutRequest {
                        method: "GET".into(),
                        url: format!("{}{path}", self.imds),
                        headers: vec![("x-aws-ec2-metadata-token".into(), token)],
                        body: vec![],
                    })
                    .await?;
                if r.status != 200 {
                    return Err(format!("IMDS {path}: HTTP {}", r.status));
                }
                Ok::<_, String>(r.body)
            }
        };
        let role_list = get("/latest/meta-data/iam/security-credentials/".into()).await?;
        let role = String::from_utf8_lossy(&role_list)
            .lines()
            .next()
            .unwrap_or("")
            .trim()
            .to_string();
        if role.is_empty() {
            return Err("IMDS: no instance role".into());
        }
        let body = get(format!("/latest/meta-data/iam/security-credentials/{role}")).await?;
        let v: serde_json::Value = serde_json::from_slice(&body).map_err(|e| e.to_string())?;
        let s = |k: &str| {
            v[k].as_str()
                .map(str::to_string)
                .ok_or_else(|| format!("IMDS credentials: missing {k}"))
        };
        Ok(Credentials {
            access_key: s("AccessKeyId")?,
            secret_key: s("SecretAccessKey")?,
            session_token: Some(s("Token")?),
            expires_at: parse_amz_date(&s("Expiration")?),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The example credentials and vectors published in the AWS S3 SigV4 documentation (not real credentials).
    fn doc_creds() -> Credentials {
        Credentials {
            access_key: "AKIAIOSFODNN7EXAMPLE".into(),
            secret_key: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY".into(),
            session_token: None,
            expires_at: None,
        }
    }

    #[test]
    fn aws_doc_presigned_get_vector() {
        let now = parse_amz_date("20130524T000000Z").unwrap();
        let q = presign_query(
            &doc_creds(),
            SignInput {
                method: "GET",
                path: "/test.txt",
                query: vec![],
                headers: vec![("host".into(), "examplebucket.s3.amazonaws.com".into())],
                payload_hash: "UNSIGNED-PAYLOAD",
                region: "us-east-1",
                service: "s3",
                now,
            },
            86_400,
        );
        assert!(
            q.ends_with(
                "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"
            ),
            "{q}"
        );
        assert!(q.contains(
            "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
        ));
    }

    #[test]
    fn aws_doc_header_signed_get_vector() {
        let now = parse_amz_date("20130524T000000Z").unwrap();
        let h = sign_headers(
            &doc_creds(),
            SignInput {
                method: "GET",
                path: "/test.txt",
                query: vec![],
                headers: vec![
                    ("Host".into(), "examplebucket.s3.amazonaws.com".into()),
                    ("Range".into(), "bytes=0-9".into()),
                ],
                payload_hash: EMPTY_SHA256,
                region: "us-east-1",
                service: "s3",
                now,
            },
        );
        let auth = &h.iter().find(|(k, _)| k == "authorization").unwrap().1;
        assert!(
            auth.ends_with(
                "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"
            ),
            "{auth}"
        );
        assert!(auth.contains("SignedHeaders=host;range;x-amz-content-sha256;x-amz-date"));
    }

    #[test]
    fn dates_round_trip() {
        for ts in [0i64, 1_369_353_600, 1_793_577_600, 951_782_400] {
            assert_eq!(parse_amz_date(&amz_date(ts)), Some(ts));
        }
        assert_eq!(parse_amz_date("2013-05-24T00:00:00Z"), Some(1_369_353_600));
    }

    #[test]
    fn encoding() {
        assert_eq!(uri_encode("a b/c~d", true), "a%20b/c~d");
        assert_eq!(uri_encode("a/b", false), "a%2Fb");
    }
}
