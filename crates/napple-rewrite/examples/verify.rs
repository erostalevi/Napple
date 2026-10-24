//! `cargo run --release --example verify -p napple-rewrite -- <module.nso>...`
//! Rewrites each extracted NSO, runs the structural check and the Unicorn equivalence harness.
#[path = "../tests/support/equiv.rs"]
mod equiv;
use napple_core::infrastructure::nso::Nso;
use napple_rewrite::image::ModuleImage;
use napple_rewrite::{check, rewrite_module, RewriteOptions};

fn main() {
    let mut bad = false;
    for path in std::env::args().skip(1) {
        let nso = Nso::parse(&std::fs::read(&path).unwrap()).unwrap();
        let t = std::time::Instant::now();
        let out = rewrite_module(&nso, &nso.build_id_hex()[..32], &RewriteOptions::default()).unwrap();
        let rw = t.elapsed();
        let orig = ModuleImage::from_nso(&nso).unwrap();
        let st = check::verify(&orig, &out);
        println!("== {path}\n  rewrite {rw:?} stats {:?} gates {} unhandled {} warnings {}", out.stats, out.gates.len(), out.unhandled.len(), out.warnings.len());
        println!("  structural: {} violations, verified {:?}", st.violations.len(), st.verified);
        for v in st.violations.iter().take(20) { println!("    VIOLATION {v}"); }
        for u in &out.unhandled { println!("    UNHANDLED {u}"); }
        let t = std::time::Instant::now();
        let rep = equiv::check_equivalence(&orig, &out, &equiv::Options::default());
        println!("  equivalence ({:?}): sites {} trials {} effective {} gate-sites {} zero-effective {} diverged {} mutations {}/{} caught",
            t.elapsed(), rep.sites, rep.trials, rep.effective, rep.gate_sites, rep.zero_effective.len(), rep.diverged.len(), rep.mutations_caught, rep.mutations);
        for a in rep.zero_effective.iter().take(10) { println!("    NOEFF {a:#x}"); }
        for (a, d) in rep.diverged.iter().take(20) { println!("    DIFF {a:#x} {d}"); }
        bad |= !st.ok() || !out.is_complete() || !rep.diverged.is_empty();
    }
    std::process::exit(bad as i32);
}
