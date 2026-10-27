use super::io::{Slice, Source, Src, check, read_vec};
use super::pfs::{Fs, parse_hfs0};
use crate::domain::error::{Error, Result, container};
use std::sync::Arc;

const XCI_KEY_AREA: u64 = 0x1000;
/// How far past the end of the file a partition may be declared (nsz XCZ rounding).
const XCI_PAD: u64 = 0x10000;

/// `inner` followed by `pad` zero bytes.
struct ZeroPad {
    inner: Src,
    pad: u64,
}

impl Source for ZeroPad {
    fn size(&self) -> u64 {
        self.inner.size() + self.pad
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.size())?;
        let have = self.inner.size();
        let n = have.saturating_sub(offset).min(buf.len() as u64) as usize;
        if n > 0 {
            self.inner.read_at(offset, &mut buf[..n])?;
        }
        buf[n..].fill(0);
        Ok(())
    }
}

/// Returns XCI partitions (update / normal / secure / logo) as HFS0 file systems.
///
/// Header layout (public docs, hactool, nsz): `HEAD` at 0x100, root HFS0 offset at 0x130 and its
/// header size at 0x138 (the size of the HFS0 *header*, not of the partition), both relative to the
/// header. "Full" images carry a 0x1000-byte key area before the header (the header then sits at
/// 0x1000). The nsz tool's XCZ rounds partition sizes up past the end of the file, so the root is
/// read through a zero-padded view.
pub fn parse_xci(src: Src) -> Result<Vec<(String, Fs)>> {
    if src.size() < 0x200 {
        return container("file too small for XCI");
    }
    let at = |base: u64| -> Result<Vec<u8>> {
        if src.size() < base + 0x200 {
            return Ok(Vec::new());
        }
        read_vec(&*src, base + 0x100, 0x100)
    };
    let (base, head) = match at(0)? {
        h if h.starts_with(b"HEAD") => (0, h),
        _ => match at(XCI_KEY_AREA)? {
            h if h.starts_with(b"HEAD") => (XCI_KEY_AREA, h),
            _ => return container("not an XCI (missing HEAD)"),
        },
    };
    let off = u64::from_le_bytes(head[0x30..0x38].try_into().unwrap());
    let size = u64::from_le_bytes(head[0x38..0x40].try_into().unwrap());
    let start = base
        .checked_add(off)
        .ok_or_else(|| Error::Container("bad XCI root offset".into()))?;
    if !(0x10..=1 << 26).contains(&size) || start >= src.size() {
        return container("bad XCI root partition");
    }
    let tail = src.size() - start;
    let root = parse_hfs0(Arc::new(ZeroPad {
        inner: Slice::new(src, start, tail)?,
        pad: XCI_PAD,
    }))?;
    let mut out = Vec::new();
    for e in &root.entries {
        out.push((e.name.clone(), parse_hfs0(root.open(&e.name)?)?));
    }
    Ok(out)
}
