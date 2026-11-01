//! Which NCA header versions do the Testing Material dumps carry, and do they all open? Retail files are
//! NCA3; NCA2 (launch-era dumps) and NCA0 (pre-1.0 betas) are covered by synthetic fixtures only unless
//! the folder happens to hold one, which this test reports. Skipped unless NAPPLE_TESTING_MATERIAL is set.
use aes::Aes128;
use aes::cipher::{KeyInit, generic_array::GenericArray};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{FileSource, read_vec};
use napple_core::infrastructure::nca::{Nca, TitleKeys};
use napple_core::infrastructure::pfs::parse_pfs0;
use napple_core::infrastructure::ticket::{encrypted_title_key, parse_ticket};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use xts_mode::Xts128;

fn nsps(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if p.is_dir() {
            nsps(&p, out);
        } else if p.extension().is_some_and(|x| x == "nsp") {
            out.push(p);
        }
    }
}

#[test]
fn every_nca_in_the_material_opens_and_reports_its_header_version() {
    let Some(root) = std::env::var_os("NAPPLE_TESTING_MATERIAL").map(PathBuf::from) else {
        eprintln!("NAPPLE_TESTING_MATERIAL not set: skipped");
        return;
    };
    let keys = KeySet::load(&root.join("Keys 22.1.0/prod.keys")).unwrap();
    let hk: [u8; 32] = keys.get("header_key").unwrap();
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&hk[..16])),
        Aes128::new(GenericArray::from_slice(&hk[16..])),
    );
    let mut files = Vec::new();
    nsps(&root, &mut files);
    let mut hist: BTreeMap<String, usize> = BTreeMap::new();
    let mut opened = 0;
    for f in &files {
        let Ok(src) = FileSource::open(f) else { continue };
        let Ok(fs) = parse_pfs0(src) else { continue };
        let mut tks = TitleKeys::new();
        for e in fs.entries.iter().filter(|e| e.name.ends_with(".tik")) {
            if let Ok(t) = parse_ticket(&fs.read(&e.name).unwrap())
                && let Ok(Some(k)) = encrypted_title_key(&t, &keys)
            {
                tks.insert(t.rights_id, k);
            }
        }
        for e in fs.entries.iter().filter(|e| e.name.ends_with(".nca")) {
            let s = fs.open(&e.name).unwrap();
            let mut h = read_vec(&*s, 0, 0x400).unwrap();
            for i in 0..2usize {
                xts.decrypt_sector(&mut h[i * 0x200..(i + 1) * 0x200], (i as u128).to_be_bytes());
            }
            let magic = String::from_utf8_lossy(&h[0x200..0x204]).to_string();
            *hist.entry(magic.clone()).or_default() += 1;
            // an NCA the title cannot decrypt for lack of a ticket is not a header problem
            match Nca::open(s, &keys, &tks) {
                Ok(_) => opened += 1,
                Err(e) => eprintln!("{}: {} ({magic}): {e}", f.display(), e_name(&e)),
            }
        }
    }
    eprintln!("NCA header versions across {} NSPs: {hist:?}; {opened} opened", files.len());
    assert!(!hist.is_empty(), "the material holds NCAs");
    assert!(hist.keys().all(|m| m == "NCA3" || m == "NCA2" || m == "NCA0"), "known magics only: {hist:?}");
}

fn e_name(e: &napple_core::domain::error::Error) -> &'static str {
    use napple_core::domain::error::Error::*;
    match e {
        Container(_) => "container",
        Unsupported(_) => "unsupported",
        Integrity(_) => "integrity",
        _ => "other",
    }
}
