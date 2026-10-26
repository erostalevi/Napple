//! CNMT (content meta) parsing: identifies Application / Patch (update) / AddOnContent (DLC) packs.
use crate::domain::error::{Result, container};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TitleKind {
    Application,
    Patch,
    AddOn,
    Delta,
    Other(u8),
}

#[derive(Debug, Clone)]
pub struct ContentRecord {
    pub id: [u8; 16],
    pub size: u64,
    pub ctype: u8, // 0 meta, 1 program, 2 data, 3 control, 4 html, 5 legal, 6 delta
}

impl ContentRecord {
    pub fn nca_name(&self) -> String {
        self.id
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>()
            + ".nca"
    }
}

#[derive(Debug, Clone)]
pub struct Cnmt {
    pub title_id: u64,
    pub version: u32,
    pub kind: TitleKind,
    /// Base application this pack belongs to (== title_id for the application itself).
    pub application_id: u64,
    pub required_system_version: u32,
    pub required_application_version: u32,
    pub contents: Vec<ContentRecord>,
}

fn u16le(b: &[u8], o: usize) -> usize {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap()) as usize
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

pub fn parse_cnmt(d: &[u8]) -> Result<Cnmt> {
    if d.len() < 0x20 {
        return container("cnmt too small");
    }
    let title_id = u64le(d, 0);
    let version = u32le(d, 8);
    let kind = match d[0xC] {
        0x80 => TitleKind::Application,
        0x81 => TitleKind::Patch,
        0x82 => TitleKind::AddOn,
        0x83 => TitleKind::Delta,
        n => TitleKind::Other(n),
    };
    let (table_off, count) = (u16le(d, 0xE), u16le(d, 0x10));
    let ext = &d[0x20..];
    // Extended header layouts (first fields only; later fields are not needed here).
    let (application_id, rsv, rav) = match kind {
        TitleKind::Application if ext.len() >= 16 => (title_id, u32le(ext, 8), u32le(ext, 12)),
        TitleKind::Patch if ext.len() >= 16 => (u64le(ext, 0), u32le(ext, 8), 0),
        TitleKind::AddOn if ext.len() >= 12 => (u64le(ext, 0), 0, u32le(ext, 8)),
        _ => (title_id, 0, 0),
    };
    let start = 0x20 + table_off;
    if count > 4096 || start.checked_add(count * 0x38).is_none_or(|e| e > d.len()) {
        return container("cnmt content table out of range");
    }
    let contents = (0..count)
        .map(|i| {
            let r = &d[start + i * 0x38..start + (i + 1) * 0x38];
            let mut size = [0u8; 8];
            size[..6].copy_from_slice(&r[0x30..0x36]);
            ContentRecord {
                id: r[0x20..0x30].try_into().unwrap(),
                size: u64::from_le_bytes(size),
                ctype: r[0x36],
            }
        })
        .collect();
    Ok(Cnmt {
        title_id,
        version,
        kind,
        application_id,
        required_system_version: rsv,
        required_application_version: rav,
        contents,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cnmt(kind: u8, title: u64, app: u64, version: u32) -> Vec<u8> {
        let mut d = vec![0u8; 0x20 + 0x10 + 0x38];
        d[..8].copy_from_slice(&title.to_le_bytes());
        d[8..12].copy_from_slice(&version.to_le_bytes());
        d[0xC] = kind;
        d[0xE..0x10].copy_from_slice(&0x10u16.to_le_bytes()); // table offset after 0x10 ext header
        d[0x10..0x12].copy_from_slice(&1u16.to_le_bytes());
        d[0x20..0x28].copy_from_slice(&app.to_le_bytes());
        let r = 0x30;
        d[r + 0x20..r + 0x30].copy_from_slice(&[0xAA; 16]);
        d[r + 0x30..r + 0x33].copy_from_slice(&[0x00, 0x10, 0x00]); // size 0x1000
        d[r + 0x36] = 1;
        d
    }

    #[test]
    fn kinds_and_contents() {
        let p = parse_cnmt(&cnmt(
            0x81,
            0x0100_0000_0000_0800,
            0x0100_0000_0000_0000,
            65536,
        ))
        .unwrap();
        assert_eq!(
            (p.kind, p.application_id, p.version),
            (TitleKind::Patch, 0x0100_0000_0000_0000, 65536)
        );
        assert_eq!((p.contents[0].size, p.contents[0].ctype), (0x1000, 1));
        assert_eq!(p.contents[0].nca_name(), "a".repeat(32) + ".nca");
        let d = parse_cnmt(&cnmt(0x82, 0x0100_0000_0000_1001, 0x0100_0000_0000_0000, 0)).unwrap();
        assert_eq!(
            (d.kind, d.application_id),
            (TitleKind::AddOn, 0x0100_0000_0000_0000)
        );
        let a = parse_cnmt(&cnmt(0x80, 0x0100_0000_0000_0000, 0, 0)).unwrap();
        assert_eq!(a.application_id, a.title_id);
    }

    #[test]
    fn hostile_tables_rejected() {
        let mut d = cnmt(0x80, 1, 0, 0);
        d[0x10..0x12].copy_from_slice(&5000u16.to_le_bytes());
        assert!(parse_cnmt(&d).is_err());
        assert!(parse_cnmt(&[0u8; 8]).is_err());
    }
}
