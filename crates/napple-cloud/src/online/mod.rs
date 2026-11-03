//! Napple Online client core (docs/guides/napple-online-client.md): the credentials shared by every Napple game on this Mac (`creds`), the
//! account / profile / social client (`client`) and the C API the runtime's overlay glue calls (`ffi`).
pub mod client;
pub mod creds;
pub mod ffi;

#[cfg(test)]
mod tests;
