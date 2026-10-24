fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = napple_rewrite::run(&args) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}
