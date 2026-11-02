//! Periodic cleanup (hourly in `serve`; tests call `run_once` with a moved clock):
//! expired uploads and ceremonies, the blob deletion queue, and read-only titles: a warning email
//! `deletion_warning_days` before the end of the read-only window, then deletion of their saves.

use crate::app::App;
use crate::mailer::Mail;
use crate::plans::delete_after;
use crate::saves::{delete_versions, flush_deletions};
use crate::util::ymd;
use rusqlite::params;

const DAY: i64 = 86_400;

#[derive(Debug, Default, PartialEq)]
pub struct Report {
    pub expired_uploads: usize,
    pub warned: usize,
    pub deleted_titles: usize,
    pub blobs_deleted: usize,
}

pub async fn run_once(app: &App) -> Result<Report, String> {
    let now = app.now();
    let mut rep = Report::default();
    let mut keys = Vec::new();
    let mut warnings: Vec<(String, String, String, i64)> = Vec::new();
    {
        let db = app.db();
        let e = |e: rusqlite::Error| e.to_string();
        let k = delete_versions(
            &db,
            "state = 'pending' AND created_at < ?1",
            &[&(now - app.cfg.pending_upload_ttl)],
            now,
        )
        .map_err(e)?;
        rep.expired_uploads = k.len();
        keys.extend(k);
        db.execute("DELETE FROM email_challenges WHERE expires_at < ?1", [now])
            .map_err(e)?;
        db.execute(
            "DELETE FROM webauthn_challenges WHERE expires_at < ?1",
            [now],
        )
        .map_err(e)?;
        db.execute("DELETE FROM refresh_tokens WHERE expires_at < ?1", [now])
            .map_err(e)?;
        db.execute(
            "DELETE FROM stripe_events WHERE received_at < ?1",
            [now - 90 * DAY],
        )
        .map_err(e)?;

        let mut st = db
            .prepare("SELECT t.account_id, t.title_id, t.readonly_since, t.warned_at, a.email FROM titles t JOIN accounts a ON a.id = t.account_id WHERE t.state = 'readonly'")
            .map_err(e)?;
        let rows: Vec<(String, String, i64, Option<i64>, String)> = st
            .query_map([], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?))
            })
            .map_err(e)?
            .collect::<Result<_, _>>()
            .map_err(e)?;
        drop(st);
        for (acc, title, since, warned, email) in rows {
            let end = delete_after(app, Some(since), warned).unwrap_or(i64::MAX);
            match warned {
                None if now >= end - app.cfg.deletion_warning_days * DAY => {
                    warnings.push((
                        email,
                        title,
                        acc,
                        delete_after(app, Some(since), Some(now)).unwrap_or(end),
                    ));
                }
                Some(_) if now >= end => {
                    keys.extend(
                        delete_versions(
                            &db,
                            "account_id = ?1 AND title_id = ?2",
                            &[&acc, &title],
                            now,
                        )
                        .map_err(e)?,
                    );
                    db.execute(
                        "DELETE FROM titles WHERE account_id = ?1 AND title_id = ?2",
                        params![acc, title],
                    )
                    .map_err(e)?;
                    rep.deleted_titles += 1;
                }
                _ => {}
            }
        }
        let mut q = db
            .prepare("SELECT object_key FROM blob_deletions ORDER BY queued_at LIMIT 1000")
            .map_err(e)?;
        let queued: Vec<String> = q
            .query_map([], |r| r.get(0))
            .map_err(e)?
            .collect::<Result<_, _>>()
            .map_err(e)?;
        for k in queued {
            if !keys.contains(&k) {
                keys.push(k);
            }
        }
    }
    // warned_at is only set once the email went out, so nobody loses saves without the warning.
    for (email, title, acc, when) in warnings {
        let text = format!(
            "Your Napple Free plan no longer syncs title {title}.\n\nIts cloud saves are read-only and will be deleted on {} unless you download them, make it your synced title, or upgrade to Napple Plus.\n\nYour local saves on your devices are not affected.\n",
            ymd(when)
        );
        match app
            .mailer
            .send(
                Mail {
                    to: email,
                    subject: "Your Napple cloud saves will be deleted soon".into(),
                    text,
                },
                now,
            )
            .await
        {
            Ok(()) => {
                let _ = app.db().execute(
                    "UPDATE titles SET warned_at = ?1 WHERE account_id = ?2 AND title_id = ?3",
                    params![now, acc, title],
                );
                rep.warned += 1;
            }
            Err(e) => crate::log::warn(&format!("retention warning mail: {e}")),
        }
    }
    let before: i64 = app
        .db()
        .query_row("SELECT COUNT(*) FROM blob_deletions", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;
    flush_deletions(app, keys).await;
    let after: i64 = app
        .db()
        .query_row("SELECT COUNT(*) FROM blob_deletions", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;
    rep.blobs_deleted = (before - after).max(0) as usize;
    Ok(rep)
}
