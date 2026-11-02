//! Mail providers: the SES v2 request (config set, multipart, SigV4), suppression mapping, and the SMTP client
//! against a local fake server.

use napple_account::aws::{CredentialProvider, Credentials};
use napple_account::mailer::{Mail, MailError, Mailer, SesMailer, SmtpMailer, parse_smtp_url};
use napple_account::outbound::{MockOutbound, json_response};
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

const NOW: i64 = 1_793_577_600;

fn mail() -> Mail {
    Mail {
        to: "p@example.com".into(),
        subject: "Your Napple confirmation code".into(),
        text: "code 123456".into(),
        html: Some("<p>code 123456</p>".into()),
    }
}

fn ses(out: Arc<MockOutbound>) -> SesMailer {
    let creds = Credentials { access_key: "AKIDEXAMPLE".into(), secret_key: "secret".into(), session_token: None, expires_at: None };
    SesMailer {
        region: "us-east-1".into(),
        from: "no-reply@napple.lat".into(),
        config_set: "napple-transactional".into(),
        creds: Arc::new(CredentialProvider::fixed(creds, out.clone())),
        out,
    }
}

#[tokio::test]
async fn ses_request_is_signed_multipart_with_the_config_set() {
    let out = Arc::new(MockOutbound::new(|_| json_response(200, serde_json::json!({"MessageId": "x"}))));
    ses(out.clone()).send(mail(), NOW).await.unwrap();
    let r = &out.requests()[0];
    assert_eq!(r.url, "https://email.us-east-1.amazonaws.com/v2/email/outbound-emails");
    let auth = r.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("authorization")).unwrap();
    assert!(auth.1.starts_with("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20261102/us-east-1/ses/aws4_request"), "{}", auth.1);
    let b: serde_json::Value = serde_json::from_slice(&r.body).unwrap();
    assert_eq!(b["ConfigurationSetName"], "napple-transactional");
    assert_eq!(b["FromEmailAddress"], "no-reply@napple.lat");
    assert_eq!(b["Destination"]["ToAddresses"][0], "p@example.com");
    assert_eq!(b["Content"]["Simple"]["Body"]["Text"]["Data"], "code 123456");
    assert_eq!(b["Content"]["Simple"]["Body"]["Html"]["Data"], "<p>code 123456</p>");
}

#[tokio::test]
async fn ses_suppression_and_outages_map_to_distinct_errors() {
    let out = Arc::new(MockOutbound::new(|_| {
        let mut r = json_response(400, serde_json::json!({"message": "Address is on the account-level suppression list."}));
        r.headers.push(("x-amzn-errortype".into(), "MessageRejected".into()));
        r
    }));
    assert!(matches!(ses(out).send(mail(), NOW).await, Err(MailError::Undeliverable(_))));
    let out = Arc::new(MockOutbound::new(|_| json_response(503, serde_json::json!({}))));
    assert!(matches!(ses(out).send(mail(), NOW).await, Err(MailError::Failed(_))));
}

/// A tiny SMTP server: records the DATA and rejects RCPT for `reject@...`.
async fn fake_smtp() -> (u16, tokio::task::JoinHandle<String>) {
    let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = l.local_addr().unwrap().port();
    let h = tokio::spawn(async move {
        let mut seen = String::new();
        // serve two connections: one delivered, one rejected
        for _ in 0..2 {
            let (s, _) = l.accept().await.unwrap();
            let (r, mut w) = s.into_split();
            let mut r = BufReader::new(r);
            w.write_all(b"220 fake ESMTP\r\n").await.unwrap();
            let mut in_data = false;
            let mut line = String::new();
            loop {
                line.clear();
                if r.read_line(&mut line).await.unwrap() == 0 {
                    break;
                }
                if in_data {
                    seen.push_str(&line);
                    if line == ".\r\n" {
                        in_data = false;
                        w.write_all(b"250 queued\r\n").await.unwrap();
                    }
                    continue;
                }
                let up = line.to_ascii_uppercase();
                let reply: &[u8] = if up.starts_with("EHLO") {
                    b"250-fake\r\n250 8BITMIME\r\n"
                } else if up.starts_with("RCPT") && line.contains("reject@") {
                    b"550 no such user\r\n"
                } else if up.starts_with("DATA") {
                    in_data = true;
                    b"354 go\r\n"
                } else if up.starts_with("QUIT") {
                    w.write_all(b"221 bye\r\n").await.unwrap();
                    break;
                } else {
                    b"250 ok\r\n"
                };
                w.write_all(reply).await.unwrap();
            }
        }
        seen
    });
    (port, h)
}

#[tokio::test]
async fn smtp_delivers_multipart_and_maps_rejected_recipients() {
    let (port, h) = fake_smtp().await;
    let m = SmtpMailer { cfg: parse_smtp_url(&format!("smtp://127.0.0.1:{port}")).unwrap(), from: "no-reply@napple.lat".into() };
    m.send(mail(), NOW).await.unwrap();
    let mut bad = mail();
    bad.to = "reject@example.com".into();
    assert!(matches!(m.send(bad, NOW).await, Err(MailError::Undeliverable(_))));
    let data = h.await.unwrap();
    assert!(data.contains("Subject: Your Napple confirmation code"));
    assert!(data.contains("multipart/alternative") && data.contains("text/html"));
    assert!(data.contains("To: <p@example.com>"));
    // unreachable server is a plain failure, not "undeliverable"
    let dead = SmtpMailer { cfg: parse_smtp_url("smtp://127.0.0.1:1").unwrap(), from: "a@b.c".into() };
    assert!(matches!(dead.send(mail(), NOW).await, Err(MailError::Failed(_))));
}
