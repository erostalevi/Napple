//! napple-online: a community-run online service for Napple, in the spirit of Pretendo. Napple instances get Napple-issued accounts and tokens
//! from it, and titles' NEX game-server traffic (redirected by the Napple runtime, see runtime/src/net/net_online.c) lands on its NEX-compatible
//! game server, so Napple users play online modes with each other.
//!
//! It never talks to Nintendo, holds no Nintendo keys, certificates or credentials, and needs no per-game secret: the PRUDP Lite transport is
//! not signed, and Kerberos tickets are sealed with keys this server chooses. Original code; the public NEX protocol documentation
//! (NintendoClients wiki, Pretendo docs) was used for message layouts only. See docs/guides/napple-online.md.

pub mod accounts;
pub mod crypto;
pub mod http;
pub mod kerberos;
pub mod matchmaking;
pub mod nex;
pub mod prudp;
pub mod rmc;
pub mod server;
pub mod session;

use std::net::TcpListener;
use std::sync::Arc;

/// Accepts connections forever (one thread each).
pub fn run(listener: TcpListener, server: Arc<server::Server>) -> std::io::Result<()> {
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(e) => {
                println!("napple-online: accept failed: {e}");
                continue;
            }
        };
        let srv = server.clone();
        std::thread::spawn(move || session::serve(srv, stream));
    }
    Ok(())
}
