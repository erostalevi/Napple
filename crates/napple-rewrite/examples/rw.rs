use napple_core::infrastructure::nso::Nso;
use napple_rewrite::{rewrite_module, RewriteOptions};
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let outdir = std::path::Path::new(&a[1]);
    for path in &a[2..] {
        let t = std::time::Instant::now();
        let nso = Nso::parse(&std::fs::read(path).unwrap()).unwrap();
        let name = std::path::Path::new(path).file_stem().unwrap().to_string_lossy().to_string();
        let out = rewrite_module(&nso, &nso.build_id_hex()[..32], &RewriteOptions::default()).unwrap();
        out.write_to(outdir, &name).unwrap();
        println!("{name}: {:?} gates={} patch={}B unhandled={} warnings={} in {:?}", out.stats, out.gates.len(), out.patch().len(), out.unhandled.len(), out.warnings.len(), t.elapsed());
        for u in out.unhandled.iter().take(40) { println!("   UNHANDLED {u}"); }
        for w in out.warnings.iter().take(10) { println!("   warn {w}"); }
    }
}
