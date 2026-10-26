use napple_core::infrastructure::nso::Nso;
use napple_rewrite::a64;
use napple_rewrite::image::ModuleImage;
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let nso = Nso::parse(&std::fs::read(&args[1]).unwrap()).unwrap();
    let img = ModuleImage::from_nso(&nso).unwrap();
    let at = u32::from_str_radix(args[2].trim_start_matches("0x"), 16).unwrap();
    let (b, f) = (
        args[3].parse::<u32>().unwrap(),
        args[4].parse::<u32>().unwrap(),
    );
    for j in (at - 4 * b..at + 4 * f).step_by(4) {
        let w = img.word(j);
        println!(
            "  {:#x}: {:08x} {}{}",
            j,
            w,
            a64::disasm(w).map(|t| t.full).unwrap_or("<?>".into()),
            if j == at { "  <==" } else { "" }
        );
    }
}
