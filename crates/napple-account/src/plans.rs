//! Plans and entitlements. Free: 1 synced title (switchable once per 30 days), 10 versions, 256 MiB.
//! Napple Plus: unlimited titles, 20 versions, 5 GiB. Numbers come from config. Never hold data hostage: titles beyond
//! the plan become read-only (downloadable) for 90 days, then the retention job deletes them after an email warning.

use crate::app::App;
use crate::error::{ApiError, ApiResult};
use axum::http::StatusCode;
use rusqlite::{Connection, OptionalExtension, params};
use serde_json::{Value, json};

const DAY: i64 = 86_400;

pub fn is_plus(db: &Connection, account_id: &str) -> rusqlite::Result<bool> {
    Ok(db.query_row(
        "SELECT plan FROM accounts WHERE id = ?1",
        [account_id],
        |r| r.get::<_, String>(0),
    )? == "plus")
}

/// Stripe subscription statuses that grant Plus (past_due keeps Plus while Stripe retries the payment).
pub fn status_grants_plus(status: &str) -> bool {
    matches!(status, "active" | "trialing" | "past_due")
}

pub fn upgrade_url(app: &App) -> String {
    format!("{}/billing", app.cfg.public_base)
}

pub fn usage(app: &App, db: &Connection, account_id: &str, now: i64) -> rusqlite::Result<u64> {
    db.query_row(
        "SELECT COALESCE(SUM(size), 0) FROM save_versions WHERE account_id = ?1 AND (state = 'committed' OR created_at > ?2)",
        params![account_id, now - app.cfg.pending_upload_ttl],
        |r| r.get::<_, i64>(0),
    )
    .map(|v| v as u64)
}

pub fn plan_limit_error(app: &App, reason: &'static str, msg: &str) -> ApiError {
    ApiError::new(StatusCode::PAYMENT_REQUIRED, "plan_limit", msg)
        .with("reason", reason)
        .with("upgrade_url", upgrade_url(app))
}

pub fn plan_json(app: &App, db: &Connection, account_id: &str, now: i64) -> ApiResult<Value> {
    let (plan, status, interval, renews_at, cancel): (String, Option<String>, Option<String>, Option<i64>, bool) = db.query_row(
        "SELECT plan, sub_status, sub_interval, renews_at, cancel_at_period_end FROM accounts WHERE id = ?1",
        [account_id],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?)),
    )?;
    let plus = plan == "plus";
    let l = app.cfg.limits(plus);
    Ok(json!({
        "name": if plus { "plus" } else { "free" },
        "display_name": if plus { "Napple Plus" } else { "Free" },
        "status": status.unwrap_or_else(|| if plus { "active".into() } else { "none".into() }),
        "interval": interval,
        "renews_at": renews_at,
        "cancel_at_period_end": cancel,
        "synced_titles_limit": if l.synced_titles == 0 { Value::Null } else { json!(l.synced_titles) },
        "versions_per_title": l.versions_per_title,
        "quota": l.quota_bytes,
        "used": usage(app, db, account_id, now)?,
        "billing_available": app.cfg.stripe.is_some(),
        "upgrade_url": if plus { Value::Null } else { json!(upgrade_url(app)) },
    }))
}

fn active_titles(db: &Connection, account_id: &str) -> rusqlite::Result<i64> {
    db.query_row(
        "SELECT COUNT(*) FROM titles WHERE account_id = ?1 AND state = 'active'",
        [account_id],
        |r| r.get(0),
    )
}

fn switch_allowed_at(
    app: &App,
    db: &Connection,
    account_id: &str,
) -> rusqlite::Result<Option<i64>> {
    let (ever, last): (i64, Option<i64>) = db.query_row(
        "SELECT titles_ever_enabled, last_title_switch_at FROM accounts WHERE id = ?1",
        [account_id],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    Ok(if ever == 0 {
        None
    } else {
        Some(
            last.map(|t| t + app.cfg.title_switch_days * DAY)
                .unwrap_or(0),
        )
    })
}

fn activate(db: &Connection, account_id: &str, title: &str, now: i64) -> rusqlite::Result<()> {
    db.execute(
        "INSERT INTO titles (account_id, title_id, state, enabled_at) VALUES (?1, ?2, 'active', ?3)
         ON CONFLICT(account_id, title_id) DO UPDATE SET state = 'active', readonly_since = NULL, warned_at = NULL",
        params![account_id, title, now],
    )?;
    Ok(())
}

/// Makes `title` a synced (active) title or explains why not. `explicit` = the user asked to switch (may demote the
/// current Free title); otherwise this is an upload, which only auto-enables while a slot is free.
pub fn enable_title(
    app: &App,
    db: &Connection,
    account_id: &str,
    title: &str,
    now: i64,
    explicit: bool,
) -> ApiResult<()> {
    let state: Option<String> = db
        .query_row(
            "SELECT state FROM titles WHERE account_id = ?1 AND title_id = ?2",
            params![account_id, title],
            |r| r.get(0),
        )
        .optional()?;
    if state.as_deref() == Some("active") {
        return Ok(());
    }
    let plus = is_plus(db, account_id)?;
    let limit = app.cfg.limits(plus).synced_titles as i64;
    if plus || limit == 0 {
        activate(db, account_id, title, now)?;
        return Ok(());
    }
    let active = active_titles(db, account_id)?;
    let allowed_at = switch_allowed_at(app, db, account_id)?;
    if active >= limit && !explicit {
        return Err(plan_limit_error(
            app,
            "plan_limit",
            "the Free plan syncs one title; switch titles or upgrade to Napple Plus",
        )
        .with("synced_titles_limit", limit)
        .with("next_switch_at", allowed_at.map(|t| t.max(now))));
    }
    if let Some(at) = allowed_at
        && now < at
    {
        return Err(plan_limit_error(
            app,
            "switch_cooldown",
            "the Free plan can switch its synced title once every 30 days",
        )
        .with("next_switch_at", at));
    }
    if active >= limit {
        // Demote the least recently saved active title(s) to read-only.
        let mut st = db.prepare(
            "SELECT t.title_id FROM titles t WHERE t.account_id = ?1 AND t.state = 'active'
             ORDER BY (SELECT COALESCE(MAX(committed_at), t.enabled_at) FROM save_versions s WHERE s.account_id = t.account_id AND s.title_id = t.title_id AND s.state = 'committed') ASC",
        )?;
        let titles: Vec<String> = st
            .query_map([account_id], |r| r.get(0))?
            .collect::<Result<_, _>>()?;
        for t in titles.iter().take((active - limit + 1) as usize) {
            db.execute("UPDATE titles SET state = 'readonly', readonly_since = ?1, warned_at = NULL WHERE account_id = ?2 AND title_id = ?3", params![now, account_id, t])?;
        }
    }
    activate(db, account_id, title, now)?;
    let ever: i64 = db.query_row(
        "SELECT titles_ever_enabled FROM accounts WHERE id = ?1",
        [account_id],
        |r| r.get(0),
    )?;
    if ever == 0 {
        db.execute(
            "UPDATE accounts SET titles_ever_enabled = 1 WHERE id = ?1",
            [account_id],
        )?;
    } else {
        db.execute(
            "UPDATE accounts SET last_title_switch_at = ?1 WHERE id = ?2",
            params![now, account_id],
        )?;
    }
    Ok(())
}

/// Called when the entitlement flips. Upgrade: every read-only title becomes active again.
/// Downgrade: the most recently saved titles up to the Free limit stay active, the rest become read-only now.
pub fn apply_plan_change(
    app: &App,
    db: &Connection,
    account_id: &str,
    plus: bool,
    now: i64,
) -> rusqlite::Result<()> {
    if plus {
        db.execute("UPDATE titles SET state = 'active', readonly_since = NULL, warned_at = NULL WHERE account_id = ?1", [account_id])?;
        return Ok(());
    }
    let limit = app.cfg.free.synced_titles as usize;
    if limit == 0 {
        return Ok(());
    }
    let mut st = db.prepare(
        "SELECT t.title_id FROM titles t WHERE t.account_id = ?1 AND t.state = 'active'
         ORDER BY (SELECT COALESCE(MAX(committed_at), t.enabled_at) FROM save_versions s WHERE s.account_id = t.account_id AND s.title_id = t.title_id AND s.state = 'committed') DESC",
    )?;
    let titles: Vec<String> = st
        .query_map([account_id], |r| r.get(0))?
        .collect::<Result<_, _>>()?;
    for t in titles.iter().skip(limit) {
        db.execute("UPDATE titles SET state = 'readonly', readonly_since = ?1, warned_at = NULL WHERE account_id = ?2 AND title_id = ?3", params![now, account_id, t])?;
    }
    Ok(())
}

pub fn delete_after(app: &App, readonly_since: Option<i64>, warned_at: Option<i64>) -> Option<i64> {
    let since = readonly_since?;
    let base = since + app.cfg.readonly_days * DAY;
    Some(match warned_at {
        Some(w) => base.max(w + app.cfg.deletion_warning_days * DAY),
        None => base,
    })
}

pub fn titles_json(app: &App, db: &Connection, account_id: &str, now: i64) -> ApiResult<Value> {
    let plus = is_plus(db, account_id)?;
    let limit = app.cfg.limits(plus).synced_titles;
    let mut st = db.prepare("SELECT title_id, state, enabled_at, readonly_since, warned_at FROM titles WHERE account_id = ?1 ORDER BY enabled_at")?;
    let titles: Vec<Value> = st
        .query_map([account_id], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, i64>(2)?, r.get::<_, Option<i64>>(3)?, r.get::<_, Option<i64>>(4)?)))?
        .collect::<Result<Vec<_>, _>>()?
        .into_iter()
        .map(|(t, s, e, ro, w)| json!({"title_id": t, "state": s, "enabled_at": e, "readonly_since": ro, "delete_after": delete_after(app, ro, w)}))
        .collect();
    let next = if plus || limit == 0 {
        None
    } else {
        switch_allowed_at(app, db, account_id)?.map(|t| t.max(now))
    };
    Ok(
        json!({"synced_titles_limit": if limit == 0 { Value::Null } else { json!(limit) }, "next_switch_at": next, "titles": titles}),
    )
}
