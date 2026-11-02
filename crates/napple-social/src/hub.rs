//! Live connections: profile id -> the WebSocket streams of that profile (one per device). Events are serialised once
//! and fanned out with `try_send`; a client too slow to drain its queue is disconnected rather than buffered forever.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};

use serde::Serialize;
use tokio::sync::mpsc;

use crate::db::{FriendView, InviteView, PresenceView, RequestView};

/// Per-connection queue depth.
pub const QUEUE: usize = 256;

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Event {
    /// First message on every stream.
    Hello { profile_id: String, friend_code: String, server_time_ms: i64, heartbeat_interval_secs: i64, token_expires_at: i64 },
    /// A friend's visible presence changed (or your own, from another device).
    Presence { profile_id: String, presence: PresenceView },
    /// Someone sent you a friend request.
    FriendRequest { request: RequestView },
    /// A pending request involving you is gone (accepted elsewhere, declined, cancelled, blocked).
    FriendRequestClosed { request_id: String },
    /// A new friend (your request was accepted, or you accepted on another device).
    FriendAdded { friend: FriendView },
    /// A friendship ended.
    FriendRemoved { profile_id: String },
    /// A friend invited you to a joinable session.
    Invite { invite: InviteView },
    /// An invite is gone: `reason` is "expired", "cancelled", "replaced", "accepted", "declined" or "removed".
    InviteClosed { invite_id: String, reason: String },
    /// Your invite was answered.
    InviteResponse { invite_id: String, profile_id: String, accepted: bool },
    /// Your profile's settings changed (from another device).
    Settings { presence_visibility: String, appear_offline: bool, accept_friend_requests: bool },
    Pong { server_time_ms: i64 },
    Reauthed { token_expires_at: i64 },
    HeartbeatAck { presence: PresenceView },
    Error { code: String, message: String },
}

impl Event {
    pub fn to_json(&self) -> Arc<str> {
        serde_json::to_string(self).unwrap_or_else(|_| "{\"type\":\"error\"}".into()).into()
    }
}

type Conns = HashMap<String, Vec<(u64, mpsc::Sender<Arc<str>>)>>;

#[derive(Default)]
pub struct Hub {
    conns: Mutex<Conns>,
    next: AtomicU64,
}

impl Hub {
    pub fn register(&self, profile_id: &str) -> (u64, mpsc::Receiver<Arc<str>>) {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel(QUEUE);
        self.conns.lock().unwrap_or_else(|e| e.into_inner()).entry(profile_id.to_string()).or_default().push((id, tx));
        (id, rx)
    }

    pub fn unregister(&self, profile_id: &str, id: u64) {
        let mut m = self.conns.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(v) = m.get_mut(profile_id) {
            v.retain(|(i, _)| *i != id);
            if v.is_empty() {
                m.remove(profile_id);
            }
        }
    }

    pub fn send(&self, profile_id: &str, ev: &Event) {
        self.send_raw(profile_id, ev.to_json());
    }

    pub fn send_raw(&self, profile_id: &str, msg: Arc<str>) {
        let mut m = self.conns.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(v) = m.get_mut(profile_id) {
            // A full or closed queue drops the sender; its stream task then sees the channel closed and hangs up.
            v.retain(|(_, tx)| tx.try_send(msg.clone()).is_ok());
            if v.is_empty() {
                m.remove(profile_id);
            }
        }
    }

    /// Sends one event to many profiles, serialising it once.
    pub fn send_many<'a>(&self, profiles: impl IntoIterator<Item = &'a String>, ev: &Event) {
        let msg = ev.to_json();
        for p in profiles {
            self.send_raw(p, msg.clone());
        }
    }

    pub fn connections(&self, profile_id: &str) -> usize {
        self.conns.lock().unwrap_or_else(|e| e.into_inner()).get(profile_id).map_or(0, Vec::len)
    }

    pub fn total(&self) -> usize {
        self.conns.lock().unwrap_or_else(|e| e.into_inner()).values().map(Vec::len).sum()
    }
}
