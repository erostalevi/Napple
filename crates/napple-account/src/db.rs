//! SQLite (WAL) schema. One connection behind a mutex: the server is small and every query is short.
//! Never hold the lock across an `.await`.

use rusqlite::Connection;
use std::path::Path;

const SCHEMA: &str = r#"
CREATE TABLE IF NOT EXISTS accounts (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    -- end-to-end save key: the server only knows a client-chosen fingerprint and opaque wrapped blobs
    key_id TEXT,
    recovery_blob TEXT,
    recovery_updated_at INTEGER
);

CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    platform TEXT NOT NULL,
    pubkey TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    revoked_at INTEGER,
    session_epoch INTEGER NOT NULL DEFAULT 0,
    wrapped_key TEXT,
    key_granted_by TEXT,
    key_granted_at INTEGER
);
CREATE INDEX IF NOT EXISTS devices_account ON devices(account_id);

CREATE TABLE IF NOT EXISTS refresh_tokens (
    hash BLOB PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER
);
CREATE INDEX IF NOT EXISTS refresh_device ON refresh_tokens(device_id);

CREATE TABLE IF NOT EXISTS profiles (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    nickname TEXT NOT NULL,
    icon_id INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS profiles_account ON profiles(account_id);

-- Admin-set permissions (reserved names, admin): 'owner_napple' | 'reserved_prefix' | 'admin'.
CREATE TABLE IF NOT EXISTS grants (
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    granted_by TEXT,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (account_id, name)
);

CREATE TABLE IF NOT EXISTS save_versions (
    id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    title_id TEXT NOT NULL,
    profile_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    size INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    state TEXT NOT NULL,            -- 'pending' | 'committed'
    device_id TEXT NOT NULL,
    device_name TEXT NOT NULL,
    object_key TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    committed_at INTEGER
);
CREATE UNIQUE INDEX IF NOT EXISTS save_versions_committed
    ON save_versions(account_id, title_id, profile_id, version) WHERE state = 'committed';
CREATE INDEX IF NOT EXISTS save_versions_lookup ON save_versions(account_id, title_id, profile_id, state);

-- Objects to delete from the blob store (retried by the retention job until the store confirms).
CREATE TABLE IF NOT EXISTS blob_deletions (
    object_key TEXT PRIMARY KEY,
    queued_at INTEGER NOT NULL
);

"#;

pub fn open(path: &Path) -> rusqlite::Result<Connection> {
    let c = Connection::open(path)?;
    init(&c)?;
    Ok(c)
}

pub fn open_memory() -> rusqlite::Result<Connection> {
    let c = Connection::open_in_memory()?;
    init(&c)?;
    Ok(c)
}

fn init(c: &Connection) -> rusqlite::Result<()> {
    c.pragma_update(None, "journal_mode", "WAL")?;
    c.pragma_update(None, "synchronous", "NORMAL")?;
    c.pragma_update(None, "foreign_keys", "ON")?;
    c.busy_timeout(std::time::Duration::from_secs(5))?;
    c.execute_batch(SCHEMA)?;
    crate::codes::migrate(c)
}
