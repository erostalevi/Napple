//! napple-room: a small server that lets Napple instances play local multiplayer over the internet.
//!
//! Napple instances (`NAPPLE_NET=host NAPPLE_ROOM=<server>/<room>`) join a named room. The server gives each member an address in a private virtual LAN
//! and relays two kinds of traffic between the members: the LAN traffic of titles' "LAN play" modes (UDP and TCP between virtual addresses, broadcasts
//! fanned out to every member) and local-wireless (LDN) sessions (networks advertised by a member, scans answered by the server, the session frames
//! relayed like LAN packets). The protocol is Napple's own (`proto`); nothing in here talks to, imitates or knows any Nintendo service.

pub mod client;
pub mod proto;
pub mod server;

pub use server::{Config, Handle, Server};
