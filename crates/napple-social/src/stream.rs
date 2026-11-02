//! `GET /social/v1/stream`: the live-update WebSocket. Authenticated like every other call (headers on the upgrade
//! request). The server sends JSON events (see [`crate::hub::Event`]); clients may send:
//!   `{"type":"ping"}`                 -> `pong`
//!   `{"type":"heartbeat"}`            -> presence heartbeat, `heartbeat_ack` (or an `error` event with `presence_expired`)
//!   `{"type":"reauth","token":"..."}` -> swaps in a fresh access token (same account); `reauthed`
//! The socket closes with code 4001 when its token expires without a reauth, 4008 when the client cannot keep up.

use std::time::Duration;

use axum::extract::State;
use axum::extract::ws::{CloseFrame, Message, WebSocket, WebSocketUpgrade};
use axum::response::Response;
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;

use crate::config::TOKEN_LEEWAY_SECS;
use crate::hub::Event;
use crate::{AppState, Caller, api, db};

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum ClientMsg {
    Ping,
    Heartbeat,
    Reauth { token: String },
}

pub async fn stream(State(s): State<AppState>, caller: Caller, ws: WebSocketUpgrade) -> Response {
    ws.max_message_size(16 * 1024).on_upgrade(move |socket| run(socket, s, caller))
}

fn close(code: u16, reason: &'static str) -> Message {
    Message::Close(Some(CloseFrame { code, reason: reason.into() }))
}

async fn run(socket: WebSocket, s: AppState, caller: Caller) {
    let pid = caller.profile_id.clone();
    let (conn_id, mut rx) = s.hub.register(&pid);
    tracing::debug!(profile = %pid, conn = conn_id, "stream open");
    let (mut tx, mut inbound) = socket.split();

    // Hello, then the live invites this profile missed while disconnected.
    let now = s.now_ms();
    let p2 = pid.clone();
    let initial = s
        .db
        .call(move |c| {
            let p = db::must_profile(c, &p2)?;
            let invites = db::invites_for(c, &p2, false, now)?.iter().map(|i| db::invite_view(c, i)).collect::<Result<Vec<_>, _>>()?;
            Ok((p.friend_code, invites))
        })
        .await;
    let Ok((code, invites)) = initial else {
        let _ = tx.send(close(1011, "internal error")).await;
        s.hub.unregister(&pid, conn_id);
        return;
    };
    let mut token_exp = caller.token_exp;
    let hello = Event::Hello {
        profile_id: pid.clone(),
        friend_code: crate::codes::display(&code),
        server_time_ms: now,
        heartbeat_interval_secs: s.config.heartbeat_secs,
        token_expires_at: token_exp,
    };
    let mut ok = tx.send(Message::Text(hello.to_json().to_string().into())).await.is_ok();
    for invite in invites {
        if !ok {
            break;
        }
        ok = tx.send(Message::Text(Event::Invite { invite }.to_json().to_string().into())).await.is_ok();
    }

    let mut ping = tokio::time::interval(Duration::from_secs(30));
    ping.tick().await;
    let expiry = |exp: i64, s: &AppState| {
        let ms = (exp + TOKEN_LEEWAY_SECS) * 1000 - s.now_ms();
        tokio::time::Instant::now() + Duration::from_millis(ms.max(0) as u64)
    };
    let deadline = tokio::time::sleep_until(expiry(token_exp, &s));
    tokio::pin!(deadline);

    while ok {
        tokio::select! {
            out = rx.recv() => match out {
                Some(msg) => ok = tx.send(Message::Text(msg.to_string().into())).await.is_ok(),
                None => {
                    // The hub dropped us: queue overflow.
                    let _ = tx.send(close(4008, "too slow")).await;
                    break;
                }
            },
            msg = inbound.next() => {
                let Some(Ok(msg)) = msg else { break };
                let text = match msg {
                    Message::Text(t) => t.to_string(),
                    Message::Binary(_) => { ok = send(&mut tx, &Event::Error { code: "unsupported".into(), message: "text JSON only".into() }).await; continue }
                    Message::Close(_) => break,
                    _ => continue,
                };
                if let Err(e) = s.limits.account.check(&caller.account_id, s.now_ms()) {
                    ok = send(&mut tx, &Event::Error { code: e.code.into(), message: e.message }).await;
                    continue;
                }
                let reply = match serde_json::from_str::<ClientMsg>(&text) {
                    Ok(ClientMsg::Ping) => Event::Pong { server_time_ms: s.now_ms() },
                    Ok(ClientMsg::Heartbeat) => match api::do_heartbeat(&s, &pid).await {
                        Ok(presence) => Event::HeartbeatAck { presence },
                        Err(e) => Event::Error { code: e.code.into(), message: e.message },
                    },
                    Ok(ClientMsg::Reauth { token }) => match s.auth.verify(&token, s.now_ms()).await {
                        Ok(c) if c.account_id == caller.account_id => {
                            token_exp = c.exp;
                            deadline.as_mut().reset(expiry(token_exp, &s));
                            Event::Reauthed { token_expires_at: token_exp }
                        }
                        Ok(_) => Event::Error { code: "account_mismatch".into(), message: "token is for another account".into() },
                        Err(e) => Event::Error { code: e.code.into(), message: e.message },
                    },
                    Err(_) => Event::Error { code: "invalid_message".into(), message: "expected {\"type\": \"ping\" | \"heartbeat\" | \"reauth\"}".into() },
                };
                ok = send(&mut tx, &reply).await;
            },
            _ = ping.tick() => ok = tx.send(Message::Ping(Vec::new().into())).await.is_ok(),
            _ = &mut deadline => {
                let _ = tx.send(close(4001, "token expired")).await;
                break;
            }
        }
    }
    s.hub.unregister(&pid, conn_id);
    tracing::debug!(profile = %pid, conn = conn_id, "stream closed");
}

async fn send(tx: &mut futures_util::stream::SplitSink<WebSocket, Message>, ev: &Event) -> bool {
    tx.send(Message::Text(ev.to_json().to_string().into())).await.is_ok()
}
