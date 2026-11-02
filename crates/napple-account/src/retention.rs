//! Periodic cleanup (hourly in `serve`; tests call `run_once` with a moved clock): uploads that were never
//! committed (and their objects), expired email codes, stale lockouts, unconfirmed signups and refresh tokens, and the blob deletion queue.

use crate::app::App;
use crate::saves::{delete_versions, flush_deletions};

#[derive(Debug, Default, PartialEq)]
pub struct Report {
    pub expired_uploads: usize,
    pub blobs_deleted: usize,
    pub unverified_removed: usize,
}

pub async fn run_once(app: &App) -> Result<Report, String> {
    let now = app.now();
    let e = |e: rusqlite::Error| e.to_string();
    let mut rep = Report::default();
    let mut keys = {
        let db = app.db();
        let cutoff = now - app.cfg.pending_upload_ttl;
        let keys = delete_versions(
            &db,
            "state = 'pending' AND created_at < ?1",
            &[&cutoff],
            now,
        )
        .map_err(e)?;
        rep.expired_uploads = keys.len();
        db.execute("DELETE FROM auth_codes WHERE expires_at < ?1", [now])
            .map_err(e)?;
        db.execute(
            "DELETE FROM login_failures WHERE locked_until < ?1 AND window_start < ?1 - ?2",
            rusqlite::params![now, crate::ratelimit::WINDOW],
        )
        .map_err(e)?;
        // Signups never confirmed within a day (the password hash and pending nickname go with them).
        rep.unverified_removed = db
            .execute(
                "DELETE FROM accounts WHERE verified_at IS NULL AND created_at < ?1",
                [now - 86_400],
            )
            .map_err(e)?;
        db.execute("DELETE FROM refresh_tokens WHERE expires_at < ?1", [now])
            .map_err(e)?;
        keys
    };
    let queued: Vec<String> = {
        let db = app.db();
        let mut q = db
            .prepare("SELECT object_key FROM blob_deletions ORDER BY queued_at LIMIT 1000")
            .map_err(e)?;
        q.query_map([], |r| r.get(0))
            .map_err(e)?
            .collect::<Result<_, _>>()
            .map_err(e)?
    };
    for k in queued {
        if !keys.contains(&k) {
            keys.push(k);
        }
    }
    let count = |app: &App| {
        app.db()
            .query_row("SELECT COUNT(*) FROM blob_deletions", [], |r| {
                r.get::<_, i64>(0)
            })
            .map_err(|e| e.to_string())
    };
    let before = count(app)?;
    flush_deletions(app, keys).await;
    rep.blobs_deleted = (before - count(app)?).max(0) as usize;
    Ok(rep)
}
