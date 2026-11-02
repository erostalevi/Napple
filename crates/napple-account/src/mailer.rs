//! Outgoing email: Amazon SES v2 in production, a stdout printer for local dev, a capture buffer for tests.

use crate::aws::{CredentialProvider, SignInput, sign_headers};
use crate::outbound::{BoxFut, OutRequest, Outbound};
use crate::util::sha256_hex;
use serde_json::json;
use std::sync::{Arc, Mutex};

#[derive(Clone, Debug)]
pub struct Mail {
    pub to: String,
    pub subject: String,
    pub text: String,
}

pub trait Mailer: Send + Sync {
    fn send(&self, mail: Mail, now: i64) -> BoxFut<'_, Result<(), String>>;
}

/// Prints the whole message (including sign-in codes) to stdout. Refused by config in production.
pub struct DevMailer;

impl Mailer for DevMailer {
    fn send(&self, mail: Mail, _now: i64) -> BoxFut<'_, Result<(), String>> {
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
}

impl Mailer for CaptureMailer {
    fn send(&self, mail: Mail, _now: i64) -> BoxFut<'_, Result<(), String>> {
        self.sent.lock().unwrap().push(mail);
        Box::pin(async { Ok(()) })
    }
}

pub struct SesMailer {
    pub region: String,
    pub from: String,
    pub creds: Arc<CredentialProvider>,
    pub out: Arc<dyn Outbound>,
}

impl Mailer for SesMailer {
    fn send(&self, mail: Mail, now: i64) -> BoxFut<'_, Result<(), String>> {
        Box::pin(async move {
            let creds = self.creds.get(now).await?;
            let host = format!("email.{}.amazonaws.com", self.region);
            let path = "/v2/email/outbound-emails";
            let body = json!({
                "FromEmailAddress": self.from,
                "Destination": {"ToAddresses": [mail.to]},
                "Content": {"Simple": {"Subject": {"Data": mail.subject, "Charset": "UTF-8"}, "Body": {"Text": {"Data": mail.text, "Charset": "UTF-8"}}}}
            })
            .to_string()
            .into_bytes();
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
                .await?;
            if r.status / 100 != 2 {
                return Err(format!("SES SendEmail: HTTP {}", r.status));
            }
            Ok(())
        })
    }
}
