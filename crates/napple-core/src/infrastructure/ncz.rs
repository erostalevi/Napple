//! NCZ (NSZ / XCZ members): an NCA whose body past the first 0x4000 bytes is stored decrypted and
//! zstd-compressed (nsz format). `NczSource` presents the original NCA bytes again (raw header +
//! body re-encrypted with the per-section AES-CTR key/counter stored in the NCZ), so `Nca::open`
//! needs no changes.
//!
//! Layout: `[0x4000 raw NCA header+gap][NCZSECTN count (offset size crypto_type pad key[16] ctr[16])*]`
//! then either one zstd stream, or `NCZBLOCK` (block-compressed, random access) + blocks.
//! Streaming form is decoded forward only: a backward seek restarts the decoder, a forward seek
//! discards. The block form decodes one block per read (the last block is cached).
use super::io::{Source, Src, check, read_vec};
use crate::domain::error::{Error, Result, container};
use aes::Aes128;
use aes::cipher::{KeyIvInit, StreamCipher, StreamCipherSeek, generic_array::GenericArray};
use std::io::Read;
use std::sync::{Arc, Mutex};

pub const NCZ_HEADER: u64 = 0x4000;
const MAX_SECTIONS: u64 = 1 << 16;
const MAX_BLOCKS: u64 = 1 << 24;

#[derive(Debug, Clone)]
struct NczSection {
    offset: u64,
    size: u64,
    crypto_type: u64,
    key: [u8; 16],
    counter: [u8; 16],
}

struct Block {
    bs: u64,
    /// Absolute offsets in the NCZ of each block, plus the end as the last element.
    offs: Vec<u64>,
}

/// Forward-only zstd decoder over `src[start..]`.
struct StreamState {
    dec: Option<zstd::stream::Decoder<'static, std::io::BufReader<SrcReader>>>,
    pos: u64,
}

struct SrcReader {
    src: Src,
    pos: u64,
}

impl Read for SrcReader {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = (self.src.size().saturating_sub(self.pos)).min(buf.len() as u64) as usize;
        if n == 0 {
            return Ok(0);
        }
        self.src
            .read_at(self.pos, &mut buf[..n])
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        self.pos += n as u64;
        Ok(n)
    }
}

enum Mode {
    Stream {
        data_start: u64,
        st: Mutex<StreamState>,
    },
    Blocks {
        blk: Block,
        cache: Mutex<Option<(u64, Vec<u8>)>>,
    },
}

pub struct NczSource {
    src: Src,
    sections: Vec<NczSection>,
    /// NCA offset where the compressed body begins (first section offset).
    body_start: u64,
    size: u64,
    mode: Mode,
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

pub fn is_ncz(src: &Src) -> bool {
    src.size() >= NCZ_HEADER + 16 && read_vec(&**src, NCZ_HEADER, 8).is_ok_and(|m| m == b"NCZSECTN")
}

impl NczSource {
    pub fn open(src: Src) -> Result<Src> {
        if !is_ncz(&src) {
            return container("not an NCZ (missing NCZSECTN)");
        }
        let count = u64le(&read_vec(&*src, NCZ_HEADER + 8, 8)?, 0);
        if count > MAX_SECTIONS {
            return container("implausible NCZ section count");
        }
        let tbl_off = NCZ_HEADER + 16;
        let mut sections = Vec::with_capacity(count as usize);
        // Entry is offset(8) size(8) type(8) pad(8) key(16) counter(16) = 0x40 bytes.
        let raw = read_vec(&*src, tbl_off, (count * 0x40) as usize)?;
        for i in 0..count as usize {
            let e = &raw[i * 0x40..];
            sections.push(NczSection {
                offset: u64le(e, 0),
                size: u64le(e, 8),
                crypto_type: u64le(e, 16),
                key: e[0x20..0x30].try_into().unwrap(),
                counter: e[0x30..0x40].try_into().unwrap(),
            });
        }
        let mut end = NCZ_HEADER;
        for s in &sections {
            if s.offset < NCZ_HEADER || s.offset.checked_add(s.size).is_none() {
                return container("bad NCZ section range");
            }
            end = end.max(s.offset + s.size);
        }
        let body_start = sections.first().map_or(NCZ_HEADER, |s| s.offset);
        let after = tbl_off + count * 0x40;
        let mode = if src.size() >= after + 8 && read_vec(&*src, after, 8)? == b"NCZBLOCK" {
            let h = read_vec(&*src, after, 0x18)?;
            let (bsx, nblk, dsize) = (h[0x0b] as u32, u32le(&h, 0x0c) as u64, u64le(&h, 0x10));
            if !(14..=32).contains(&bsx) || nblk > MAX_BLOCKS {
                return container("implausible NCZ block header");
            }
            if dsize != end - body_start {
                return container("NCZ block size disagrees with sections");
            }
            let bs = 1u64 << bsx;
            if dsize.div_ceil(bs) != nblk {
                return container("NCZ block count mismatch");
            }
            let sizes = read_vec(&*src, after + 0x18, nblk as usize * 4)?;
            let mut offs = Vec::with_capacity(nblk as usize + 1);
            let mut o = after + 0x18 + nblk * 4;
            for i in 0..nblk as usize {
                offs.push(o);
                o += u32le(&sizes, i * 4) as u64;
            }
            offs.push(o);
            if o > src.size() {
                return container("NCZ blocks extend past file");
            }
            Mode::Blocks {
                blk: Block { bs, offs },
                cache: Mutex::new(None),
            }
        } else {
            Mode::Stream {
                data_start: after,
                st: Mutex::new(StreamState { dec: None, pos: 0 }),
            }
        };
        Ok(Arc::new(Self {
            src,
            sections,
            body_start,
            size: end,
            mode,
        }))
    }

    /// Decompressed (decrypted) body bytes at `off` relative to `body_start`.
    fn read_plain(&self, off: u64, buf: &mut [u8]) -> Result<()> {
        match &self.mode {
            Mode::Stream { data_start, st } => {
                let mut st = st.lock().unwrap();
                if st.dec.is_none() || off < st.pos {
                    let r = SrcReader {
                        src: self.src.clone(),
                        pos: *data_start,
                    };
                    st.dec = Some(zstd::stream::Decoder::new(r)?);
                    st.pos = 0;
                }
                let mut skip = off - st.pos;
                let mut sink = [0u8; 16384];
                let dec = st.dec.as_mut().unwrap();
                while skip > 0 {
                    let n = dec.read(&mut sink[..skip.min(16384) as usize])?;
                    if n == 0 {
                        return container("NCZ stream ended early");
                    }
                    skip -= n as u64;
                }
                dec.read_exact(buf)
                    .map_err(|_| Error::Container("NCZ stream ended early".into()))?;
                st.pos = off + buf.len() as u64;
                Ok(())
            }
            Mode::Blocks { blk, cache } => {
                let total = self.size - self.body_start;
                let (mut done, mut cur) = (0usize, off);
                let mut c = cache.lock().unwrap();
                while done < buf.len() {
                    let i = cur / blk.bs;
                    if c.as_ref().is_none_or(|(ci, _)| *ci != i) {
                        let (a, b) = (blk.offs[i as usize], blk.offs[i as usize + 1]);
                        let dlen = blk.bs.min(total - i * blk.bs) as usize;
                        let raw = read_vec(&*self.src, a, (b - a) as usize)?;
                        let data = if raw.len() == dlen {
                            raw
                        } else {
                            zstd::bulk::decompress(&raw, dlen)
                                .map_err(|e| Error::Container(format!("NCZ block: {e}")))?
                        };
                        if data.len() != dlen {
                            return container("NCZ block has wrong size");
                        }
                        *c = Some((i, data));
                    }
                    let data = &c.as_ref().unwrap().1;
                    let within = (cur - i * blk.bs) as usize;
                    let n = (data.len() - within).min(buf.len() - done);
                    buf[done..done + n].copy_from_slice(&data[within..within + n]);
                    done += n;
                    cur += n as u64;
                }
                Ok(())
            }
        }
    }
}

impl Source for NczSource {
    fn size(&self) -> u64 {
        self.size
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.size)?;
        let end = offset + buf.len() as u64;
        // Raw header part (and any gap before the first section).
        if offset < self.body_start {
            let n = (self.body_start.min(end) - offset) as usize;
            self.src.read_at(offset, &mut buf[..n])?;
        }
        let from = offset.max(self.body_start);
        if from >= end {
            return Ok(());
        }
        let body = &mut buf[(from - offset) as usize..];
        self.read_plain(from - self.body_start, body)?;
        for s in &self.sections {
            let (a, b) = (from.max(s.offset), end.min(s.offset + s.size));
            if a >= b || !matches!(s.crypto_type, 3 | 4) {
                continue;
            }
            let mut c = ctr::Ctr128BE::<Aes128>::new(
                GenericArray::from_slice(&s.key),
                GenericArray::from_slice(&s.counter),
            );
            c.seek(a);
            c.apply_keystream(&mut body[(a - from) as usize..(b - from) as usize]);
        }
        Ok(())
    }
}

/// `x.ncz` -> `x.nca` (so `.cnmt.ncz` becomes `.cnmt.nca`); other names unchanged.
pub fn nca_name(name: &str) -> String {
    match name.strip_suffix(".ncz") {
        Some(stem) => format!("{stem}.nca"),
        None => name.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::infrastructure::io::BytesSource;

    const K: [u8; 16] = [0x11; 16];
    const CTR: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0];

    fn plain_body(n: usize) -> Vec<u8> {
        // First half compressible, second half xorshift noise (exercises raw-stored blocks).
        let mut x = 0x9E37_79B9u32;
        (0..n)
            .map(|i| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                if i < n / 2 {
                    ((i * 31 + i / 251) % 251) as u8
                } else {
                    x as u8
                }
            })
            .collect()
    }

    fn encrypt(buf: &mut [u8], key: &[u8; 16], ctr: &[u8; 16], abs: u64) {
        let mut c = ctr::Ctr128BE::<Aes128>::new(
            GenericArray::from_slice(key),
            GenericArray::from_slice(ctr),
        );
        c.seek(abs);
        c.apply_keystream(buf);
    }

    /// Returns (ncz bytes, original nca bytes). Section 0 is CTR, section 1 is unencrypted.
    pub(crate) fn synth(body_len: usize, block_exp: Option<u8>) -> (Vec<u8>, Vec<u8>) {
        let header: Vec<u8> = (0..NCZ_HEADER as usize).map(|i| (i % 253) as u8).collect();
        let plain = plain_body(body_len);
        let split = (body_len / 3) & !15;
        let mut nca = header.clone();
        let mut enc = plain.clone();
        encrypt(&mut enc[..split], &K, &CTR, NCZ_HEADER);
        nca.extend(&enc);
        let mut ncz = header;
        ncz.extend(b"NCZSECTN");
        ncz.extend(2u64.to_le_bytes());
        for (off, size, ty, key, ctr) in [
            (NCZ_HEADER, split as u64, 3u64, K, CTR),
            (
                NCZ_HEADER + split as u64,
                (body_len - split) as u64,
                1,
                [0; 16],
                [0; 16],
            ),
        ] {
            ncz.extend(off.to_le_bytes());
            ncz.extend(size.to_le_bytes());
            ncz.extend(ty.to_le_bytes());
            ncz.extend(0u64.to_le_bytes());
            ncz.extend(key);
            ncz.extend(ctr);
        }
        match block_exp {
            None => ncz.extend(zstd::encode_all(&plain[..], 3).unwrap()),
            Some(x) => {
                let bs = 1usize << x;
                let blocks: Vec<Vec<u8>> = plain
                    .chunks(bs)
                    .map(|c| {
                        let z = zstd::bulk::compress(c, 3).unwrap();
                        if z.len() >= c.len() { c.to_vec() } else { z }
                    })
                    .collect();
                ncz.extend(b"NCZBLOCK");
                ncz.extend([2u8, 1, 0, x]);
                ncz.extend((blocks.len() as u32).to_le_bytes());
                ncz.extend((body_len as u64).to_le_bytes());
                for b in &blocks {
                    ncz.extend((b.len() as u32).to_le_bytes());
                }
                for b in &blocks {
                    ncz.extend(b);
                }
            }
        }
        (ncz, nca)
    }

    fn check_all(ncz: Vec<u8>, nca: &[u8]) {
        let s = NczSource::open(Arc::new(BytesSource(ncz))).unwrap();
        assert_eq!(s.size(), nca.len() as u64);
        let mut all = vec![0u8; nca.len()];
        s.read_at(0, &mut all).unwrap();
        assert!(all == nca, "full read differs");
        // Unaligned, backward, crossing header/section/block edges.
        for (o, l) in [
            (0x3ff0u64, 0x40usize),
            (0x4001, 17),
            (nca.len() as u64 - 5, 5),
            (0x4000 + 16383, 20000),
            (10, 3),
            (0x4000 + 3333, 1),
        ] {
            if o as usize + l > nca.len() {
                continue;
            }
            let mut b = vec![0u8; l];
            s.read_at(o, &mut b).unwrap();
            assert_eq!(
                &b[..],
                &nca[o as usize..o as usize + l],
                "read at {o:#x}+{l}"
            );
        }
        assert!(s.read_at(nca.len() as u64 - 1, &mut [0u8; 2]).is_err());
    }

    #[test]
    fn round_trip_stream() {
        let (z, n) = synth(100_003, None);
        check_all(z, &n);
    }

    #[test]
    fn round_trip_blocks() {
        for exp in [14u8, 16] {
            let (z, n) = synth(100_003, Some(exp));
            check_all(z, &n);
        }
    }

    #[test]
    fn incompressible_block_stored_raw() {
        // Noise blocks do not shrink and are stored raw by the encoder above (len == block size).
        let (z, n) = synth(1 << 16, Some(14));
        let raw_blocks = z[0x4000 + 16 + 2 * 0x40 + 0x18..][..16]
            .chunks(4)
            .filter(|c| u32::from_le_bytes((*c).try_into().unwrap()) == 1 << 14)
            .count();
        assert!(raw_blocks >= 1);
        check_all(z, &n);
    }

    #[test]
    fn hostile_inputs_error_not_panic() {
        let (z, _) = synth(40_000, None);
        for cut in [0usize, 100, 0x4000, 0x4010, 0x4040, 0x4080, z.len() - 100] {
            let s = NczSource::open(Arc::new(BytesSource(z[..cut].to_vec())));
            if let Ok(s) = s {
                let mut b = vec![0u8; s.size() as usize];
                let _ = s.read_at(0, &mut b); // truncated stream: Err, never panic
            }
        }
        let mut bad = z.clone();
        bad[0x4008..0x4010].copy_from_slice(&u64::MAX.to_le_bytes());
        assert!(NczSource::open(Arc::new(BytesSource(bad))).is_err());
        let mut bad = z;
        bad[0x4010..0x4018].copy_from_slice(&0u64.to_le_bytes()); // section offset below header
        assert!(NczSource::open(Arc::new(BytesSource(bad))).is_err());
    }

    #[test]
    fn names() {
        assert_eq!(nca_name("ab.cnmt.ncz"), "ab.cnmt.nca");
        assert_eq!(nca_name("ab.nca"), "ab.nca");
    }
}
