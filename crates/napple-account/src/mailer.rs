//! Outgoing email: Amazon SES v2 and SMTP in production, a stdout printer for local dev, a capture buffer for tests.
//! Messages are multipart (text + HTML). A provider that reports the address as permanently undeliverable (SES
//! suppression list, SMTP 5xx recipient rejection) returns `MailError::Undeliverable`; the caller records it and
//! never retries that address.

use crate::aws::{CredentialProvider, SignInput, sign_headers};
use crate::outbound::{BoxFut, OutRequest, Outbound};
use crate::util::{b64std, sha256_hex};
use serde_json::json;
use std::collections::HashSet;
use std::sync::{Arc, Mutex};

#[derive(Clone, Debug)]
pub struct Mail {
    pub to: String,
    pub subject: String,
    pub text: String,
    pub html: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MailError {
    /// Permanent: the address is suppressed, bounced or refused. Do not retry.
    Undeliverable(String),
    /// Temporary or configuration problem.
    Failed(String),
}

impl std::fmt::Display for MailError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            MailError::Undeliverable(s) => write!(f, "undeliverable: {s}"),
            MailError::Failed(s) => write!(f, "{s}"),
        }
    }
}

pub trait Mailer: Send + Sync {
    fn send(&self, mail: Mail, now: i64) -> BoxFut<'_, Result<(), MailError>>;
}

// ---- templates ----------------------------------------------------------------------------------------------

const HTML_TEMPLATE: &str = include_str!("../templates/code_email.html");
const TEXT_TEMPLATE: &str = include_str!("../templates/code_email.txt");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CodeKind {
    Confirm,
    Reset,
}

pub fn html_escape(s: &str) -> String {
    let mut o = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => o.push_str("&amp;"),
            '<' => o.push_str("&lt;"),
            '>' => o.push_str("&gt;"),
            '"' => o.push_str("&quot;"),
            '\'' => o.push_str("&#39;"),
            _ => o.push(c),
        }
    }
    o
}

fn fill(template: &str, vars: &[(&str, &str)], escape: bool) -> String {
    let mut out = template.to_string();
    for (k, v) in vars {
        let v = if escape { html_escape(v) } else { v.to_string() };
        out = out.replace(&format!("{{{{{k}}}}}"), &v);
    }
    out
}

/// The confirmation / reset code email: (subject, text, html). `code` is the 6 digits.
pub fn render_code_mail(kind: CodeKind, code: &str, minutes: i64) -> (String, String, String) {
    let (subject, preheader, headline, intro, ignore) = match kind {
        CodeKind::Confirm => (
            "Your Napple confirmation code",
            "Use this code to finish creating your Napple account.",
            "Confirm your email",
            "Enter this code in Napple to finish creating your account.",
            "If you didn't try to create a Napple account, you can ignore this email; nothing will happen.",
        ),
        CodeKind::Reset => (
            "Your Napple password reset code",
            "Use this code to choose a new Napple password.",
            "Reset your password",
            "Enter this code in Napple to choose a new password.",
            "If you didn't ask to reset your password, you can ignore this email; your password stays as it is.",
        ),
    };
    let minutes = minutes.to_string();
    let vars = [
        ("subject", subject),
        ("preheader", preheader),
        ("headline", headline),
        ("intro", intro),
        ("code", code),
        ("minutes", minutes.as_str()),
        ("ignore", ignore),
    ];
    // The developer comment at the top of the HTML template documents the placeholders; it is not sent.
    let html_src = match (HTML_TEMPLATE.find("<!--"), HTML_TEMPLATE.find("-->")) {
        (Some(a), Some(b)) if a < b => format!("{}{}", &HTML_TEMPLATE[..a], &HTML_TEMPLATE[b + 3..]),
        _ => HTML_TEMPLATE.to_string(),
    };
    (
        subject.to_string(),
        fill(TEXT_TEMPLATE, &vars, false),
        fill(&html_src, &vars, true),
    )
}

// ---- providers ----------------------------------------------------------------------------------------------

/// Prints the whole message (including codes) to stdout. Refused by config in production.
pub struct DevMailer;

impl Mailer for DevMailer {
    fn send(&self, mail: Mail, _now: i64) -> BoxFut<'_, Result<(), MailError>> {
        println!(
            "---- dev mailer (not sent) ----\nTo: {}\nSubject: {}\n\n{}\n-------------------------------",
            mail.to, mail.subject, mail.text
        );
        Box::pin(async { Ok(()) })
    }
}

#[derive(Default)]
pub struct CaptureMailer {
    pub sent: Mutex<Vec<Mail>>,
    /// Addresses the fake provider reports as permanently undeliverable.
    pub suppressed: Mutex<HashSet<String>>,
}

impl CaptureMailer {
    pub fn last_to(&self, to: &str) -> Option<Mail> {
        self.sent
            .lock()
            .unwrap()
            .iter()
            .rev()
            .find(|m| m.to == to)
            .cloned()
    }
    pub fn count(&self) -> usize {
        self.sent.lock().unwrap().len()
    }
    pub fn suppress(&self, to: &str) {
        self.suppressed.lock().unwrap().insert(to.to_string());
    }
}

impl Mailer for CaptureMailer {
    fn send(&self, mail: Mail, _now: i64) -> BoxFut<'_, Result<(), MailError>> {
        let r = if self.suppressed.lock().unwrap().contains(&mail.to) {
            Err(MailError::Undeliverable("suppressed".into()))
        } else {
            self.sent.lock().unwrap().push(mail);
            Ok(())
        };
        Box::pin(async move { r })
    }
}

pub struct SesMailer {
    pub region: String,
    pub from: String,
    pub config_set: String,
    pub creds: Arc<CredentialProvider>,
    pub out: Arc<dyn Outbound>,
}

/// Maps an SES v2 error response to Undeliverable (suppression list, bounced address) or Failed.
pub fn ses_error(status: u16, error_type: Option<&str>, body: &[u8]) -> MailError {
    let text = String::from_utf8_lossy(body).to_ascii_lowercase();
    let ty = error_type.unwrap_or("").to_ascii_lowercase();
    let suppressed = text.contains("suppression list")
        || text.contains("suppressed")
        || text.contains("bounced")
        || text.contains("complaint")
        || text.contains("invalid email address")
        || text.contains("not a valid email");
    if status == 400 && ty.contains("messagerejected") && suppressed
        || status == 400 && ty.contains("badrequest") && text.contains("invalid")
    {
        MailError::Undeliverable(format!("SES rejected the address (HTTP {status})"))
    } else {
        MailError::Failed(format!("SES SendEmail: HTTP {status} {ty}"))
    }
}

impl Mailer for SesMailer {
    fn send(&self, mail: Mail, now: i64) -> BoxFut<'_, Result<(), MailError>> {
        Box::pin(async move {
            let creds = self.creds.get(now).await.map_err(MailError::Failed)?;
            let host = format!("email.{}.amazonaws.com", self.region);
            let path = "/v2/email/outbound-emails";
            let mut body = json!({
                "FromEmailAddress": self.from,
                "Destination": {"ToAddresses": [mail.to]},
                "Content": {"Simple": {"Subject": {"Data": mail.subject, "Charset": "UTF-8"}, "Body": {"Text": {"Data": mail.text, "Charset": "UTF-8"}}}},
                "ConfigurationSetName": self.config_set,
            });
            if let Some(h) = &mail.html {
                body["Content"]["Simple"]["Body"]["Html"] = json!({"Data": h, "Charset": "UTF-8"});
            }
            let body = body.to_string().into_bytes();
            let hash = sha256_hex(&body);
            let mut headers = sign_headers(
                &creds,
                SignInput {
                    method: "POST",
                    path,
                    query: vec![],
                    headers: vec![
                        ("host".into(), host.clone()),
                        ("content-type".into(), "application/json".into()),
                    ],
                    payload_hash: &hash,
                    region: &self.region,
                    service: "ses",
                    now,
                },
            );
            headers.push(("content-type".into(), "application/json".into()));
            let r = self
                .out
                .send(OutRequest {
                    method: "POST".into(),
                    url: format!("https://{host}{path}"),
                    headers,
                    body,
                })
                .await
                .map_err(MailError::Failed)?;
            if r.status / 100 != 2 {
                return Err(ses_error(r.status, r.header("x-amzn-errortype"), &r.body));
            }
            Ok(())
        })
    }
}

// ---- SMTP ---------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SmtpUrl {
    pub tls: SmtpTls,
    pub host: String,
    pub port: u16,
    pub user: Option<(String, String)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SmtpTls {
    None,
    StartTls,
    Implicit,
}

pub fn parse_smtp_url(url: &str) -> Result<SmtpUrl, String> {
    let (scheme, rest) = url.split_once("://").ok_or("NAPPLE_SMTP_URL: scheme://host")?;
    let (tls, default_port) = match scheme {
        "smtp" => (SmtpTls::None, 25),
        "smtp+starttls" | "submission" => (SmtpTls::StartTls, 587),
        "smtps" => (SmtpTls::Implicit, 465),
        _ => return Err(format!("NAPPLE_SMTP_URL: unknown scheme {scheme}")),
    };
    let rest = rest.trim_end_matches('/');
    let (auth, hostport) = match rest.rsplit_once('@') {
        Some((a, h)) => (Some(a), h),
        None => (None, rest),
    };
    let dec = |s: &str| percent_encoding::percent_decode_str(s).decode_utf8_lossy().to_string();
    let user = auth.map(|a| match a.split_once(':') {
        Some((u, p)) => (dec(u), dec(p)),
        None => (dec(a), String::new()),
    });
    let (host, port) = match hostport.rsplit_once(':') {
        Some((h, p)) => (h.to_string(), p.parse::<u16>().map_err(|_| "NAPPLE_SMTP_URL: bad port")?),
        None => (hostport.to_string(), default_port),
    };
    if host.is_empty() {
        return Err("NAPPLE_SMTP_URL: empty host".into());
    }
    Ok(SmtpUrl { tls, host, port, user })
}

/// RFC 5322 multipart/alternative message, bodies base64 (no dot-stuffing or line-length issues).
pub fn build_message(from: &str, mail: &Mail, boundary: &str, date: &str) -> String {
    let wrap = |s: &str| {
        b64std(s.as_bytes())
            .as_bytes()
            .chunks(76)
            .map(|c| String::from_utf8_lossy(c).into_owned())
            .collect::<Vec<_>>()
            .join("\r\n")
    };
    let subject = if mail.subject.is_ascii() {
        mail.subject.replace(['\r', '\n'], " ")
    } else {
        format!("=?UTF-8?B?{}?=", b64std(mail.subject.as_bytes()))
    };
    let mut m = format!(
        "From: Napple <{from}>\r\nTo: <{}>\r\nSubject: {subject}\r\nDate: {date}\r\nMIME-Version: 1.0\r\nAuto-Submitted: auto-generated\r\n",
        mail.to
    );
    match &mail.html {
        Some(h) => {
            m += &format!("Content-Type: multipart/alternative; boundary=\"{boundary}\"\r\n\r\n");
            m += &format!("--{boundary}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n{}\r\n", wrap(&mail.text));
            m += &format!("--{boundary}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n{}\r\n", wrap(h));
            m += &format!("--{boundary}--\r\n");
        }
        None => {
            m += &format!("Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n{}\r\n", wrap(&mail.text));
        }
    }
    m
}

pub struct SmtpMailer {
    pub cfg: SmtpUrl,
    pub from: String,
}

trait Io: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send {}
impl<T: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin + Send> Io for T {}

struct Conn {
    io: tokio::io::BufReader<Box<dyn Io>>,
}

impl Conn {
    /// Reads one (possibly multi-line) reply: (code, text).
    async fn reply(&mut self) -> Result<(u16, String), MailError> {
        use tokio::io::AsyncBufReadExt;
        let mut text = String::new();
        loop {
            let mut line = String::new();
            let n = self.io.read_line(&mut line).await.map_err(|e| MailError::Failed(format!("smtp read: {e}")))?;
            if n == 0 {
                return Err(MailError::Failed("smtp: connection closed".into()));
            }
            let code: u16 = line.get(..3).and_then(|c| c.parse().ok()).ok_or_else(|| MailError::Failed(format!("smtp: bad reply {line:?}")))?;
            text.push_str(line[3..].trim_start_matches(['-', ' ']).trim_end());
            text.push('\n');
            if line.as_bytes().get(3) != Some(&b'-') {
                return Ok((code, text));
            }
        }
    }
    async fn cmd(&mut self, line: &str, ok: u16) -> Result<String, MailError> {
        use tokio::io::AsyncWriteExt;
        self.io
            .write_all(format!("{line}\r\n").as_bytes())
            .await
            .map_err(|e| MailError::Failed(format!("smtp write: {e}")))?;
        self.expect(ok, line).await
    }
    async fn expect(&mut self, ok: u16, what: &str) -> Result<String, MailError> {
        let (code, text) = self.reply().await?;
        if code == ok || (ok == 250 && code == 251) {
            Ok(text)
        } else if (500..600).contains(&code) && (what.starts_with("RCPT") || code == 550 || code == 553) {
            Err(MailError::Undeliverable(format!("smtp {code}")))
        } else {
            Err(MailError::Failed(format!("smtp {code} after {}", what.split_whitespace().next().unwrap_or("?"))))
        }
    }
}

fn tls_connector() -> tokio_rustls::TlsConnector {
    let mut roots = tokio_rustls::rustls::RootCertStore::empty();
    roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    let cfg = tokio_rustls::rustls::ClientConfig::builder_with_provider(Arc::new(tokio_rustls::rustls::crypto::ring::default_provider()))
        .with_safe_default_protocol_versions()
        .expect("tls versions")
        .with_root_certificates(roots)
        .with_no_client_auth();
    tokio_rustls::TlsConnector::from(Arc::new(cfg))
}

impl SmtpMailer {
    async fn deliver(&self, mail: &Mail, now: i64) -> Result<(), MailError> {
        use tokio::io::AsyncWriteExt;
        let c = &self.cfg;
        let tcp = tokio::time::timeout(
            std::time::Duration::from_secs(15),
            tokio::net::TcpStream::connect((c.host.as_str(), c.port)),
        )
        .await
        .map_err(|_| MailError::Failed("smtp connect timeout".into()))?
        .map_err(|e| MailError::Failed(format!("smtp connect: {e}")))?;
        let name = rustls_name(&c.host)?;
        let io: Box<dyn Io> = if c.tls == SmtpTls::Implicit {
            Box::new(tls_connector().connect(name.clone(), tcp).await.map_err(|e| MailError::Failed(format!("smtp tls: {e}")))?)
        } else {
            Box::new(tcp)
        };
        let mut s = Conn { io: tokio::io::BufReader::new(io) };
        s.expect(220, "greeting").await?;
        let helo = "EHLO napple.lat";
        s.cmd(helo, 250).await?;
        if c.tls == SmtpTls::StartTls {
            s.cmd("STARTTLS", 220).await?;
            let tcp = s.io.into_inner();
            // Re-wrap: the plain stream is moved into the TLS handshake.
            let tls = tls_connector().connect(name, tcp).await.map_err(|e| MailError::Failed(format!("smtp starttls: {e}")))?;
            s = Conn { io: tokio::io::BufReader::new(Box::new(tls)) };
            s.cmd(helo, 250).await?;
        }
        if let Some((u, p)) = &c.user {
            let plain = b64std(format!("\0{u}\0{p}").as_bytes());
            s.cmd(&format!("AUTH PLAIN {plain}"), 235).await?;
        }
        s.cmd(&format!("MAIL FROM:<{}>", self.from), 250).await?;
        s.cmd(&format!("RCPT TO:<{}>", mail.to), 250).await?;
        s.cmd("DATA", 354).await?;
        let boundary = format!("napple-{}", crate::util::random_token());
        let msg = build_message(&self.from, mail, &boundary, &crate::util::ymd(now));
        s.io.write_all(format!("{msg}\r\n.\r\n").as_bytes()).await.map_err(|e| MailError::Failed(format!("smtp write: {e}")))?;
        s.expect(250, "DATA body").await?;
        let _ = s.cmd("QUIT", 221).await;
        Ok(())
    }
}

fn rustls_name(host: &str) -> Result<tokio_rustls::rustls::pki_types::ServerName<'static>, MailError> {
    tokio_rustls::rustls::pki_types::ServerName::try_from(host.to_string()).map_err(|_| MailError::Failed("smtp: bad host name".into()))
}

impl Mailer for SmtpMailer {
    fn send(&self, mail: Mail, now: i64) -> BoxFut<'_, Result<(), MailError>> {
        Box::pin(async move { self.deliver(&mail, now).await })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn code_mail_renders_and_escapes() {
        let (subj, text, html) = render_code_mail(CodeKind::Confirm, "123456", 10);
        assert_eq!(subj, "Your Napple confirmation code");
        assert!(text.contains("123456") && text.contains("10 minutes") && !text.contains("{{"));
        assert!(html.contains("123456") && !html.contains("{{") && !html.contains("Placeholders"));
        let (_, rt, rh) = render_code_mail(CodeKind::Reset, "000042", 10);
        assert!(rt.contains("Reset your password") && rh.contains("Reset your password"));
        // hostile values never become markup
        let out = fill("<b>{{code}}</b>", &[("code", "<script>\"&'")], true);
        assert_eq!(out, "<b>&lt;script&gt;&quot;&amp;&#39;</b>");
        assert!(!html.contains("<script"));
    }

    #[test]
    fn ses_suppression_is_undeliverable() {
        let body = br#"{"message":"Address is on the account-level suppression list."}"#;
        assert!(matches!(ses_error(400, Some("MessageRejected"), body), MailError::Undeliverable(_)));
        assert!(matches!(ses_error(400, Some("MessageRejected"), b"{\"message\":\"Email address is not verified.\"}"), MailError::Failed(_)));
        assert!(matches!(ses_error(500, None, b""), MailError::Failed(_)));
        assert!(matches!(ses_error(429, Some("TooManyRequestsException"), b""), MailError::Failed(_)));
    }

    #[test]
    fn smtp_url_and_message() {
        let u = parse_smtp_url("smtp+starttls://me%40x:p%3Aw@mail.example.com").unwrap();
        assert_eq!((u.port, u.tls), (587, SmtpTls::StartTls));
        assert_eq!(u.user, Some(("me@x".into(), "p:w".into())));
        assert_eq!(parse_smtp_url("smtps://h:2465").unwrap().port, 2465);
        assert!(parse_smtp_url("http://x").is_err());
        let m = Mail { to: "a@b.c".into(), subject: "Hi".into(), text: "t".into(), html: Some("<p>h</p>".into()) };
        let msg = build_message("no-reply@napple.lat", &m, "BND", "2026-11-03");
        assert!(msg.contains("multipart/alternative") && msg.contains("text/html") && msg.ends_with("--BND--\r\n"));
    }
}
