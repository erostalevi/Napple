//! Synthetic fixtures only (random keys, no game data): NSP containers with NCAs, NSOs, a BKTR update and DLC.
//! They prove internal consistency of the pipeline, NOT compatibility with retail files.
#![allow(dead_code)]
use aes::Aes128;
use aes::cipher::{
    BlockEncrypt, KeyInit, KeyIvInit, StreamCipher, StreamCipherSeek, generic_array::GenericArray,
};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use xts_mode::Xts128;

pub const HEADER_KEY: [u8; 32] = [7; 32];
pub const KAK: [u8; 16] = [9; 16];
pub const TKEK: [u8; 16] = [5; 16];
pub const CTR_KEY: [u8; 16] = [0x42; 16];
pub const APP_ID: u64 = 0x0100_0000_0000_2000;

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn keys_text() -> String {
    format!(
        "header_key = {}\nkey_area_key_application_02 = {}\ntitlekek_02 = {}\n",
        hex(&HEADER_KEY),
        hex(&KAK),
        hex(&TKEK)
    )
}

fn ecb_enc(key: &[u8; 16], data: &mut [u8]) {
    let c = Aes128::new(GenericArray::from_slice(key));
    for b in data.as_chunks_mut::<16>().0 {
        c.encrypt_block(GenericArray::from_mut_slice(b));
    }
}

pub fn pad(v: &mut Vec<u8>, to: usize) {
    while !v.len().is_multiple_of(to) {
        v.push(0);
    }
}

pub fn pfs0(files: &[(&str, &[u8])]) -> Vec<u8> {
    let (mut strtab, mut ents, mut off) = (Vec::new(), Vec::new(), 0u64);
    for (n, d) in files {
        let mut e = vec![0u8; 24];
        e[..8].copy_from_slice(&off.to_le_bytes());
        e[8..16].copy_from_slice(&(d.len() as u64).to_le_bytes());
        e[16..20].copy_from_slice(&(strtab.len() as u32).to_le_bytes());
        ents.extend(e);
        strtab.extend(n.as_bytes());
        strtab.push(0);
        off += d.len() as u64;
    }
    let mut out = b"PFS0".to_vec();
    out.extend((files.len() as u32).to_le_bytes());
    out.extend((strtab.len() as u32).to_le_bytes());
    out.extend([0u8; 4]);
    out.extend(ents);
    out.extend(strtab);
    for (_, d) in files {
        out.extend(*d);
    }
    out
}

/// Flat RomFS image (all files in the root directory).
pub fn romfs_flat(files: &[(&str, &[u8])]) -> Vec<u8> {
    let none = u32::MAX;
    let mut dirs = Vec::new();
    let first_file = 0u32;
    for v in [0u32, none, none, first_file, none, 0] {
        dirs.extend(v.to_le_bytes());
    }
    let (mut fmeta, mut data) = (Vec::new(), Vec::new());
    for (i, (name, d)) in files.iter().enumerate() {
        let nl = name.len();
        let entry_len = 0x20 + ((nl + 3) & !3);
        let sibling = if i + 1 < files.len() {
            (fmeta.len() + entry_len) as u32
        } else {
            none
        };
        for v in [0u32, sibling] {
            fmeta.extend(v.to_le_bytes());
        }
        fmeta.extend((data.len() as u64).to_le_bytes());
        fmeta.extend((d.len() as u64).to_le_bytes());
        for v in [none, nl as u32] {
            fmeta.extend(v.to_le_bytes());
        }
        fmeta.extend(name.as_bytes());
        while fmeta.len() % 4 != 0 {
            fmeta.push(0);
        }
        data.extend(*d);
        while data.len() % 8 != 0 {
            data.push(0);
        }
    }
    let dm = 0x50u64;
    let fm = dm + dirs.len() as u64;
    let data_off = (fm + fmeta.len() as u64 + 0x1F) & !0x1F;
    let mut h = Vec::new();
    for v in [
        0x50u64,
        0,
        0,
        dm,
        dirs.len() as u64,
        0,
        0,
        fm,
        fmeta.len() as u64,
        data_off,
    ] {
        h.extend(v.to_le_bytes());
    }
    h.extend(dirs);
    h.extend(fmeta);
    h.resize(data_off as usize, 0);
    h.extend(data);
    h
}

/// RomFS image with real directories. Paths are `a/b/c.html`; every parent directory is created,
/// in first-seen order (retail HtmlDocument images are nested like this).
pub fn romfs_tree(files: &[(&str, &[u8])]) -> Vec<u8> {
    const NONE: u32 = u32::MAX;
    struct Dir {
        name: String,
        parent: u32,
        sibling: u32,
        child_dir: u32,
        child_file: u32,
    }
    let mut dirs = vec![Dir {
        name: String::new(),
        parent: 0,
        sibling: NONE,
        child_dir: NONE,
        child_file: NONE,
    }];
    // (name, parent dir index, data offset, size)
    let mut fl: Vec<(String, usize, u64, u64)> = Vec::new();
    let mut data = Vec::new();
    for (path, d) in files {
        let parts: Vec<&str> = path.split('/').collect();
        let mut cur = 0usize;
        for part in &parts[..parts.len() - 1] {
            let found = (0..dirs.len())
                .find(|&i| dirs[i].parent == cur as u32 && i != 0 && dirs[i].name == *part);
            cur = match found {
                Some(i) => i,
                None => {
                    dirs.push(Dir {
                        name: part.to_string(),
                        parent: cur as u32,
                        sibling: NONE,
                        child_dir: NONE,
                        child_file: NONE,
                    });
                    dirs.len() - 1
                }
            };
        }
        fl.push((
            parts[parts.len() - 1].to_string(),
            cur,
            data.len() as u64,
            d.len() as u64,
        ));
        data.extend(*d);
        while data.len() % 8 != 0 {
            data.push(0);
        }
    }
    let dir_len = |d: &Dir| 0x18 + ((d.name.len() + 3) & !3);
    let mut dir_off = Vec::new();
    let mut o = 0usize;
    for d in &dirs {
        dir_off.push(o as u32);
        o += dir_len(d);
    }
    let dir_total = o;
    let mut file_off = Vec::new();
    let mut o = 0usize;
    for (n, ..) in &fl {
        file_off.push(o as u32);
        o += 0x20 + ((n.len() + 3) & !3);
    }
    let file_total = o;
    // sibling / child links, in creation order
    for i in 1..dirs.len() {
        let par = dirs[i].parent as usize;
        match (0..i)
            .rev()
            .find(|&j| j != 0 && dirs[j].parent as usize == par && dirs[j].sibling == NONE)
        {
            Some(j) => dirs[j].sibling = dir_off[i],
            None => dirs[par].child_dir = dir_off[i],
        }
    }
    let mut fsib = vec![NONE; fl.len()];
    for i in 0..fl.len() {
        let par = fl[i].1;
        match (0..i).rev().find(|&j| fl[j].1 == par && fsib[j] == NONE) {
            Some(j) => fsib[j] = file_off[i],
            None => dirs[par].child_file = file_off[i],
        }
    }
    let mut dt = Vec::new();
    for d in &dirs {
        for v in [
            dir_off[d.parent as usize],
            d.sibling,
            d.child_dir,
            d.child_file,
            NONE,
            d.name.len() as u32,
        ] {
            dt.extend(v.to_le_bytes());
        }
        dt.extend(d.name.as_bytes());
        while dt.len() % 4 != 0 {
            dt.push(0);
        }
    }
    assert_eq!(dt.len(), dir_total);
    let mut ft = Vec::new();
    for (i, (n, par, off, size)) in fl.iter().enumerate() {
        ft.extend(dir_off[*par].to_le_bytes());
        ft.extend(fsib[i].to_le_bytes());
        ft.extend(off.to_le_bytes());
        ft.extend(size.to_le_bytes());
        ft.extend(NONE.to_le_bytes());
        ft.extend((n.len() as u32).to_le_bytes());
        ft.extend(n.as_bytes());
        while ft.len() % 4 != 0 {
            ft.push(0);
        }
    }
    assert_eq!(ft.len(), file_total);
    let dm = 0x50u64;
    let fm = dm + dt.len() as u64;
    let data_off = (fm + ft.len() as u64 + 0x1F) & !0x1F;
    let mut h = Vec::new();
    for v in [
        0x50u64,
        0,
        0,
        dm,
        dt.len() as u64,
        0,
        0,
        fm,
        ft.len() as u64,
        data_off,
    ] {
        h.extend(v.to_le_bytes());
    }
    h.extend(dt);
    h.extend(ft);
    h.resize(data_off as usize, 0);
    h.extend(data);
    h
}

pub struct BktrSpec {
    /// Plain patch storage (`[0, len)`), a multiple of 0x200.
    pub patch: Vec<u8>,
    /// (virtual offset, physical offset, source 0 base / 1 patch)
    pub relocs: Vec<(u64, u64, u32)>,
    /// (offset in patch storage, generation)
    pub subs: Vec<(u64, u32)>,
    /// Total virtual size and the IVFC data level (offset, size) inside it.
    pub total: u64,
    pub level: (u64, u64),
    /// Declare one relocation entry more than the table holds.
    pub corrupt: bool,
}

pub enum Sec {
    Pfs0(Vec<u8>),
    Rom(Vec<u8>),
    Bktr(BktrSpec),
}

fn ctr_apply(buf: &mut [u8], hi: [u8; 8], generation: Option<u32>, abs: u64) {
    let mut iv = [0u8; 16];
    iv[..8].copy_from_slice(&hi);
    if let Some(g) = generation {
        iv[4..8].copy_from_slice(&g.to_be_bytes());
    }
    let mut c = ctr::Ctr128BE::<Aes128>::new(
        GenericArray::from_slice(&CTR_KEY),
        GenericArray::from_slice(&iv),
    );
    c.seek(abs);
    c.apply_keystream(buf);
}

fn tree(entries: &[Vec<u8>], entry_size: usize, end: u64) -> Vec<u8> {
    let per_set = (0x4000 - 0x10) / entry_size;
    let sets = entries.chunks(per_set).count();
    let mut t = vec![0u8; 0x4000 * (1 + sets)];
    t[4..8].copy_from_slice(&(sets as u32).to_le_bytes());
    t[8..16].copy_from_slice(&end.to_le_bytes());
    for (k, chunk) in entries.chunks(per_set).enumerate() {
        let at = 0x4000 * (1 + k);
        t[at..at + 4].copy_from_slice(&(k as u32).to_le_bytes());
        t[at + 4..at + 8].copy_from_slice(&(chunk.len() as u32).to_le_bytes());
        for (i, e) in chunk.iter().enumerate() {
            t[at + 0x10 + i * entry_size..at + 0x10 + (i + 1) * entry_size].copy_from_slice(e);
        }
    }
    t
}

fn ivfc_hash_data(level_off: u64, level_size: u64) -> Vec<u8> {
    let mut hd = vec![0u8; 0xF8];
    hd[..4].copy_from_slice(b"IVFC");
    let l = 0x10 + 5 * 0x18;
    hd[l..l + 8].copy_from_slice(&level_off.to_le_bytes());
    hd[l + 8..l + 16].copy_from_slice(&level_size.to_le_bytes());
    hd
}

pub fn build_nca(program_id: u64, ctype: u8, secs: Vec<Sec>) -> Vec<u8> {
    let mut h = vec![0u8; 0xC00];
    h[0x200..0x204].copy_from_slice(b"NCA3");
    h[0x205] = ctype;
    h[0x220] = 3;
    h[0x210..0x218].copy_from_slice(&program_id.to_le_bytes());
    let mut body: Vec<u8> = Vec::new();
    for (i, s) in secs.into_iter().enumerate() {
        let start = 0xC00 + body.len();
        let ctr_hdr = [1u8, 2, 3, 4, 5, 6, 7, 8 + i as u8];
        let mut hi = ctr_hdr;
        hi.reverse();
        let mut fh = vec![0u8; 0x200];
        fh[3] = 2;
        fh[0x140..0x148].copy_from_slice(&ctr_hdr);
        let mut data: Vec<u8>;
        match s {
            Sec::Pfs0(p) => {
                let table = Sha256::digest(&p).to_vec();
                let mut hd = vec![0u8; 0xF8];
                hd[..0x20].copy_from_slice(&Sha256::digest(&table));
                hd[0x20..0x24].copy_from_slice(&(p.len() as u32).to_le_bytes());
                hd[0x24] = 2;
                hd[0x30..0x38].copy_from_slice(&32u64.to_le_bytes());
                hd[0x38..0x40].copy_from_slice(&0x200u64.to_le_bytes());
                hd[0x40..0x48].copy_from_slice(&(p.len() as u64).to_le_bytes());
                data = table;
                pad(&mut data, 0x200);
                data.extend(p);
                pad(&mut data, 0x200);
                fh[2] = 1;
                fh[4] = 3;
                fh[8..0x100].copy_from_slice(&hd);
                ctr_apply(&mut data, hi, None, start as u64);
            }
            Sec::Rom(r) => {
                data = vec![0u8; 0x200];
                data.extend(&r);
                pad(&mut data, 0x200);
                fh[2] = 0;
                fh[4] = 3;
                fh[8..0x100].copy_from_slice(&ivfc_hash_data(0x200, r.len() as u64));
                ctr_apply(&mut data, hi, None, start as u64);
            }
            Sec::Bktr(b) => {
                let p = b.patch.len() as u64;
                let reloc_entries: Vec<Vec<u8>> = b
                    .relocs
                    .iter()
                    .map(|(v, ph, s)| {
                        [
                            v.to_le_bytes().to_vec(),
                            ph.to_le_bytes().to_vec(),
                            s.to_le_bytes().to_vec(),
                        ]
                        .concat()
                    })
                    .collect();
                let mut sub_entries: Vec<Vec<u8>> = b
                    .subs
                    .iter()
                    .map(|(o, g)| {
                        [
                            o.to_le_bytes().to_vec(),
                            0u32.to_le_bytes().to_vec(),
                            g.to_le_bytes().to_vec(),
                        ]
                        .concat()
                    })
                    .collect();
                sub_entries.push(
                    [
                        p.to_le_bytes().to_vec(),
                        0u32.to_le_bytes().to_vec(),
                        0u32.to_le_bytes().to_vec(),
                    ]
                    .concat(),
                ); // sentinel
                let (rt, st) = (
                    tree(&reloc_entries, 0x14, b.total),
                    tree(&sub_entries, 0x10, p),
                );
                // encrypt: patch storage per subsection generation, tables with the plain counter
                let mut patch = b.patch.clone();
                for (k, (off, g)) in b.subs.iter().enumerate() {
                    let end = b.subs.get(k + 1).map(|n| n.0).unwrap_or(p);
                    ctr_apply(
                        &mut patch[*off as usize..end as usize],
                        hi,
                        Some(*g),
                        start as u64 + off,
                    );
                }
                let (mut rt_e, mut st_e) = (rt.clone(), st.clone());
                ctr_apply(&mut rt_e, hi, None, start as u64 + p);
                ctr_apply(&mut st_e, hi, None, start as u64 + p + rt.len() as u64);
                data = patch;
                data.extend(&rt_e);
                data.extend(&st_e);
                fh[2] = 0;
                fh[4] = 4;
                fh[8..0x100].copy_from_slice(&ivfc_hash_data(b.level.0, b.level.1));
                fh[0x100..0x108].copy_from_slice(&p.to_le_bytes());
                fh[0x108..0x110].copy_from_slice(&(rt.len() as u64).to_le_bytes());
                fh[0x110..0x114].copy_from_slice(b"BKTR");
                fh[0x114..0x118].copy_from_slice(&1u32.to_le_bytes());
                fh[0x118..0x11C].copy_from_slice(
                    &(reloc_entries.len() as u32 + b.corrupt as u32).to_le_bytes(),
                );
                fh[0x120..0x128].copy_from_slice(&(p + rt.len() as u64).to_le_bytes());
                fh[0x128..0x130].copy_from_slice(&(st.len() as u64).to_le_bytes());
                fh[0x130..0x134].copy_from_slice(b"BKTR");
                fh[0x134..0x138].copy_from_slice(&1u32.to_le_bytes());
                fh[0x138..0x13C].copy_from_slice(&(sub_entries.len() as u32).to_le_bytes());
            }
        }
        let e = 0x240 + i * 0x10;
        h[e..e + 4].copy_from_slice(&((start / 0x200) as u32).to_le_bytes());
        h[e + 4..e + 8].copy_from_slice(&(((start + data.len()) / 0x200) as u32).to_le_bytes());
        h[0x400 + i * 0x200..0x400 + (i + 1) * 0x200].copy_from_slice(&fh);
        body.extend(data);
    }
    h[0x208..0x210].copy_from_slice(&((0xC00 + body.len()) as u64).to_le_bytes());
    let mut area = [0u8; 64];
    area[0x20..0x30].copy_from_slice(&CTR_KEY);
    ecb_enc(&KAK, &mut area);
    h[0x300..0x340].copy_from_slice(&area);
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&HEADER_KEY[..16])),
        Aes128::new(GenericArray::from_slice(&HEADER_KEY[16..])),
    );
    for (i, s) in h.as_chunks_mut::<0x200>().0.iter_mut().enumerate() {
        xts.encrypt_sector(s, (i as u128).to_be_bytes());
    }
    h.extend(body);
    h
}

/// A rewritable NSO: MOD0 + dynamic + symbols + RELR/RELA in a 0x2300-byte image, one `svc #1` in the text.
pub fn nso(build_tag: u8) -> Vec<u8> {
    let mut img = vec![0u8; 0x2300];
    let p64 = |img: &mut Vec<u8>, o: usize, v: u64| img[o..o + 8].copy_from_slice(&v.to_le_bytes());
    let p32 = |img: &mut Vec<u8>, o: usize, v: u32| img[o..o + 4].copy_from_slice(&v.to_le_bytes());
    p32(&mut img, 0, 0x1400_0020); // b +0x80 (over the MOD0 block)
    p32(&mut img, 4, 0x80);
    img[0x80..0x84].copy_from_slice(b"MOD0");
    p32(&mut img, 0x84, 0x1000 - 0x80);
    p32(&mut img, 0x100, 0xD400_0021); // svc #1
    p32(&mut img, 0x104, 0xD65F_03C0); // ret
    let tags: [(i64, u64); 20] = [
        (4, 0x1200),
        (5, 0x1300),
        (6, 0x1180),
        (10, 0x40),
        (11, 24),
        (7, 0x1500),
        (8, 48),
        (9, 24),
        (23, 0x1540),
        (2, 24),
        (20, 7),
        (12, 0x100),
        (13, 0x104),
        (25, 0x2100),
        (27, 16),
        (26, 0x2110),
        (28, 8),
        (36, 0x1400),
        (35, 16),
        (37, 8),
    ];
    for (i, (t, v)) in tags.iter().enumerate() {
        p64(&mut img, 0x1000 + i * 16, *t as u64);
        p64(&mut img, 0x1000 + i * 16 + 8, *v);
    }
    let sym = |img: &mut Vec<u8>, i: usize, name: u32, info: u8, shndx: u16, v: u64| {
        let o = 0x1180 + i * 24;
        img[o..o + 4].copy_from_slice(&name.to_le_bytes());
        img[o + 4] = info;
        img[o + 6..o + 8].copy_from_slice(&shndx.to_le_bytes());
        img[o + 8..o + 16].copy_from_slice(&v.to_le_bytes());
    };
    sym(&mut img, 1, 1, 0x12, 1, 0x100);
    sym(&mut img, 2, 5, 0x11, 0, 0);
    p32(&mut img, 0x1204, 3);
    img[0x1300..0x130A].copy_from_slice(b"\0foo\0bar\0\0");
    p64(&mut img, 0x1400, 0x2100);
    p64(&mut img, 0x1408, 0b101);
    p64(&mut img, 0x2100, 0x40);
    p64(&mut img, 0x2108, 0x80);
    let rela = |img: &mut Vec<u8>, o: usize, w: u64, kind: u64, si: u64| {
        p64(img, o, w);
        p64(img, o + 8, si << 32 | kind);
        p64(img, o + 16, 0);
    };
    rela(&mut img, 0x1500, 0x2200, 1025, 2);
    rela(&mut img, 0x1518, 0x2208, 257, 1);
    rela(&mut img, 0x1540, 0x2210, 1026, 2);
    // NSO file: three uncompressed segments with SHA-256 flags
    let segs: [(&[u8], u32); 3] = [
        (&img[..0x1000], 0),
        (&img[0x1000..0x2000], 0x1000),
        (&img[0x2000..0x2300], 0x2000),
    ];
    let mut f = vec![0u8; 0x100];
    f[..4].copy_from_slice(b"NSO0");
    let (mut flags, mut body) = (0u32, Vec::<u8>::new());
    for (i, (s, mem)) in segs.iter().enumerate() {
        flags |= 8 << i;
        let h = 0x10 + i * 0x10;
        f[h..h + 4].copy_from_slice(&((0x100 + body.len()) as u32).to_le_bytes());
        f[h + 4..h + 8].copy_from_slice(&mem.to_le_bytes());
        f[h + 8..h + 12].copy_from_slice(&(s.len() as u32).to_le_bytes());
        f[0x60 + i * 4..0x64 + i * 4].copy_from_slice(&(s.len() as u32).to_le_bytes());
        f[0xA0 + i * 0x20..0xC0 + i * 0x20].copy_from_slice(&Sha256::digest(s));
        body.extend(*s);
    }
    f[0xC..0x10].copy_from_slice(&flags.to_le_bytes());
    for (i, b) in f[0x40..0x60].iter_mut().enumerate() {
        *b = build_tag.wrapping_add(i as u8);
    }
    f.extend(body);
    f
}

pub fn exefs(tag: u8) -> Vec<u8> {
    pfs0(&[
        ("main", &nso(tag)),
        ("subsdk0", &nso(tag.wrapping_add(100))),
        ("main.npdm", b"NPDMDATA"),
    ])
}

pub fn nacp(name: &str, version: &str) -> Vec<u8> {
    let mut d = vec![0u8; 0x4000];
    d[..name.len()].copy_from_slice(name.as_bytes());
    d[0x200..0x205].copy_from_slice(b"Pub X");
    d[0x3060..0x3060 + version.len()].copy_from_slice(version.as_bytes());
    d
}

pub struct Rec {
    pub id: [u8; 16],
    pub size: u64,
    pub ctype: u8,
}

pub fn cnmt(kind: u8, title: u64, app: u64, version: u32, recs: &[Rec]) -> Vec<u8> {
    let ext = 0x10usize;
    let mut d = vec![0u8; 0x20 + ext + 0x38 * recs.len()];
    d[..8].copy_from_slice(&title.to_le_bytes());
    d[8..12].copy_from_slice(&version.to_le_bytes());
    d[0xC] = kind;
    d[0xE..0x10].copy_from_slice(&(ext as u16).to_le_bytes());
    d[0x10..0x12].copy_from_slice(&(recs.len() as u16).to_le_bytes());
    if kind != 0x80 {
        d[0x20..0x28].copy_from_slice(&app.to_le_bytes());
    }
    for (i, r) in recs.iter().enumerate() {
        let o = 0x20 + ext + i * 0x38;
        d[o + 0x20..o + 0x30].copy_from_slice(&r.id);
        d[o + 0x30..o + 0x36].copy_from_slice(&r.size.to_le_bytes()[..6]);
        d[o + 0x36] = r.ctype;
    }
    d
}

fn id(n: u8) -> [u8; 16] {
    [n; 16]
}

fn nca_name(id: &[u8; 16]) -> String {
    format!("{}.nca", hex(id))
}

/// Meta NCA carrying one cnmt.
fn meta_nca(title: u64, cn: &[u8]) -> Vec<u8> {
    build_nca(title, 1, vec![Sec::Pfs0(pfs0(&[("c.cnmt", cn)]))])
}

pub struct Game {
    pub dir: PathBuf,
    pub keys: PathBuf,
    pub base_nsp: PathBuf,
    pub update_nsp: PathBuf,
    pub dlc_nsp: PathBuf,
    pub base_romfs: Vec<u8>,
    pub update_romfs: Vec<u8>,
    pub dlc_romfs: Vec<u8>,
    pub update_version: u32,
}

pub fn base_files() -> Vec<(&'static str, Vec<u8>)> {
    vec![
        ("a.bin", b"HELLO".to_vec()),
        ("b.bin", (0..3000u32).map(|i| (i * 7) as u8).collect()),
        ("c.txt", b"third file".to_vec()),
    ]
}

pub fn make_game(dir: &Path, mode_bktr: bool) -> Game {
    make_game_with(dir, mode_bktr, false)
}

/// `base_data_space`: the relocation table's base offsets address the base RomFS data level instead of the whole section.
pub fn make_game_with(dir: &Path, mode_bktr: bool, base_data_space: bool) -> Game {
    make_game_full(dir, mode_bktr, base_data_space, false)
}

pub fn make_game_full(dir: &Path, mode_bktr: bool, base_data_space: bool, corrupt: bool) -> Game {
    make_game_inner(dir, mode_bktr, base_data_space, corrupt, false)
}

/// Like `make_game`, with an HtmlDocument NCA (`index.html`, `a.png`) in the base.
pub fn make_game_html(dir: &Path) -> Game {
    make_game_inner(dir, false, false, false, true)
}

fn make_game_inner(
    dir: &Path,
    mode_bktr: bool,
    base_data_space: bool,
    corrupt: bool,
    html: bool,
) -> Game {
    std::fs::create_dir_all(dir).unwrap();
    let keys = dir.join("prod.keys");
    std::fs::write(&keys, keys_text()).unwrap();
    // ---- base
    let bf = base_files();
    let refs: Vec<(&str, &[u8])> = bf.iter().map(|(n, d)| (*n, d.as_slice())).collect();
    let base_romfs = romfs_flat(&refs);
    let prog = build_nca(
        APP_ID,
        0,
        vec![Sec::Pfs0(exefs(1)), Sec::Rom(base_romfs.clone())],
    );
    let control = build_nca(
        APP_ID,
        2,
        vec![Sec::Rom(romfs_flat(&[
            ("control.nacp", &nacp("Test Game", "1.0.0")),
            (
                "icon_AmericanEnglish.dat",
                &[0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3],
            ),
        ]))],
    );
    let htmlnca = build_nca(
        APP_ID,
        4,
        vec![Sec::Rom(romfs_tree(&[
            ("index.html", b"<html>manual</html>"),
            ("a.png", &[0x89, b'P', b'N', b'G']),
            ("img/ui/b.png", &[1, 2, 3]),
            ("img/c.png", &[4, 5]),
            ("css/main.css", b"body{}"),
        ]))],
    );
    let mut recs = vec![
        Rec {
            id: id(1),
            size: prog.len() as u64,
            ctype: 1,
        },
        Rec {
            id: id(2),
            size: control.len() as u64,
            ctype: 3,
        },
    ];
    if html {
        recs.push(Rec {
            id: id(3),
            size: htmlnca.len() as u64,
            ctype: 4,
        });
    }
    let cn = cnmt(0x80, APP_ID, APP_ID, 0, &recs);
    let meta = meta_nca(APP_ID, &cn);
    let mut base_files_v = vec![
        (nca_name(&id(1)), prog.clone()),
        (nca_name(&id(2)), control.clone()),
        ("m.cnmt.nca".to_string(), meta.clone()),
    ];
    if html {
        base_files_v.push((nca_name(&id(3)), htmlnca));
    }
    let base_refs: Vec<(&str, &[u8])> = base_files_v
        .iter()
        .map(|(n, d)| (n.as_str(), d.as_slice()))
        .collect();
    let base = pfs0(&base_refs);
    let base_nsp = dir.join("base.nsp");
    std::fs::write(&base_nsp, base).unwrap();
    // ---- update: a.bin becomes "WORLD" (same size), everything else is shared with the base
    let mut uf = base_files();
    uf[0].1 = b"WORLD".to_vec();
    let refs: Vec<(&str, &[u8])> = uf.iter().map(|(n, d)| (*n, d.as_slice())).collect();
    let update_romfs = romfs_flat(&refs);
    assert_eq!(update_romfs.len(), base_romfs.len());
    let data_off = u64::from_le_bytes(update_romfs[0x48..0x50].try_into().unwrap());
    // virtual space = [0x200 hash bytes][RomFS]; base section space has the RomFS at 0x200 too
    let mut patch = vec![0u8; 0x400];
    patch[0x200..0x205].copy_from_slice(b"WORLD");
    let total = 0x200 + update_romfs.len() as u64;
    let (v0, v1) = (0x200 + data_off, 0x200 + data_off + 5);
    let sh = if base_data_space { 0x200 } else { 0 };
    let spec = BktrSpec {
        patch,
        relocs: vec![
            (0, 0, 1),
            (0x200, 0x200 - sh, 0),
            (v0, 0x200, 1),
            (v1, v1 - sh, 0),
        ],
        subs: vec![(0, 1), (0x200, 5)],
        total,
        level: (0x200, update_romfs.len() as u64),
        corrupt: false,
    };
    let update_version = 0x10000;
    let mut spec = spec;
    spec.corrupt = corrupt;
    let sec_romfs = if mode_bktr {
        Sec::Bktr(spec)
    } else {
        Sec::Rom(update_romfs.clone())
    };
    let uprog = build_nca(APP_ID, 0, vec![Sec::Pfs0(exefs(50)), sec_romfs]);
    let ucn = cnmt(
        0x81,
        APP_ID + 0x800,
        APP_ID,
        update_version,
        &[Rec {
            id: id(11),
            size: uprog.len() as u64,
            ctype: 1,
        }],
    );
    let umeta = meta_nca(APP_ID + 0x800, &ucn);
    let update_nsp = dir.join("update.nsp");
    std::fs::write(
        &update_nsp,
        pfs0(&[(&nca_name(&id(11)), &uprog), ("m.cnmt.nca", &umeta)]),
    )
    .unwrap();
    // ---- DLC
    let dlc_romfs = romfs_flat(&[(
        "dlc.dat",
        &(0..1500u32).map(|i| (i * 3) as u8).collect::<Vec<u8>>(),
    )]);
    let ddata = build_nca(APP_ID + 0x1001, 5, vec![Sec::Rom(dlc_romfs.clone())]);
    let dcn = cnmt(
        0x82,
        APP_ID + 0x1001,
        APP_ID,
        0,
        &[Rec {
            id: id(21),
            size: ddata.len() as u64,
            ctype: 2,
        }],
    );
    let dmeta = meta_nca(APP_ID + 0x1001, &dcn);
    let dlc_nsp = dir.join("dlc.nsp");
    std::fs::write(
        &dlc_nsp,
        pfs0(&[(&nca_name(&id(21)), &ddata), ("m.cnmt.nca", &dmeta)]),
    )
    .unwrap();
    Game {
        dir: dir.to_path_buf(),
        keys,
        base_nsp,
        update_nsp,
        dlc_nsp,
        base_romfs,
        update_romfs,
        dlc_romfs,
        update_version,
    }
}

/// Temporary directory removed on drop.
pub struct Tmp(pub PathBuf);

impl std::ops::Deref for Tmp {
    type Target = PathBuf;
    fn deref(&self) -> &PathBuf {
        &self.0
    }
}

impl AsRef<Path> for Tmp {
    fn as_ref(&self) -> &Path {
        &self.0
    }
}

impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

pub fn tmp(name: &str) -> Tmp {
    let d = std::env::temp_dir().join(format!("napple-e2e-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    Tmp(d)
}
