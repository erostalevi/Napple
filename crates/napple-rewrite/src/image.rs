//! Flat module image (the NSO as the loader maps it) plus the layout the rewriter appends to it.
//!
//! ```text
//! 0 ..text_end          code (the only region the rewriter edits)
//! ro_off.. / data_off.. read-only data / data
//! ..image_len           end of .bss (image_len = data_off + data + bss)
//! key_page_off          16 KiB key page (zero in the file; the loader fills it)
//! patch_off             64 KiB aligned patch area (rewriter output code)
//! ```
use crate::error::RewriteError;
use napple_core::infrastructure::nso::Nso;
use std::collections::BTreeMap;

pub const KEY_PAGE_SIZE: usize = 0x4000;
const PATCH_ALIGN: usize = 0x1_0000;
/// Largest module image accepted (guards allocations against hostile headers).
const MAX_IMAGE: usize = 1 << 30;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Layout {
    pub text_end: u32,
    pub ro_off: u32,
    pub data_off: u32,
    /// Length of text+rodata+data+bss, before the key page.
    pub image_len: usize,
    pub key_page_off: u32,
    pub patch_off: u32,
}

/// Module image with room for the key page; the patch area is appended by the caller.
#[derive(Debug, Clone)]
pub struct ModuleImage {
    /// `image_len + KEY_PAGE_SIZE` bytes: segments in place, everything else zero.
    pub bytes: Vec<u8>,
    pub layout: Layout,
    pub mod0: Option<Mod0>,
}

/// What the MOD0 header points at (all offsets module-relative).
#[derive(Debug, Clone)]
pub struct Mod0 {
    pub offset: u32,
    pub dynamic: u32,
    pub eh_frame_hdr: Option<u32>,
}

fn align(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}
fn i32_at(b: &[u8], o: usize) -> Option<i32> {
    u32_at(b, o).map(|v| v as i32)
}
fn u64_at(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

impl ModuleImage {
    pub fn from_nso(nso: &Nso) -> Result<Self, RewriteError> {
        if nso.text.mem_offset != 0 {
            return Err(RewriteError::Layout(format!(
                "text segment at {:#x}, expected 0",
                nso.text.mem_offset
            )));
        }
        let segs = [&nso.text, &nso.rodata, &nso.data];
        let mut end = 0usize;
        for s in segs {
            let (o, l) = (s.mem_offset as usize, s.data.len());
            if o < end || o % 4 != 0 {
                return Err(RewriteError::Layout(
                    "segments overlap or are misaligned".into(),
                ));
            }
            end = o + l;
        }
        let image_len = end
            .checked_add(nso.bss_size as usize)
            .filter(|n| *n <= MAX_IMAGE)
            .ok_or_else(|| RewriteError::Layout("module image too large".into()))?;
        let key_page_off = align(image_len, KEY_PAGE_SIZE);
        let mut bytes = vec![0u8; key_page_off + KEY_PAGE_SIZE];
        for s in segs {
            bytes[s.mem_offset as usize..s.mem_offset as usize + s.data.len()]
                .copy_from_slice(&s.data);
        }
        let patch_off = align(bytes.len(), PATCH_ALIGN);
        let layout = Layout {
            text_end: nso.text.data.len() as u32,
            ro_off: nso.rodata.mem_offset,
            data_off: nso.data.mem_offset,
            image_len,
            key_page_off: key_page_off as u32,
            patch_off: patch_off as u32,
        };
        let mod0 = Self::find_mod0(&bytes);
        Ok(Self {
            bytes,
            layout,
            mod0,
        })
    }

    fn find_mod0(img: &[u8]) -> Option<Mod0> {
        let at = u32_at(img, 4)? as usize;
        if img.get(at..at + 4)? != b"MOD0" || at + 24 > img.len() {
            return None;
        }
        let rel = |field: usize| {
            let v = i32_at(img, at + field)?;
            usize::try_from(at as i64 + v as i64).ok()
        };
        Some(Mod0 {
            offset: at as u32,
            dynamic: rel(4)? as u32,
            eh_frame_hdr: rel(16).filter(|o| *o < img.len()).map(|o| o as u32),
        })
    }

    pub fn word(&self, addr: u32) -> u32 {
        u32_at(&self.bytes, addr as usize).unwrap_or(0)
    }

    pub fn text(&self) -> &[u8] {
        &self.bytes[..self.layout.text_end as usize]
    }

    /// Function start addresses from `.eh_frame_hdr`'s binary search table (text only, sorted).
    pub fn function_starts(&self) -> Vec<u32> {
        let (Some(m), text_end) = (&self.mod0, self.layout.text_end) else {
            return Vec::new();
        };
        let Some(hdr) = m.eh_frame_hdr else {
            return Vec::new();
        };
        let img = &self.bytes;
        let hdr = hdr as usize;
        let (Some(&_ver), Some(&penc), Some(&cenc), Some(&tenc)) = (
            img.get(hdr),
            img.get(hdr + 1),
            img.get(hdr + 2),
            img.get(hdr + 3),
        ) else {
            return Vec::new();
        };
        // returns (value, size)
        let read = |o: usize, enc: u8| -> Option<(i64, usize)> {
            let (v, sz) = match enc & 0x0F {
                0x3 => (u32_at(img, o)? as i64, 4),
                0xB => (i32_at(img, o)? as i64, 4),
                0x4 | 0xC => (u64_at(img, o)? as i64, 8),
                _ => return None,
            };
            let v = match enc & 0x70 {
                0x00 => v,
                0x10 => v.checked_add(o as i64)?,
                0x30 => v.checked_add(hdr as i64)?,
                _ => return None,
            };
            Some((v, sz))
        };
        let mut o = hdr + 4;
        let Some((_, s)) = read(o, penc) else {
            return Vec::new();
        };
        o += s;
        let Some((count, s)) = read(o, cenc) else {
            return Vec::new();
        };
        o += s;
        let mut starts = Vec::new();
        for _ in 0..count.max(0) {
            let (Some((a, s1)), true) = (read(o, tenc), o < img.len()) else {
                break;
            };
            o += s1;
            let Some((_, s2)) = read(o, tenc) else { break };
            o += s2;
            if (0..text_end as i64).contains(&a) {
                starts.push(a as u32);
            }
        }
        starts.sort_unstable();
        starts.dedup();
        starts
    }

    /// Dynamic section tags (tag -> value), empty when the module has no usable MOD0.
    pub fn dynamic_tags(&self) -> BTreeMap<i64, u64> {
        let mut tags = BTreeMap::new();
        let Some(m) = &self.mod0 else { return tags };
        let mut o = m.dynamic as usize;
        while let (Some(t), Some(v)) = (u64_at(&self.bytes, o), u64_at(&self.bytes, o + 8)) {
            o += 16;
            if t == 0 || tags.len() > 256 {
                break;
            }
            tags.insert(t as i64, v);
        }
        tags
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::nso::Segment;

    pub(crate) fn nso(text: &[u32], ro: usize, data: usize, bss: u32) -> Nso {
        Nso {
            build_id: [0x11; 32],
            text: Segment {
                mem_offset: 0,
                data: text.iter().flat_map(|w| w.to_le_bytes()).collect(),
            },
            rodata: Segment {
                mem_offset: 0x1000,
                data: vec![0; ro],
            },
            data: Segment {
                mem_offset: 0x2000,
                data: vec![0; data],
            },
            bss_size: bss,
        }
    }

    #[test]
    fn layout_alignment() {
        let img = ModuleImage::from_nso(&nso(&[0xD503201F; 4], 16, 32, 0x100)).unwrap();
        let l = img.layout;
        assert_eq!(l.image_len, 0x2000 + 32 + 0x100);
        assert_eq!(l.key_page_off % 0x4000, 0);
        assert!(l.key_page_off as usize >= l.image_len);
        assert_eq!(l.patch_off % 0x10000, 0);
        assert_eq!(img.bytes.len(), l.key_page_off as usize + KEY_PAGE_SIZE);
        assert!(img.mod0.is_none());
    }

    #[test]
    fn rejects_bad_layouts() {
        let mut n = nso(&[0; 4], 16, 16, 0);
        n.rodata.mem_offset = 8; // overlaps text? text is 16 bytes
        assert!(ModuleImage::from_nso(&n).is_err());
        let mut n = nso(&[0; 4], 16, 16, 0);
        n.text.mem_offset = 0x1000;
        assert!(ModuleImage::from_nso(&n).is_err());
    }
}
