//! Shared seeded-mutation helpers for the parser tests (included with #[path] by the robustness tests
//! of the other crates). Scale with NAPPLE_MUT_ITERS / NAPPLE_MUT_SEED.
use std::time::{Duration, Instant};

/// Tracks the largest single allocation of the current thread so a tiny input that makes a parser
/// reserve gigabytes ("huge length field") fails the test even when the allocation would succeed.
pub mod bigalloc {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;

    thread_local! {
        static MAX: Cell<usize> = const { Cell::new(0) };
    }
    pub struct Tracking;
    unsafe impl GlobalAlloc for Tracking {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            let _ = MAX.try_with(|m| m.set(m.get().max(l.size())));
            unsafe { System.alloc(l) }
        }
        unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
            let _ = MAX.try_with(|m| m.set(m.get().max(l.size())));
            unsafe { System.alloc_zeroed(l) }
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            unsafe { System.dealloc(p, l) }
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            let _ = MAX.try_with(|m| m.set(m.get().max(n)));
            unsafe { System.realloc(p, l, n) }
        }
    }
    pub fn reset() {
        MAX.with(|m| m.set(0));
    }
    pub fn max() -> usize {
        MAX.with(|m| m.get())
    }
}

#[global_allocator]
pub static ALLOC: bigalloc::Tracking = bigalloc::Tracking;

/// Largest single allocation a hostile file may cause (legit data here is < 1 MiB).
pub const ALLOC_LIMIT: usize = 128 << 20;

pub struct Rng(pub u64);
impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    pub fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            (self.next() % n as u64) as usize
        }
    }
}

pub const HUGE: [u64; 10] = [
    0,
    1,
    0x7fff_ffff,
    0x8000_0000,
    0xffff_ffff,
    0xffff_ffff_ffff_ffff,
    0x7fff_ffff_ffff_ffff,
    0x1_0000_0000,
    0xffff_fff0,
    0x00ff_ffff,
];

/// One mutated variant of `seed`. `hot` biases edits towards the first `hot` bytes (headers).
pub fn mutate(seed: &[u8], hot: usize, r: &mut Rng) -> Vec<u8> {
    let mut v = seed.to_vec();
    for _ in 0..1 + r.below(4) {
        if v.is_empty() {
            break;
        }
        let span = if r.below(4) != 0 {
            hot.min(v.len())
        } else {
            v.len()
        };
        let at = r.below(span.max(1));
        match r.below(7) {
            0 => v[at] ^= 1 << r.below(8),
            1 => v[at] = r.next() as u8,
            2 => v.truncate(r.below(v.len() + 1)),
            3 => {
                let h = HUGE[r.below(HUGE.len())].to_le_bytes();
                let w = if r.below(2) == 0 { 4 } else { 8 };
                let at = at & !3;
                for (i, b) in h.iter().take(w).enumerate() {
                    if let Some(x) = v.get_mut(at + i) {
                        *x = *b;
                    }
                }
            }
            4 => {
                let end = (at + 1 + r.below(64)).min(v.len());
                v[at..end].fill(0);
            }
            5 => {
                let from = r.below(v.len());
                let n = r.below(32).min(v.len() - from).min(v.len() - at);
                let chunk = v[from..from + n].to_vec();
                v[at..at + n].copy_from_slice(&chunk);
            }
            _ => v.extend(std::iter::repeat_n(0, r.below(4096))),
        }
    }
    v
}

pub fn iters() -> usize {
    std::env::var("NAPPLE_MUT_ITERS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(200)
}

pub fn run(name: &str, seeds: &[Vec<u8>], hot: usize, f: impl Fn(&[u8])) {
    run_with(name, seeds, &|s, r| mutate(s, hot, r), f)
}

pub fn run_with(
    name: &str,
    seeds: &[Vec<u8>],
    mutator: &dyn Fn(&[u8], &mut Rng) -> Vec<u8>,
    f: impl Fn(&[u8]),
) {
    let seed0 = std::env::var("NAPPLE_MUT_SEED")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(0x9e37_79b9u64);
    let mut r = Rng(seed0
        ^ name
            .bytes()
            .fold(0xcbf2_9ce4u64, |a, b| a.wrapping_mul(131) ^ b as u64)
        | 1);
    for s in seeds {
        f(s);
    }
    for i in 0..iters() {
        let s = &seeds[i % seeds.len()];
        let m = mutator(s, &mut r);
        let t = Instant::now();
        bigalloc::reset();
        f(&m);
        assert!(
            bigalloc::max() <= ALLOC_LIMIT,
            "{name}: case {i} allocated {} bytes at once",
            bigalloc::max()
        );
        assert!(
            t.elapsed() < Duration::from_secs(20),
            "{name}: case {i} too slow ({:?})",
            t.elapsed()
        );
    }
}
