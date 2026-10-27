//! Fixtures written by public tools (the `nsz` compressor) from SYNTHETIC NCAs with throwaway keys:
//! see tests/fixtures/README.md and tests/fixtures/gen_nsz.py. They prove the readers against bytes
//! another implementation wrote, which the self-made synthetic builders in ingest.rs cannot.
use napple_core::application::ingest::ingest;
use napple_core::domain::error::Error;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{FileSource, Src, read_vec};
use napple_core::infrastructure::nca::{self, Nca, TitleKeys};
use napple_core::infrastructure::ncz::{NCZ_HEADER, NczSource, is_ncz};
use napple_core::infrastructure::pfs::{Fs, parse_pfs0};
use std::path::{Path, PathBuf};

#[allow(dead_code)]
mod nca0_fixture;

fn fx(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/nsz")
        .join(name)
}

fn keys() -> KeySet {
    KeySet::load(&fx("synthetic_keys.txt")).unwrap()
}

fn keys_with_device() -> KeySet {
    // eticket_rsa_keypair layout (public docs): D at 0x00, N at 0x100, E at 0x200, big-endian.
    let mut kp = unhex(nca0_fixture::D);
    kp.extend(unhex(nca0_fixture::N));
    kp.extend([0, 1, 0, 1]);
    kp.resize(0x230, 0);
    let text = std::fs::read_to_string(fx("synthetic_keys.txt")).unwrap();
    KeySet::parse(&format!("{text}eticket_rsa_keypair = {}\n", hex(&kp))).unwrap()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn read(name: &str) -> Vec<u8> {
    std::fs::read(fx(name)).unwrap()
}

/// The single `.ncz` member of an NSZ.
fn ncz_member(nsz: &str) -> (Fs, String) {
    let fs = parse_pfs0(FileSource::open(&fx(nsz)).unwrap()).unwrap();
    let name = fs
        .entries
        .iter()
        .find(|e| e.name.ends_with(".ncz"))
        .expect("an .ncz member")
        .name
        .clone();
    (fs, name)
}

fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// Section table of the NCZ as the nsz tool wrote it: (offset, size, crypto type) per entry.
fn ncz_sections(ncz: &Src) -> Vec<(u64, u64, u64)> {
    let n = u64le(&read_vec(&**ncz, NCZ_HEADER + 8, 8).unwrap(), 0) as usize;
    let raw = read_vec(&**ncz, NCZ_HEADER + 16, n * 0x40).unwrap();
    (0..n)
        .map(|i| {
            (
                u64le(&raw, i * 0x40),
                u64le(&raw, i * 0x40 + 8),
                u64le(&raw, i * 0x40 + 16),
            )
        })
        .collect()
}

#[test]
fn nsz_tool_output_reads_back_byte_exact() {
    for (nsz, nca) in [
        ("c00_solid.nsz.bin", "c00.nca.bin"),
        ("c00_block.nsz.bin", "c00.nca.bin"),
        ("g4000_solid.nsz.bin", "g4000.nca.bin"),
        ("g4000_block.nsz.bin", "g4000.nca.bin"),
    ] {
        let want = read(nca);
        let (fs, name) = ncz_member(nsz);
        let ncz = fs.open(&name).unwrap();
        assert!(is_ncz(&ncz), "{nsz}: NCZSECTN");
        let src = NczSource::open(ncz).unwrap();
        assert_eq!(src.size(), want.len() as u64, "{nsz}: size");
        let mut got = vec![0u8; want.len()];
        src.read_at(0, &mut got).unwrap();
        assert!(got == want, "{nsz}: full read differs");
        // backward, unaligned and boundary-crossing reads
        for (o, l) in [
            (want.len() - 100, 100usize),
            (0x3ff0, 0x40),
            (0xC00, 0x200),
            (0x4000 + 16383, 20000),
            (7, 3),
            (0x5555, 1),
        ] {
            let mut b = vec![0u8; l];
            src.read_at(o as u64, &mut b).unwrap();
            assert_eq!(b, &want[o..o + l], "{nsz}: read at {o:#x}+{l}");
        }
    }
}

#[test]
fn nsz_tool_layouts_are_the_ones_the_reader_special_cases() {
    // nsz records the first section where the NCA has it: right after the 0xC00 header (inside
    // its raw 0x4000 block, the straddling case) or at 0x4000 when the NCA is padded.
    for (nsz, first, block) in [
        ("c00_solid.nsz.bin", 0xC00u64, false),
        ("c00_block.nsz.bin", 0xC00, true),
        ("g4000_solid.nsz.bin", 0x4000, false),
        ("g4000_block.nsz.bin", 0x4000, true),
    ] {
        let (fs, name) = ncz_member(nsz);
        let ncz = fs.open(&name).unwrap();
        let secs = ncz_sections(&ncz);
        assert_eq!(secs.len(), 2, "{nsz}: CTR section + unencrypted section");
        assert_eq!(secs[0].0, first, "{nsz}: first section offset");
        assert_eq!(secs[0].2, 3, "{nsz}: AES-CTR");
        assert_eq!(secs[1].2, 1, "{nsz}: plain section");
        let after = NCZ_HEADER + 16 + 2 * 0x40;
        let tag = read_vec(&*ncz, after, 8).unwrap();
        assert_eq!(tag == b"NCZBLOCK", block, "{nsz}: block form");
        if block {
            let h = read_vec(&*ncz, after, 0x18).unwrap();
            assert_eq!(h[0x08], 2, "{nsz}: block type"); // type 2 = zstd blocks
            assert!([14, 15].contains(&h[0x0b]), "{nsz}: block size exponent");
        }
    }
}

#[test]
fn nca_reads_through_the_ncz_view() {
    let ks = keys();
    let (fs, name) = ncz_member("c00_block.nsz.bin");
    let n = Nca::open(
        NczSource::open(fs.open(&name).unwrap()).unwrap(),
        &ks,
        &TitleKeys::new(),
    )
    .unwrap();
    assert_eq!(n.program_id, 0x0100_0000_0000_1000);
    let sec = &n.sections[0];
    let pfs = parse_pfs0(nca::pfs0_data(&n.section(sec.index).unwrap(), sec).unwrap()).unwrap();
    let main = pfs.read("main").unwrap();
    assert_eq!(main.len(), 150_000);
    assert_eq!(pfs.read("main.npdm").unwrap()[..5], *b"npdm-");
}

fn pfs0_one(name: &str, data: &[u8]) -> Vec<u8> {
    let mut strtab = name.as_bytes().to_vec();
    strtab.push(0);
    strtab.resize(strtab.len().next_multiple_of(0x20), 0);
    let mut out = b"PFS0".to_vec();
    out.extend(1u32.to_le_bytes());
    out.extend((strtab.len() as u32).to_le_bytes());
    out.extend([0u8; 4]);
    out.extend(0u64.to_le_bytes());
    out.extend((data.len() as u64).to_le_bytes());
    out.extend([0u8; 8]);
    out.extend(strtab);
    out.extend(data);
    out
}

fn exefs_of(path: &Path, ks: &KeySet) -> Vec<(String, Vec<u8>)> {
    ingest(path, ks).unwrap().exefs
}

#[test]
fn ingest_same_exefs_from_nsz_xcz_and_plain_nca() {
    let ks = keys();
    // the reference: the same NCA, uncompressed, in a PFS0 (built here so no nsz bytes are involved)
    let nca = read("c00.nca.bin");
    let nsp = pfs0_one("c00plain.nca", &nca);
    let dir = std::env::temp_dir().join(format!("napple-realw-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let plain = dir.join("plain.nsp");
    std::fs::write(&plain, &nsp).unwrap();
    let want = exefs_of(&plain, &ks);
    assert_eq!(want.len(), 2);

    for nsz in [
        "c00_solid.nsz.bin",
        "c00_block.nsz.bin",
        "g4000_solid.nsz.bin",
        "g4000_block.nsz.bin",
    ] {
        // g4000 is a different NCA body (different seed): only compare with itself below
        if nsz.starts_with("c00") {
            assert_eq!(exefs_of(&fx(nsz), &ks), want, "{nsz}");
        } else {
            let got = exefs_of(&fx(nsz), &ks);
            assert_eq!(got.len(), 2, "{nsz}");
            assert_eq!(got[0].1.len(), 150_000);
        }
    }
    // XCZ written by nsz from an XCI (block form), plain and behind a 0x1000 key area
    assert_eq!(exefs_of(&fx("c00.xcz.bin"), &ks), want, "xcz");
    let mut full = vec![0u8; 0x1000];
    full.extend(read("c00.xcz.bin"));
    let p = dir.join("full.xcz");
    std::fs::write(&p, &full).unwrap();
    assert_eq!(exefs_of(&p, &ks), want, "xcz with key area");
}

#[test]
fn nca2_written_independently_reads() {
    // NCA2: FS headers are separate XTS units at sector 0 (hactool); built by gen_nsz.py, not by
    // this crate. nsz itself leaves NCA2 uncompressed (it reads NCA2 FS headers as NCA3 ones).
    let ks = keys();
    let n = Nca::open(
        FileSource::open(&fx("nca2.nca.bin")).unwrap(),
        &ks,
        &TitleKeys::new(),
    )
    .unwrap();
    let sec = &n.sections[0];
    let pfs = parse_pfs0(nca::pfs0_data(&n.section(sec.index).unwrap(), sec).unwrap()).unwrap();
    assert_eq!(pfs.read("main").unwrap().len(), 150_000);
    // and the NCA3 twin differs from it only by header magic/sector numbering, not by readability
    let n3 = Nca::open(
        FileSource::open(&fx("c00.nca.bin")).unwrap(),
        &ks,
        &TitleKeys::new(),
    )
    .unwrap();
    assert_eq!(n3.sections.len(), n.sections.len());
}

#[test]
fn personalized_ticket_nsz_resolves_with_device_key() {
    // rights_solid.nsz: nsz compressed the NCA (it was given the title key in common-ticket form);
    // the shipped ticket was then swapped for a personalized (RSA-OAEP) one.
    let want_key = unhex(
        std::fs::read_to_string(fx("rights_enc_title_key.hex"))
            .unwrap()
            .trim(),
    );
    let fs = parse_pfs0(FileSource::open(&fx("rights_solid.nsz.bin")).unwrap()).unwrap();
    let tik = fs
        .entries
        .iter()
        .find(|e| e.name.ends_with(".tik"))
        .expect("ticket");
    let t =
        napple_core::infrastructure::ticket::parse_ticket(&fs.read(&tik.name).unwrap()).unwrap();
    assert_eq!(t.key_type, 1, "personalized");
    let with = keys_with_device();
    let k = napple_core::infrastructure::ticket::encrypted_title_key(&t, &with)
        .unwrap()
        .unwrap();
    assert_eq!(k.to_vec(), want_key);

    // end to end through the ingest path: NSZ -> NCZ view -> title key -> section decrypt
    let r = ingest(&fx("rights_solid.nsz.bin"), &with).unwrap();
    assert_eq!(r.exefs.len(), 2);
    // without the device key the ticket is skipped and the NCA names what is missing
    assert!(matches!(
        ingest(&fx("rights_solid.nsz.bin"), &keys()),
        Err(Error::MissingKey(m)) if m.contains("ticket")
    ));
}
