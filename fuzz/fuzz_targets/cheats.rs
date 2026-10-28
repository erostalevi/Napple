#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    use napple_mods::cheats::{self, CheatMemory, Region};
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
    if let Ok(f) = cheats::parse(&String::from_utf8_lossy(data)) {
        for c in &f.cheats {
            let _ = cheats::run(&cheats::decode(&c.words).ops, &mut Mem(1));
        }
    }
    let w: Vec<u32> = data
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
        .collect();
    let _ = cheats::run(&cheats::decode(&w).ops, &mut Mem(7));
});
