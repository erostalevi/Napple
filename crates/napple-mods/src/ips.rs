//! IPS and IPS32 patches, including RLE records, the EOF marker and the truncate extension.
use crate::util::{Result, err};

const MAX_EXPANDED: usize = 64 << 20;
const MAX_RECORDS: usize = 1 << 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IpsRecord {
    pub offset: u64,
    /// RLE records are expanded.
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct IpsPatch {
    /// `IPS32` (4-byte offsets, `EEOF`) instead of `PATCH` (3-byte offsets, `EOF`).
    pub wide: bool,
    pub records: Vec<IpsRecord>,
    /// Length after the EOF marker: the target is truncated to this size.
    pub truncate: Option<u64>,
}

fn be(b: &[u8]) -> u64 {
    b.iter().fold(0u64, |a, &x| (a << 8) | x as u64)
}

pub fn parse(b: &[u8]) -> Result<IpsPatch> {
    let (wide, mut pos) = if b.starts_with(b"PATCH") {
        (false, 5)
    } else if b.starts_with(b"IPS32") {
        (true, 5)
    } else {
        return err("not an IPS file (missing PATCH/IPS32 header)");
    };
    let ol = if wide { 4 } else { 3 };
    let eof: &[u8] = if wide { b"EEOF" } else { b"EOF" };
    let mut p = IpsPatch {
        wide,
        ..Default::default()
    };
    let mut expanded = 0usize;
    loop {
        if pos + ol > b.len() {
            return err("IPS: missing EOF marker");
        }
        let off = &b[pos..pos + ol];
        let rest = b.len() - pos - ol;
        if off == eof && (rest == 0 || rest == ol) {
            if rest == ol {
                p.truncate = Some(be(&b[pos + ol..]));
            }
            return Ok(p);
        }
        pos += ol;
        if pos + 2 > b.len() {
            return err("IPS: truncated record header");
        }
        let size = be(&b[pos..pos + 2]) as usize;
        pos += 2;
        let data = if size == 0 {
            if pos + 3 > b.len() {
                return err("IPS: truncated RLE record");
            }
            let n = be(&b[pos..pos + 2]) as usize;
            let v = b[pos + 2];
            pos += 3;
            if n == 0 {
                return err("IPS: RLE record of length 0");
            }
            vec![v; n]
        } else {
            if pos + size > b.len() {
                return err("IPS: truncated record data");
            }
            let d = b[pos..pos + size].to_vec();
            pos += size;
            d
        };
        expanded += data.len();
        if expanded > MAX_EXPANDED || p.records.len() >= MAX_RECORDS {
            return err("IPS: patch too large");
        }
        p.records.push(IpsRecord {
            offset: be(off),
            data,
        });
    }
}

/// Serialises a patch (long constant runs become RLE records).
pub fn write(p: &IpsPatch) -> Result<Vec<u8>> {
    let mut o = Vec::new();
    o.extend_from_slice(if p.wide { b"IPS32" } else { b"PATCH" });
    let ol = if p.wide { 4 } else { 3 };
    let max_off: u64 = if p.wide { u32::MAX as u64 } else { 0xFF_FFFF };
    for r in &p.records {
        if r.offset > max_off {
            return err(format!("IPS: offset {:#x} does not fit", r.offset));
        }
        if r.data.is_empty() {
            continue;
        }
        for chunk_start in (0..r.data.len()).step_by(0xFFFF) {
            let c = &r.data[chunk_start..(chunk_start + 0xFFFF).min(r.data.len())];
            let off = r.offset + chunk_start as u64;
            if off > max_off {
                return err("IPS: offset does not fit");
            }
            o.extend_from_slice(&off.to_be_bytes()[8 - ol..]);
            if c.len() >= 8 && c.iter().all(|&x| x == c[0]) {
                o.extend_from_slice(&[0, 0]);
                o.extend_from_slice(&(c.len() as u16).to_be_bytes());
                o.push(c[0]);
            } else {
                o.extend_from_slice(&(c.len() as u16).to_be_bytes());
                o.extend_from_slice(c);
            }
        }
    }
    o.extend_from_slice(if p.wide { b"EEOF" } else { b"EOF" });
    if let Some(t) = p.truncate {
        o.extend_from_slice(&t.to_be_bytes()[8 - ol..]);
    }
    Ok(o)
}

/// Applies to a growable buffer (a plain file). Writes past the end extend it with zeros.
pub fn apply(p: &IpsPatch, target: &mut Vec<u8>) -> Result<()> {
    for r in &p.records {
        let end = r.offset as usize + r.data.len();
        if end > (1 << 30) {
            return err("IPS: write beyond 1 GiB");
        }
        if end > target.len() {
            target.resize(end, 0);
        }
        target[r.offset as usize..end].copy_from_slice(&r.data);
    }
    if let Some(t) = p.truncate {
        target.truncate(t as usize);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_rle_eof_truncate() {
        let mut f = b"PATCH".to_vec();
        f.extend_from_slice(&[0, 0, 4, 0, 2, 0xAA, 0xBB]);
        f.extend_from_slice(&[0, 0, 8, 0, 0, 0, 5, 0x11]);
        f.extend_from_slice(b"EOF");
        f.extend_from_slice(&[0, 0, 10]);
        let p = parse(&f).unwrap();
        assert!(!p.wide);
        assert_eq!(p.records.len(), 2);
        assert_eq!(p.records[1].data, vec![0x11; 5]);
        assert_eq!(p.truncate, Some(10));
        let mut t = vec![0u8; 16];
        apply(&p, &mut t).unwrap();
        assert_eq!(t.len(), 10);
        assert_eq!(&t[4..6], &[0xAA, 0xBB]);
        assert_eq!(&t[8..10], &[0x11, 0x11]);
        assert_eq!(parse(&write(&p).unwrap()).unwrap(), p);
        let long = IpsPatch {
            records: vec![IpsRecord {
                offset: 1,
                data: vec![7; 100],
            }],
            ..Default::default()
        };
        let w = write(&long).unwrap();
        assert_eq!(w.len(), 5 + 3 + 2 + 3 + 3, "long runs are written as RLE");
        assert_eq!(parse(&w).unwrap(), long);
    }

    #[test]
    fn ips32() {
        let mut f = b"IPS32".to_vec();
        f.extend_from_slice(&[0, 1, 0, 0, 0, 1, 0x7F]);
        f.extend_from_slice(b"EEOF");
        let p = parse(&f).unwrap();
        assert!(p.wide);
        assert_eq!(p.records[0].offset, 0x10000);
        assert_eq!(write(&p).unwrap(), f);
    }

    #[test]
    fn eof_lookalike_offset_is_a_record() {
        // offset 0x454F46 ("EOF") followed by more data is a record, not the marker
        let mut f = b"PATCH".to_vec();
        f.extend_from_slice(b"EOF");
        f.extend_from_slice(&[0, 1, 0x55]);
        f.extend_from_slice(b"EOF");
        let p = parse(&f).unwrap();
        assert_eq!(p.records.len(), 1);
        assert_eq!(p.records[0].offset, 0x454F46);
    }

    #[test]
    fn malformed() {
        assert!(parse(b"").is_err());
        assert!(parse(b"PATCH").is_err());
        assert!(parse(b"PATCH\0\0\0\0\x05ab").is_err());
        assert!(parse(b"PATCH\0\0\0\0\0\0\0").is_err());
        assert!(parse(b"PATCHEOF").is_ok());
    }

    #[test]
    fn corpus_never_panics() {
        let mut s = 0x1234_5678_9abc_def1u64;
        let mut base = b"PATCH".to_vec();
        base.extend_from_slice(&[0, 0, 4, 0, 2, 0xAA, 0xBB, 0, 0, 8, 0, 0, 0, 5, 0x11]);
        base.extend_from_slice(b"EOF");
        for _ in 0..4000 {
            let mut m = base.clone();
            for _ in 0..(crate::util::xorshift(&mut s) % 4 + 1) {
                let i = crate::util::xorshift(&mut s) as usize % m.len();
                m[i] = crate::util::xorshift(&mut s) as u8;
            }
            let cut = crate::util::xorshift(&mut s) as usize % (m.len() + 1);
            let _ = parse(&m[..cut]);
            let _ = parse(&m);
        }
    }
}
