//! Synthetic fixtures only: built with random keys, no game data. These prove internal consistency
//! (format parsing, crypto plumbing, hostile-input handling), NOT compatibility with retail files;
//! that needs a real game, tracked as a pending validation.
use aes::cipher::{generic_array::GenericArray, BlockEncrypt, KeyInit, KeyIvInit, StreamCipher, StreamCipherSeek};
use aes::Aes128;
use napple_core::application::ingest::ingest;
use napple_core::domain::error::Error;
use napple_core::domain::keys::KeySet;
use sha2::{Digest, Sha256};
use xts_mode::Xts128;

const HEADER_KEY: [u8; 32] = [7; 32];
const KAK: [u8; 16] = [9; 16]; // key_area_key_application_02
const TKEK: [u8; 16] = [5; 16]; // titlekek_02
const CTR_KEY: [u8; 16] = [0x42; 16];

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
fn keys_text() -> String {
    format!("header_key = {}\nkey_area_key_application_02 = {}\ntitlekek_02 = {}\n", hex(&HEADER_KEY), hex(&KAK), hex(&TKEK))
}
fn ecb_enc(key: &[u8; 16], data: &mut [u8]) {
    let c = Aes128::new(GenericArray::from_slice(key));
    for b in data.chunks_exact_mut(16) {
        c.encrypt_block(GenericArray::from_mut_slice(b));
    }
}
fn pad(v: &mut Vec<u8>, to: usize) {
    while v.len() % to != 0 {
        v.push(0);
    }
}

fn pfs0(files: &[(&str, &[u8])], hfs: bool) -> Vec<u8> {
    let esz = if hfs { 64 } else { 24 };
    let mut strtab = Vec::new();
    let mut ents = Vec::new();
    let mut off = 0u64;
    for (n, d) in files {
        let mut e = vec![0u8; esz];
        e[..8].copy_from_slice(&off.to_le_bytes());
        e[8..16].copy_from_slice(&(d.len() as u64).to_le_bytes());
        e[16..20].copy_from_slice(&(strtab.len() as u32).to_le_bytes());
        ents.extend(e);
        strtab.extend(n.as_bytes());
        strtab.push(0);
        off += d.len() as u64;
    }
    let mut out = Vec::new();
    out.extend(if hfs { b"HFS0" } else { b"PFS0" });
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

fn romfs() -> Vec<u8> {
    // root dir (0): child_dir=24 ("d"), child_file=0 ("a.bin"); dir "d" (24): child_file=0x28 ("b.bin")
    let none = u32::MAX;
    let mut dirs = Vec::new();
    for v in [0u32, none, 24, 0, none, 0] { dirs.extend(v.to_le_bytes()); }
    for v in [0u32, none, none, 0x28, none, 1] { dirs.extend(v.to_le_bytes()); }
    dirs.extend(b"d\0\0\0");
    let mut files = Vec::new();
    // a.bin: sibling none, data 0, size 5 ; entry size 32 + 8
    for v in [0u32, none] { files.extend(v.to_le_bytes()); }
    files.extend(0u64.to_le_bytes()); files.extend(5u64.to_le_bytes());
    for v in [none, 5u32] { files.extend(v.to_le_bytes()); }
    files.extend(b"a.bin\0\0\0");
    assert_eq!(files.len(), 0x28);
    for v in [24u32, none] { files.extend(v.to_le_bytes()); }
    files.extend(8u64.to_le_bytes()); files.extend(3u64.to_le_bytes());
    for v in [none, 5u32] { files.extend(v.to_le_bytes()); }
    files.extend(b"b.bin\0\0\0");
    let mut h = Vec::new();
    let (dm, fm, data) = (0x50u64, 0x50 + dirs.len() as u64, 0x50 + (dirs.len() + files.len()) as u64);
    let data = (data + 0x1F) & !0x1F;
    for v in [0x50u64, 0, 0, dm, dirs.len() as u64, 0, 0, fm, files.len() as u64, data] { h.extend(v.to_le_bytes()); }
    h.extend(dirs); h.extend(files);
    h.resize(data as usize, 0);
    h.extend(b"HELLO\0\0\0abc");
    h
}

enum Sec { Pfs0(Vec<u8>, bool /*tamper*/), Rom(Vec<u8>) }

fn build_nca(ctype: u8, secs: Vec<Sec>, rights: Option<[u8; 16]>) -> Vec<u8> {
    let mut h = vec![0u8; 0xC00];
    h[0x200..0x204].copy_from_slice(b"NCA3");
    h[0x205] = ctype;
    h[0x220] = 3; // key generation 3 -> index 2
    h[0x210..0x218].copy_from_slice(&0x0100_0000_0000_1000u64.to_le_bytes());
    if let Some(r) = rights { h[0x230..0x240].copy_from_slice(&r); }
    let mut body = Vec::new();
    for (i, s) in secs.into_iter().enumerate() {
        let start = 0xC00 + body.len();
        let (data, fs_type, hash_data): (Vec<u8>, u8, Vec<u8>) = match s {
            Sec::Pfs0(p, tamper) => {
                let mut table = Sha256::digest(&p).to_vec();
                let mut hd = vec![0u8; 0xF8];
                hd[..0x20].copy_from_slice(&Sha256::digest(&table));
                hd[0x20..0x24].copy_from_slice(&(p.len() as u32).to_le_bytes());
                hd[0x24] = 2;
                hd[0x30..0x38].copy_from_slice(&32u64.to_le_bytes());
                hd[0x38..0x40].copy_from_slice(&0x200u64.to_le_bytes());
                hd[0x40..0x48].copy_from_slice(&(p.len() as u64).to_le_bytes());
                if tamper { table[0] ^= 1; }
                let mut d = table; pad(&mut d, 0x200); d.extend(p); pad(&mut d, 0x200);
                (d, 1, hd)
            }
            Sec::Rom(r) => {
                let mut hd = vec![0u8; 0xF8];
                hd[..4].copy_from_slice(b"IVFC");
                let l = 0x10 + 5 * 0x18;
                hd[l..l + 8].copy_from_slice(&0x200u64.to_le_bytes());
                hd[l + 8..l + 16].copy_from_slice(&(r.len() as u64).to_le_bytes());
                let mut d = vec![0u8; 0x200]; d.extend(r); pad(&mut d, 0x200);
                (d, 0, hd)
            }
        };
        let e = 0x240 + i * 0x10;
        h[e..e + 4].copy_from_slice(&((start / 0x200) as u32).to_le_bytes());
        h[e + 4..e + 8].copy_from_slice(&(((start + data.len()) / 0x200) as u32).to_le_bytes());
        let f = 0x400 + i * 0x200;
        h[f + 2] = fs_type; h[f + 3] = 2; h[f + 4] = 3;
        h[f + 8..f + 8 + 0xF8].copy_from_slice(&hash_data);
        let ctr_hdr = [1u8, 2, 3, 4, 5, 6, 7, 8 + i as u8];
        h[f + 0x140..f + 0x148].copy_from_slice(&ctr_hdr);
        let mut hi = ctr_hdr; hi.reverse();
        let mut iv = [0u8; 16]; iv[..8].copy_from_slice(&hi);
        let mut enc = data;
        let mut c = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(&CTR_KEY), GenericArray::from_slice(&iv));
        c.seek(start as u64);
        c.apply_keystream(&mut enc);
        body.extend(enc);
    }
    h[0x208..0x210].copy_from_slice(&((0xC00 + body.len()) as u64).to_le_bytes());
    let mut area = [0u8; 64];
    area[0x20..0x30].copy_from_slice(&CTR_KEY);
    ecb_enc(&KAK, &mut area);
    if rights.is_some() {
        // ticket path: area is ignored by the reader
    }
    h[0x300..0x340].copy_from_slice(&area);
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&HEADER_KEY[..16])),
        Aes128::new(GenericArray::from_slice(&HEADER_KEY[16..])),
    );
    for (i, s) in h.chunks_exact_mut(0x200).enumerate() {
        xts.encrypt_sector(s, (i as u128).to_be_bytes());
    }
    h.extend(body);
    h
}

fn ticket(rights: [u8; 16], title_key: [u8; 16]) -> Vec<u8> {
    let mut t = vec![0u8; 4 + 0x13C + 0x2C0];
    t[..4].copy_from_slice(&0x10004u32.to_le_bytes());
    let b = 4 + 0x13C;
    let mut k = title_key;
    ecb_enc(&TKEK, &mut k);
    t[b + 0x40..b + 0x50].copy_from_slice(&k);
    t[b + 0x160..b + 0x170].copy_from_slice(&rights);
    t
}

fn exefs() -> Vec<u8> {
    pfs0(&[("main", b"MAINCODE"), ("main.npdm", b"npdm")], false)
}

fn write(name: &str, data: &[u8]) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("napple-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join(name);
    std::fs::write(&p, data).unwrap();
    p
}

fn keys() -> KeySet {
    KeySet::parse(&keys_text()).unwrap()
}

#[test]
fn nsp_key_area_exefs_and_romfs() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false), Sec::Rom(romfs())], None);
    let nsp = pfs0(&[("prog.nca", &nca)], false);
    let r = ingest(&write("a.nsp", &nsp), &keys()).unwrap();
    assert_eq!(r.title_id, Some(0x0100_0000_0000_1000));
    assert_eq!(r.exefs.iter().find(|(n, _)| n == "main").unwrap().1, b"MAINCODE");
    let rf = r.romfs.unwrap();
    let mut paths: Vec<_> = rf.files.iter().map(|f| f.path.clone()).collect();
    paths.sort();
    assert_eq!(paths, ["a.bin", "d/b.bin"]);
    let f = rf.files.iter().find(|f| f.path == "d/b.bin").unwrap();
    let mut buf = [0u8; 3];
    rf.open(f).unwrap().read_at(0, &mut buf).unwrap();
    assert_eq!(&buf, b"abc");
}

#[test]
fn nsp_titlekey_via_ticket() {
    let rid = [0xAA; 16];
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false)], Some(rid));
    let nsp = pfs0(&[("p.nca", &nca), ("p.tik", &ticket(rid, CTR_KEY))], false);
    let r = ingest(&write("b.nsp", &nsp), &keys()).unwrap();
    assert_eq!(r.exefs.len(), 2);
}

#[test]
fn xci_secure_partition() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false)], None);
    let secure = pfs0(&[("p.nca", &nca)], true);
    let root = pfs0(&[("secure", &secure)], true);
    let mut xci = vec![0u8; 0x200];
    xci[0x100..0x104].copy_from_slice(b"HEAD");
    xci[0x138..0x140].copy_from_slice(&0x200u64.to_le_bytes());
    xci[0x140..0x148].copy_from_slice(&(root.len() as u64).to_le_bytes());
    xci.extend(root);
    assert_eq!(ingest(&write("c.xci", &xci), &keys()).unwrap().exefs.len(), 2);
}

#[test]
fn wrong_header_key_is_integrity_error() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false)], None);
    let nsp = pfs0(&[("p.nca", &nca)], false);
    let bad = KeySet::parse(&keys_text().replace(&hex(&HEADER_KEY), &hex(&[8u8; 32]))).unwrap();
    assert!(matches!(ingest(&write("d.nsp", &nsp), &bad), Err(Error::Integrity(_))));
}

#[test]
fn wrong_area_key_fails_hash_check() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false)], None);
    let nsp = pfs0(&[("p.nca", &nca)], false);
    let bad = KeySet::parse(&keys_text().replace(&hex(&KAK), &hex(&[1u8; 16]))).unwrap();
    assert!(matches!(ingest(&write("e.nsp", &nsp), &bad), Err(Error::Integrity(_))));
}

#[test]
fn tampered_hash_table_rejected() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), true)], None);
    let nsp = pfs0(&[("p.nca", &nca)], false);
    assert!(matches!(ingest(&write("f.nsp", &nsp), &keys()), Err(Error::Integrity(_))));
}

#[test]
fn missing_key_reported_by_name() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false)], None);
    let nsp = pfs0(&[("p.nca", &nca)], false);
    let k = KeySet::parse(&format!("header_key = {}\n", hex(&HEADER_KEY))).unwrap();
    assert!(matches!(ingest(&write("g.nsp", &nsp), &k), Err(Error::MissingKey(n)) if n == "key_area_key_application_02"));
}

#[test]
fn path_traversal_names_rejected() {
    let nsp = pfs0(&[("../evil.nca", b"x")], false);
    assert!(matches!(ingest(&write("h.nsp", &nsp), &keys()), Err(Error::Container(_))));
}

#[test]
fn truncated_and_garbage_inputs_do_not_panic() {
    let nca = build_nca(0, vec![Sec::Pfs0(exefs(), false), Sec::Rom(romfs())], None);
    let nsp = pfs0(&[("p.nca", &nca)], false);
    for cut in [0, 3, 16, 40, nsp.len() / 2, nsp.len() - 1] {
        assert!(ingest(&write("t.nsp", &nsp[..cut]), &keys()).is_err());
    }
    assert!(ingest(&write("z.bin", &[0u8; 64]), &keys()).is_err());
}

/// Converts a synthetic single-CTR-section NCA into NCZ form (stream or block), like `nsz` does.
fn to_ncz(nca: &[u8], block_exp: Option<u8>) -> Vec<u8> {
    let mut out = nca[..0x4000].to_vec();
    let mut body = nca[0x4000..].to_vec();
    let mut iv = [0u8; 16];
    iv[..8].copy_from_slice(&[8, 7, 6, 5, 4, 3, 2, 1]); // section 0 ctr header [1..8] reversed
    let mut c = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(&CTR_KEY), GenericArray::from_slice(&iv));
    c.seek(0x4000u64);
    c.apply_keystream(&mut body); // CTR is symmetric: this yields the plaintext
    out.extend(b"NCZSECTN");
    out.extend(1u64.to_le_bytes());
    for v in [0x4000u64, body.len() as u64, 3, 0] {
        out.extend(v.to_le_bytes());
    }
    out.extend(CTR_KEY);
    out.extend(iv);
    match block_exp {
        None => out.extend(zstd::encode_all(&body[..], 3).unwrap()),
        Some(x) => {
            let blocks: Vec<Vec<u8>> = body.chunks(1 << x).map(|c| zstd::bulk::compress(c, 3).unwrap()).collect();
            out.extend(b"NCZBLOCK");
            out.extend([2u8, 1, 0, x]);
            out.extend((blocks.len() as u32).to_le_bytes());
            out.extend((body.len() as u64).to_le_bytes());
            for b in &blocks { out.extend((b.len() as u32).to_le_bytes()); }
            for b in &blocks { out.extend(b); }
        }
    }
    out
}

fn big_exefs() -> Vec<u8> {
    let pad = vec![0x5Au8; 0x6000];
    pfs0(&[("main", b"MAINCODE"), ("main.npdm", b"npdm"), ("sdk", &pad)], false)
}

#[test]
fn nsz_and_xcz_ingest_same_as_nsp() {
    let nca = build_nca(0, vec![Sec::Pfs0(big_exefs(), false)], None);
    let want = ingest(&write("n.nsp", &pfs0(&[("p.nca", &nca)], false)), &keys()).unwrap();
    for exp in [None, Some(14u8)] {
        let ncz = to_ncz(&nca, exp);
        let nsz = pfs0(&[("p.ncz", &ncz)], false);
        let got = ingest(&write("n.nsz", &nsz), &keys()).unwrap();
        assert_eq!(got.title_id, want.title_id);
        assert_eq!(got.exefs, want.exefs);
        assert_eq!(got.ncas[0].name, "p.nca");
        // XCZ = same HFS0 layout as XCI.
        let secure = pfs0(&[("p.ncz", &ncz)], true);
        let root = pfs0(&[("secure", &secure)], true);
        let mut xcz = vec![0u8; 0x200];
        xcz[0x100..0x104].copy_from_slice(b"HEAD");
        xcz[0x138..0x140].copy_from_slice(&0x200u64.to_le_bytes());
        xcz[0x140..0x148].copy_from_slice(&(root.len() as u64).to_le_bytes());
        xcz.extend(root);
        assert_eq!(ingest(&write("n.xcz", &xcz), &keys()).unwrap().exefs, want.exefs);
    }
}
