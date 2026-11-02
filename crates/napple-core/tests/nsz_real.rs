//! A real dump through the NSZ path: the Testing Material NSP is converted to an NSZ by a minimal NCZ
//! writer (decrypt the AES-CTR sections, zstd the body, record keys and counters; the same layout the
//! public `nsz` tool writes), then every NCA read back through `NczSource` must hash like the original.
//! Skipped unless NAPPLE_TESTING_MATERIAL points at the Testing Material folder. Set
//! NAPPLE_NSZ_OUT=<dir> to keep the NSZ (the build tests ingest it), NAPPLE_NSZ_FULL=1 to convert the
//! multi-gigabyte RomFS NCA too (otherwise NCAs over 200 MB stay plain `.nca` members, which an NSZ
//! may mix).
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{FileSource, Src, read_vec};
use napple_core::infrastructure::nca::{Nca, TitleKeys};
use napple_core::infrastructure::ncz::{NCZ_HEADER, NczSource};
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::ticket::{encrypted_title_key, parse_ticket};
use sha2::{Digest, Sha256};
use std::io::Write;
use std::path::{Path, PathBuf};

const CHUNK: usize = 4 << 20;
const BLOCK: usize = 1 << 20; // NAPPLE_NSZ_BLOCK=1: NCZBLOCK form, 1 MiB blocks (random access)

fn material() -> Option<PathBuf> {
    std::env::var_os("NAPPLE_TESTING_MATERIAL").map(PathBuf::from)
}

fn sha(src: &Src) -> [u8; 32] {
    let mut h = Sha256::new();
    let (mut off, size) = (0u64, src.size());
    while off < size {
        let n = (size - off).min(CHUNK as u64) as usize;
        h.update(read_vec(&**src, off, n).unwrap());
        off += n as u64;
    }
    h.finalize().into()
}

/// The NCZ bytes of `nca` streamed into `out`: raw 0x4000 head, NCZSECTN table, one zstd stream of
/// the decrypted body.
fn write_ncz(src: &Src, nca: &Nca, out: &mut impl Write) {
    let size = src.size();
    out.write_all(&read_vec(&**src, 0, NCZ_HEADER as usize).unwrap())
        .unwrap();
    // Sections: gaps and plain sections are recorded unencrypted (type 1), CTR sections type 3.
    let mut secs: Vec<(u64, u64, u64, [u8; 16], [u8; 16])> = Vec::new();
    let mut at = NCZ_HEADER;
    for (start, len, key, ctr) in nca.ctr_sections() {
        let end = start + len;
        let start = start.max(NCZ_HEADER);
        if start > at {
            secs.push((at, start - at, 1, [0; 16], [0; 16]));
        }
        secs.push((start, end - start, 3, key, ctr));
        at = end;
    }
    if at < size {
        secs.push((at, size - at, 1, [0; 16], [0; 16]));
    }
    if std::env::var_os("NAPPLE_NSZ_DEBUG").is_some() {
        eprintln!("sections of a {size} byte NCA: {:?}", secs.iter().map(|s| (s.0, s.1, s.2)).collect::<Vec<_>>());
    }
    out.write_all(b"NCZSECTN").unwrap();
    out.write_all(&(secs.len() as u64).to_le_bytes()).unwrap();
    for (off, len, ty, key, ctr) in &secs {
        out.write_all(&off.to_le_bytes()).unwrap();
        out.write_all(&len.to_le_bytes()).unwrap();
        out.write_all(&ty.to_le_bytes()).unwrap();
        out.write_all(&0u64.to_le_bytes()).unwrap();
        out.write_all(key).unwrap();
        out.write_all(ctr).unwrap();
    }
    let block = std::env::var_os("NAPPLE_NSZ_BLOCK").is_some();
    let tmp = std::env::temp_dir().join(format!("napple-nsz-blocks-{}", std::process::id()));
    let (mut sizes, mut blocks_file, mut pending) = (Vec::<u32>::new(), None, Vec::<u8>::new());
    if block {
        blocks_file = Some(std::io::BufWriter::new(std::fs::File::create(&tmp).unwrap()));
    }
    let mut flush_block = |buf: &[u8], sizes: &mut Vec<u32>, f: &mut std::io::BufWriter<std::fs::File>| {
        let z = zstd::bulk::compress(buf, 1).unwrap();
        let data = if z.len() >= buf.len() { buf } else { &z[..] };
        sizes.push(data.len() as u32);
        f.write_all(data).unwrap();
    };
    let mut enc = zstd::stream::Encoder::new(&mut *out, 1).unwrap();
    for (off, len, ty, _, _) in &secs {
        let plain: Src = if *ty == 3 {
            // decrypted view through the NCA's own CTR source, positioned by file offset
            let idx = nca
                .sections
                .iter()
                .find(|s| s.start <= *off && *off < s.end)
                .unwrap();
            let view = nca.section(idx.index).unwrap();
            let rel = *off - idx.start;
            let part = napple_core::infrastructure::io::Slice::new(view, rel, *len).unwrap();
            part
        } else {
            napple_core::infrastructure::io::Slice::new(src.clone(), *off, *len).unwrap()
        };
        let (mut o, total) = (0u64, plain.size());
        while o < total {
            let n = (total - o).min(CHUNK as u64) as usize;
            let chunk = read_vec(&*plain, o, n).unwrap();
            if block {
                pending.extend_from_slice(&chunk);
                while pending.len() >= BLOCK {
                    let rest = pending.split_off(BLOCK);
                    flush_block(&pending, &mut sizes, blocks_file.as_mut().unwrap());
                    pending = rest;
                }
            } else {
                enc.write_all(&chunk).unwrap();
            }
            o += n as u64;
        }
    }
    if block {
        if !pending.is_empty() {
            flush_block(&pending, &mut sizes, blocks_file.as_mut().unwrap());
        }
        blocks_file.take().unwrap().flush().unwrap();
        drop(enc); // an unused stream encoder wrote nothing but its empty-frame epilogue on finish; skip it
        let total: u64 = secs.iter().map(|s| s.1).sum();
        out.write_all(b"NCZBLOCK").unwrap();
        out.write_all(&[2u8, 1, 0, BLOCK.trailing_zeros() as u8]).unwrap();
        out.write_all(&(sizes.len() as u32).to_le_bytes()).unwrap();
        out.write_all(&total.to_le_bytes()).unwrap();
        for z in &sizes {
            out.write_all(&z.to_le_bytes()).unwrap();
        }
        std::io::copy(&mut std::fs::File::open(&tmp).unwrap(), out).unwrap();
        std::fs::remove_file(&tmp).unwrap();
    } else {
        enc.finish().unwrap();
    }
}

#[test]
fn smo_nsp_converted_to_nsz_reads_back_byte_identical() {
    let Some(root) = material() else {
        eprintln!("NAPPLE_TESTING_MATERIAL not set: skipped");
        return;
    };
    let nsp = std::fs::read_dir(&root)
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .find(|p| {
            p.extension().is_some_and(|x| x == "nsp")
                && p.file_name().unwrap().to_string_lossy().contains("0100000000010000")
        });
    let Some(nsp) = nsp else {
        eprintln!("SMO NSP not found: skipped");
        return;
    };
    let keys = KeySet::load(&root.join("Keys 22.1.0/prod.keys")).unwrap();
    let full = std::env::var_os("NAPPLE_NSZ_FULL").is_some();
    let outdir = std::env::var_os("NAPPLE_NSZ_OUT")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("napple-nsz-real"));
    std::fs::create_dir_all(&outdir).unwrap();

    let fs = parse_pfs0(FileSource::open(&nsp).unwrap()).unwrap();
    let mut tks = TitleKeys::new();
    for e in fs.entries.iter().filter(|e| e.name.ends_with(".tik")) {
        let t = parse_ticket(&fs.read(&e.name).unwrap()).unwrap();
        assert_eq!(t.key_type, 0, "SMO ships a common ticket");
        tks.insert(t.rights_id, encrypted_title_key(&t, &keys).unwrap().unwrap());
    }

    // Write every member: converted NCAs as .ncz (the cnmt stays .nca like nsz does), the rest as is.
    let nsz_path = outdir.join("smo.nsz");
    let mut members: Vec<(String, PathBuf, u64)> = Vec::new();
    for e in &fs.entries {
        let tmp = outdir.join(format!("member{}", members.len()));
        let (name, kept) = if e.name.ends_with(".nca")
            && !e.name.ends_with(".cnmt.nca")
            && (full || e.size < 200 << 20)
        {
            let src = fs.open(&e.name).unwrap();
            let nca = Nca::open(src.clone(), &keys, &tks).unwrap();
            let mut f = std::io::BufWriter::new(std::fs::File::create(&tmp).unwrap());
            write_ncz(&src, &nca, &mut f);
            f.flush().unwrap();
            (e.name.replace(".nca", ".ncz"), tmp)
        } else {
            let src = fs.open(&e.name).unwrap();
            let mut f = std::io::BufWriter::new(std::fs::File::create(&tmp).unwrap());
            let (mut o, total) = (0u64, src.size());
            while o < total {
                let n = (total - o).min(CHUNK as u64) as usize;
                f.write_all(&read_vec(&*src, o, n).unwrap()).unwrap();
                o += n as u64;
            }
            f.flush().unwrap();
            (e.name.clone(), tmp)
        };
        let len = std::fs::metadata(&kept).unwrap().len();
        members.push((name, kept, len));
    }
    {
        let mut strtab = Vec::new();
        let mut name_off = Vec::new();
        for (n, _, _) in &members {
            name_off.push(strtab.len() as u32);
            strtab.extend(n.as_bytes());
            strtab.push(0);
        }
        while (16 + members.len() * 24 + strtab.len()) % 16 != 0 {
            strtab.push(0);
        }
        let mut f = std::io::BufWriter::new(std::fs::File::create(&nsz_path).unwrap());
        f.write_all(b"PFS0").unwrap();
        f.write_all(&(members.len() as u32).to_le_bytes()).unwrap();
        f.write_all(&(strtab.len() as u32).to_le_bytes()).unwrap();
        f.write_all(&0u32.to_le_bytes()).unwrap();
        let mut off = 0u64;
        for (i, (_, _, len)) in members.iter().enumerate() {
            f.write_all(&off.to_le_bytes()).unwrap();
            f.write_all(&len.to_le_bytes()).unwrap();
            f.write_all(&name_off[i].to_le_bytes()).unwrap();
            f.write_all(&0u32.to_le_bytes()).unwrap();
            off += len;
        }
        f.write_all(&strtab).unwrap();
        for (_, p, _) in &members {
            std::io::copy(&mut std::fs::File::open(p).unwrap(), &mut f).unwrap();
        }
        f.flush().unwrap();
    }
    for (_, p, _) in &members {
        std::fs::remove_file(p).unwrap();
    }

    // Read it back: every NCA hashes like the NSP's, and it still parses as the same program.
    let nsz = parse_pfs0(FileSource::open(&nsz_path).unwrap()).unwrap();
    let mut converted = 0;
    for e in fs.entries.iter().filter(|e| e.name.ends_with(".nca")) {
        let want = sha(&fs.open(&e.name).unwrap());
        let ncz_name = e.name.replace(".nca", ".ncz");
        let got = if let Some(m) = nsz.entries.iter().find(|m| m.name == ncz_name) {
            converted += 1;
            assert!(
                m.size < e.size || e.size < 1 << 20,
                "{} grew: {} vs {}",
                ncz_name,
                m.size,
                e.size
            );
            sha(&NczSource::open(nsz.open(&ncz_name).unwrap()).unwrap())
        } else {
            sha(&nsz.open(&e.name).unwrap())
        };
        assert!(want == got, "{} differs after NSZ round trip", e.name);
    }
    assert!(converted >= 2, "at least the program NCA is converted");
    eprintln!(
        "nsz: {} ({} NCAs converted) at {}",
        Path::new(&nsz_path).display(),
        converted,
        nsz_path.display()
    );
}
