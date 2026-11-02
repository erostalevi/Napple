//! The NAT check a title's networking library (Pia) runs before matchmaking: UDP messages to a primary and a secondary server. Each server has a
//! primary port (10025) and a secondary port (10125) for the reachability and filtering tests, and two more (33334, 33335, seen from MK8D) for the
//! mapping tests. The server echoes the message with the address and port it saw filled in; some message types are answered from another port,
//! or from "another server", so the client can tell how its NAT maps and filters.
//!
//! Message (big endian): u32 type | u32 external port (server fills) | u32 external IPv4 (server fills) | u32 local IPv4 (client: its own external
//! address once known; server: its own address). Replies: 3 and 102 from the same server's paired port; 2 from the other server's paired port;
//! every other type from the socket that received it.
//!
//! One machine plays both servers: the primary server on the ports above, the secondary on the same ports + 1000. The Napple runtime gives the
//! secondary server's name its own virtual address and maps its ports to these (runtime/src/net/net_online.c), so the title sees two servers.

use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::Arc;

pub const DEFAULT_BASE: u16 = 10025;
/// The secondary server's ports are the primary's + this.
pub const SECONDARY_OFFSET: u16 = 1000;
/// The mapping-test ports MK8D's Pia sends to (on the primary server).
pub const MAPPING_PORTS: [u16; 2] = [33334, 33335];

/// The primary server's ports: `base`, `base + 100` (a pair), and the two mapping ports (a pair); then the same + SECONDARY_OFFSET.
pub fn ports(base: u16) -> [u16; 8] {
    let p = [base, base + 100, MAPPING_PORTS[0], MAPPING_PORTS[1]];
    [p[0], p[1], p[2], p[3], p[0] + SECONDARY_OFFSET, p[1] + SECONDARY_OFFSET, p[2] + SECONDARY_OFFSET, p[3] + SECONDARY_OFFSET]
}

/// Which socket answers message `kind` received on socket `on` (0..4 primary server, 4..8 secondary; pairs 0/1, 2/3, 4/5, 6/7).
pub fn reply_socket(kind: u32, on: usize) -> usize {
    match kind {
        3 | 102 => on ^ 1,
        2 => (on ^ 4) ^ 1,
        _ => on,
    }
}

/// The address to report for a client seen at `ip`. A client on the server's own machine is seen at a loopback address, which is no use as a
/// station address for peer-to-peer play: it is reported at the machine's LAN address instead (NAPPLE_ONLINE_PUBLIC_IP when set, else the
/// address of the interface the default route uses; nothing is sent to find it).
pub fn observed_ip(ip: IpAddr) -> IpAddr {
    if !ip.is_loopback() && !matches!(ip, IpAddr::V6(v) if v.to_ipv4_mapped().is_some_and(|m| m.is_loopback())) {
        return ip;
    }
    static LAN: std::sync::OnceLock<Option<IpAddr>> = std::sync::OnceLock::new();
    LAN.get_or_init(|| {
        if let Some(v) = std::env::var("NAPPLE_ONLINE_PUBLIC_IP").ok().and_then(|v| v.parse().ok()) {
            return Some(v);
        }
        let s = UdpSocket::bind("0.0.0.0:0").ok()?;
        s.connect("192.0.2.1:9").ok()?; // TEST-NET-1: a UDP connect only picks the route, no packet leaves
        let a = s.local_addr().ok()?.ip();
        (!a.is_unspecified()).then_some(a)
    })
    .unwrap_or(ip)
}

/// The reply to `msg` from `from`, sent by a server whose own address is `server_ip`. None for a message too short to be one.
pub fn reply(msg: &[u8], from: SocketAddr, server_ip: Ipv4Addr) -> Option<(u32, Vec<u8>)> {
    if msg.len() < 16 {
        return None;
    }
    let kind = u32::from_be_bytes([msg[0], msg[1], msg[2], msg[3]]);
    if !matches!(kind, 1..=5 | 101..=103) {
        return None; // not a NAT check message (MK8D also sends zero-filled and Pia packets to these ports): no answer
    }
    let ip = match observed_ip(from.ip()) {
        IpAddr::V4(v) => v,
        IpAddr::V6(v) => v.to_ipv4_mapped()?,
    };
    let mut r = msg.to_vec();
    r[4..8].copy_from_slice(&(from.port() as u32).to_be_bytes());
    r[8..12].copy_from_slice(&ip.octets());
    r[12..16].copy_from_slice(&server_ip.octets());
    Some((kind, r))
}

/// Binds the four sockets on `ip` and answers forever (one thread per socket).
pub fn start(ip: IpAddr, base: u16, verbose: bool) -> std::io::Result<()> {
    let socks: Vec<UdpSocket> = ports(base).iter().map(|p| UdpSocket::bind(SocketAddr::new(ip, *p))).collect::<Result<_, _>>()?;
    let socks = Arc::new(socks);
    let server_ip = match ip {
        IpAddr::V4(v) if !v.is_unspecified() => v,
        _ => Ipv4Addr::LOCALHOST,
    };
    for on in 0..8 {
        let socks = socks.clone();
        std::thread::spawn(move || {
            let mut buf = [0u8; 512];
            loop {
                let Ok((n, from)) = socks[on].recv_from(&mut buf) else { continue };
                if verbose {
                    println!("napple-online: udp {} <- {from}: {}", ports(base)[on], crate::crypto::hex(&buf[..n.min(128)]));
                }
                let Some((kind, r)) = reply(&buf[..n], from, server_ip) else { continue };
                let via = reply_socket(kind, on);
                if verbose {
                    println!("napple-online: nat check type {kind} from {from} on port {} -> answered from port {}", ports(base)[on], ports(base)[via]);
                }
                let _ = socks[via].send_to(&r, from);
            }
        });
    }
    println!("napple-online: NAT check on UDP {:?}", ports(base));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reply_fields() {
        let mut m = vec![0, 0, 0, 1];
        m.extend_from_slice(&[0; 12]);
        let (k, r) = reply(&m, "192.168.1.7:50123".parse().unwrap(), Ipv4Addr::new(10, 0, 0, 1)).unwrap();
        assert_eq!(k, 1);
        assert_eq!(&r[4..8], &50123u32.to_be_bytes());
        assert_eq!(&r[8..12], &[192, 168, 1, 7]);
        assert_eq!(&r[12..16], &[10, 0, 0, 1]);
        assert!(reply(&m[..15], "1.2.3.4:5".parse().unwrap(), Ipv4Addr::LOCALHOST).is_none());
        assert!(reply(&[0u8; 16], "1.2.3.4:5".parse().unwrap(), Ipv4Addr::LOCALHOST).is_none(), "type 0: no answer");
        let mut pia = vec![0x32, 0xab, 0x98, 0x64];
        pia.extend_from_slice(&[1; 40]);
        assert!(reply(&pia, "1.2.3.4:5".parse().unwrap(), Ipv4Addr::LOCALHOST).is_none(), "a Pia packet: no answer");
    }

    #[test]
    fn which_socket_answers() {
        assert_eq!(ports(10025), [10025, 10125, 33334, 33335, 11025, 11125, 34334, 34335]);
        // same socket
        for k in [1, 4, 5, 101, 103] {
            assert_eq!(reply_socket(k, 0), 0);
            assert_eq!(reply_socket(k, 6), 6);
        }
        // same server, paired port
        assert_eq!(reply_socket(3, 0), 1);
        assert_eq!(reply_socket(102, 1), 0);
        assert_eq!(reply_socket(3, 4), 5);
        // other server, paired port
        assert_eq!(reply_socket(2, 0), 5);
        assert_eq!(reply_socket(2, 1), 4);
        assert_eq!(reply_socket(2, 5), 0);
    }

    #[test]
    fn live_echo() {
        // the whole loop on localhost with a free base port
        let base = 20000 + (std::process::id() % 2000) as u16 * 4;
        if start("127.0.0.1".parse().unwrap(), base, false).is_err() {
            return; // ports taken on this machine: the pure tests above still cover the logic
        }
        let c = UdpSocket::bind("127.0.0.1:0").unwrap();
        c.set_read_timeout(Some(std::time::Duration::from_secs(2))).unwrap();
        let mut m = vec![0, 0, 0, 2];
        m.extend_from_slice(&[0; 12]);
        c.send_to(&m, ("127.0.0.1", base)).unwrap();
        let mut buf = [0u8; 64];
        let (n, from) = c.recv_from(&mut buf).unwrap();
        assert_eq!(n, 16);
        assert_eq!(from.port(), base + 100 + SECONDARY_OFFSET, "type 2 comes back from the other server's paired port");
        assert_eq!(&buf[4..8], &(c.local_addr().unwrap().port() as u32).to_be_bytes());
    }
}
