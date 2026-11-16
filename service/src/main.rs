// --help text (issue #131): stdout, exit 0.
const HELP: &str = "usage: wdf-service [--help | --version]

Local API service for the wd-face-recognition app. M1 skeleton:
it prints a banner and exits; the local API arrives in milestone 5.

  -h, --help   show this help and exit
  --version    print the version and exit
";

fn main() {
    let version = env!("CARGO_PKG_VERSION");
    match std::env::args().nth(1).as_deref() {
        Some("--version") => println!("wdf-service {version}"),
        Some("--help" | "-h") => print!("{HELP}"),
        _ => println!("wdf-service {version} (M1 skeleton - local API arrives in milestone 5)"),
    }
}
