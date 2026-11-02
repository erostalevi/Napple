//! A minimal blocking client of the room protocol: the `list` / `probe` commands and the tests use it. The runtime has its own client in C
//! (`runtime/src/net/net_room.c`).

use crate::proto::{self, Msg, ProtoError};
use std::io;
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

pub struct Client {
    pub stream: TcpStream,
    pub member_id: u32,
    pub vip: u32,
    pub netmask: u32,
    pub members: Vec<proto::Member>,
}

#[derive(Debug)]
pub enum JoinError {
    Io(io::Error),
    Refused { code: u32, message: String },
    Protocol(String),
}
impl From<io::Error> for JoinError {
    fn from(e: io::Error) -> Self {
        JoinError::Io(e)
    }
}
impl From<ProtoError> for JoinError {
    fn from(e: ProtoError) -> Self {
        match e {
            ProtoError::Io(e) => JoinError::Io(e),
            ProtoError::Malformed(m) => JoinError::Protocol(m.into()),
        }
    }
}

fn connect(addr: impl ToSocketAddrs) -> io::Result<TcpStream> {
    let mut last = io::Error::new(io::ErrorKind::NotFound, "no address");
    for a in addr.to_socket_addrs()? {
        match TcpStream::connect_timeout(&a, Duration::from_secs(5)) {
            Ok(s) => {
                s.set_nodelay(true)?;
                s.set_read_timeout(Some(Duration::from_secs(10)))?;
                return Ok(s);
            }
            Err(e) => last = e,
        }
    }
    Err(last)
}

/// The rooms on a server.
pub fn list(addr: impl ToSocketAddrs) -> Result<Vec<proto::RoomInfo>, JoinError> {
    let mut s = connect(addr)?;
    proto::write_msg(&mut s, &Msg::List)?;
    match proto::read_msg(&mut s)? {
        Msg::ListResult { rooms } => Ok(rooms),
        Msg::Error { code, message } => Err(JoinError::Refused { code, message }),
        m => Err(JoinError::Protocol(format!("unexpected {m:?}"))),
    }
}

impl Client {
    pub fn join(addr: impl ToSocketAddrs, room: &str, password: &str, nick: &str, want_vip: u32) -> Result<Client, JoinError> {
        let mut s = connect(addr)?;
        proto::write_msg(
            &mut s,
            &Msg::Hello { version: proto::VERSION, want_vip, room: room.into(), password: password.into(), nick: nick.into() },
        )?;
        match proto::read_msg(&mut s)? {
            Msg::Welcome { member_id, vip, netmask, members, .. } => Ok(Client { stream: s, member_id, vip, netmask, members }),
            Msg::Error { code, message } => Err(JoinError::Refused { code, message }),
            m => Err(JoinError::Protocol(format!("unexpected {m:?}"))),
        }
    }

    pub fn send(&mut self, m: &Msg) -> io::Result<()> {
        proto::write_msg(&mut self.stream, m)
    }

    /// The next frame, or None when nothing arrives within `timeout` (or the connection ended).
    pub fn recv(&mut self, timeout: Duration) -> Option<Msg> {
        self.stream.set_read_timeout(Some(timeout)).ok()?;
        proto::read_msg(&mut self.stream).ok()
    }

    /// The next frame that is not a membership note or a pong.
    pub fn recv_data(&mut self, timeout: Duration) -> Option<Msg> {
        let end = std::time::Instant::now() + timeout;
        loop {
            let left = end.checked_duration_since(std::time::Instant::now())?;
            match self.recv(left.max(Duration::from_millis(1)))? {
                Msg::MemberJoin(_) | Msg::MemberLeave { .. } | Msg::Pong { .. } => continue,
                m => return Some(m),
            }
        }
    }
}
