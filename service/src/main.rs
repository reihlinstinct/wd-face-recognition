fn main() {
    let version = env!("CARGO_PKG_VERSION");
    match std::env::args().nth(1).as_deref() {
        Some("--version") => println!("wdf-service {version}"),
        _ => println!("wdf-service {version} (M1 skeleton - local API arrives in milestone 5)"),
    }
}
