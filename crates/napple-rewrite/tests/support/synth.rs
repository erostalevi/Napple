//! Tiny synthetic NSO modules for template tests (no game bytes involved).
#![allow(dead_code)]
use napple_core::infrastructure::nso::{Nso, Segment};

/// Module with `code` as text (a 16-byte-aligned padded segment) and small ro/data/bss.
pub fn nso(code: &[u32]) -> Nso {
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    while text.len() % 0x1000 != 0 || text.len() < 0x1000 {
        text.extend_from_slice(&0xD503_201Fu32.to_le_bytes()); // nop padding keeps the tail executable
    }
    let tl = text.len() as u32;
    Nso {
        build_id: [0xAB; 32],
        text: Segment { mem_offset: 0, data: text },
        rodata: Segment { mem_offset: tl, data: vec![0; 0x1000] },
        data: Segment { mem_offset: tl + 0x1000, data: vec![0; 0x1000] },
        bss_size: 0x1000,
    }
}
