use crate::domain::error::{container, Result};

pub struct Ticket {
    pub rights_id: [u8; 16],
    pub encrypted_title_key: [u8; 16],
    pub key_type: u8, // 0 common, 1 personalized
    pub master_key_rev: u8,
}

pub fn parse_ticket(d: &[u8]) -> Result<Ticket> {
    if d.len() < 4 {
        return container("ticket too small");
    }
    let sig_pad = match u32::from_le_bytes(d[..4].try_into().unwrap()) {
        0x10000 | 0x10003 => 0x23C,
        0x10001 | 0x10004 => 0x13C,
        0x10002 | 0x10005 => 0x7C,
        _ => return container("unknown ticket signature type"),
    };
    let b = 4 + sig_pad;
    if d.len() < b + 0x170 {
        return container("ticket truncated");
    }
    Ok(Ticket {
        rights_id: d[b + 0x160..b + 0x170].try_into().unwrap(),
        encrypted_title_key: d[b + 0x40..b + 0x50].try_into().unwrap(),
        key_type: d[b + 0x141],
        master_key_rev: d[b + 0x145],
    })
}
