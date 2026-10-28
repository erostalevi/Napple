//! Seeded-mutation robustness tests for the mod-pack parsers (IPS, pchtxt, cheats, manifest, config,
//! `.napmod` zips). Seeds are the public example mod in docs/mods/examples. Errors are fine; panics,
//! hangs and huge up-front allocations are not. Scale with NAPPLE_MUT_ITERS / NAPPLE_MUT_SEED.
#[allow(dead_code)]
#[path = "../../napple-core/tests/mutlib/mod.rs"]
mod mutlib;

use mutlib::*;
use napple_mods::apply::{ModuleBytes, apply_mods_to_module, dry_run_mods_for_module};
use napple_mods::cheats::{self, CheatMemory, Region};
use napple_mods::model::{LoadedMod, Selection};
use napple_mods::{config, ips, manifest, pack, pchtxt, romfs, util, validate};
use std::path::{Path, PathBuf};

fn ex(rel: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../docs/mods/examples/synthetic-fix")
        .join(rel)
}
fn read(rel: &str) -> Vec<u8> {
    std::fs::read(ex(rel)).unwrap()
}

#[test]
fn mutate_ips() {
    let seed = read("patches/FEDCBA9876543210FEDCBA9876543210.ips");
    let wide = {
        let mut w = b"IPS32".to_vec();
        w.extend([0, 0, 0x10, 0, 0, 3, 1, 2, 3, 0, 0, 0, 0x20, 0, 0, 0, 5, 7]);
        w.extend(b"EEOF");
        w
    };
    run("ips", &[seed, wide], 64, |d| {
        if let Ok(p) = ips::parse(d) {
            let _ = ips::write(&p);
            // applying a patch whose records reach far (up to the documented 1 GiB cap) is by design
            if p.records.iter().all(|r| r.offset < (1 << 20)) {
                let mut t = vec![0u8; 4096];
                let _ = ips::apply(&p, &mut t);
            }
        }
    });
}

#[test]
fn mutate_pchtxt_and_json() {
    let seed = read("patches/fix.pchtxt");
    run("pchtxt", std::slice::from_ref(&seed), seed.len(), |d| {
        if let Ok(p) = pchtxt::parse(&String::from_utf8_lossy(d)) {
            let text = pchtxt::write(&p);
            let _ = pchtxt::parse(&text);
            let _ = pchtxt::from_json(&pchtxt::to_json(&p));
        }
    });
    let p = pchtxt::parse(&String::from_utf8_lossy(&seed)).unwrap();
    let json = serde_json::to_vec(&pchtxt::to_json(&p)).unwrap();
    run(
        "pchtxt-json",
        std::slice::from_ref(&json),
        json.len(),
        |d| {
            if let Ok(v) = serde_json::from_slice(d) {
                let _ = pchtxt::from_json(&v);
            }
        },
    );
}

struct Mem(u64);
impl CheatMemory for Mem {
    fn read(&mut self, _: Region, a: u64, w: u8) -> Option<u64> {
        self.0 = self.0.wrapping_add(a ^ w as u64);
        (a & 1 == 0).then_some(self.0)
    }
    fn write(&mut self, _: Region, a: u64, w: u8, v: u64) -> bool {
        self.0 = self.0.wrapping_add(a ^ v ^ w as u64);
        a & 3 != 3
    }
}

#[test]
fn mutate_cheats() {
    let seed = read("cheats/0123456789ABCDEF.txt");
    run("cheats", std::slice::from_ref(&seed), seed.len(), |d| {
        if let Ok(f) = cheats::parse(&String::from_utf8_lossy(d)) {
            for c in &f.cheats {
                let dec = cheats::decode(&c.words);
                let _ = cheats::run(&dec.ops, &mut Mem(1));
            }
        }
    });
    // arbitrary word streams straight into the decoder/interpreter
    let words: Vec<u8> = (0..64u32)
        .flat_map(|i| (i.wrapping_mul(0x0123_4567)).to_le_bytes())
        .collect();
    run(
        "cheat-words",
        std::slice::from_ref(&words),
        words.len(),
        |d| {
            let w: Vec<u32> = d
                .chunks_exact(4)
                .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
                .collect();
            let dec = cheats::decode(&w);
            let _ = cheats::run(&dec.ops, &mut Mem(7));
        },
    );
}

#[test]
fn mutate_manifest_config_and_names() {
    let m = read("mod.json");
    run("manifest", std::slice::from_ref(&m), m.len(), |d| {
        if let Ok((mf, _)) = manifest::parse(d) {
            let _ = manifest::check(&mf);
            let _ = manifest::require_valid(&mf);
            let _ = manifest::parse(&manifest::to_json(&mf));
        }
    });
    let c = read("config.json");
    run("config", std::slice::from_ref(&c), c.len(), |d| {
        if let Ok((cf, _)) = config::parse(&String::from_utf8_lossy(d)) {
            let _ = config::merge(&[&cf, &cf]);
        }
    });
    let names = b"patches/../a\\b/ROMFS/files/ui/title.txt.delete\0.//x:y".to_vec();
    run("names", std::slice::from_ref(&names), names.len(), |d| {
        let s = String::from_utf8_lossy(d);
        let _ = util::safe_rel_path(&s);
        let _ = util::norm_build_id(&s);
        let _ = util::hex_decode(&s);
        let _ = romfs::overlay_path(&s);
    });
}

#[test]
fn mutate_napmod_zip() {
    let dir = std::env::temp_dir().join(format!("napple-modmut-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let zip_path = dir.join("seed.napmod");
    pack::pack_dir(&ex(""), &zip_path).unwrap();
    let seed = std::fs::read(&zip_path).unwrap();
    let mp = dir.join("m.napmod");
    let module = ModuleBytes::from_flat(&[0x1F, 0x20, 0x03, 0xD5].repeat(0x400), 0x1000, 0x1800);
    let id = util::norm_build_id("FEDCBA9876543210FEDCBA9876543210").unwrap();
    run("napmod", &[seed], 4096, |d| {
        std::fs::write(&mp, d).unwrap();
        let Ok(m) = LoadedMod::open(&mp) else { return };
        let _ = m.verify_sha_manifest();
        let _ = m.load_config();
        let _ = m.thumbnail_ok();
        let _ = validate::validate(&m);
        let _ = romfs::build_overlay(&[&m]);
        let sel = Selection {
            enabled: true,
            code_patches: true,
            cheats: true,
            shaders: true,
            ..Default::default()
        };
        if let Ok(r) = m.resolve(sel) {
            let _ = r.cheats_for(&id);
            let rs = [r];
            let _ = dry_run_mods_for_module(&id, &module, &rs);
            let mut mm = module.clone();
            let _ = apply_mods_to_module(&id, &mut mm, &rs);
        }
    });
    let _ = std::fs::remove_dir_all(&dir);
}
