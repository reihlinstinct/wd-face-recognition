//! wdf-bench-rust: language shootout, Rust side (issue #14).
//! Subcommands mirror bench/cpp: sha256 | exif | jpeg | http-server | sqlite
//! (http-client is shared from the C++ bench binary for fairness).

use std::time::Instant;

fn workload_sha256(path: &str, iterations: u32) {
    use sha2::{Digest, Sha256};
    let t0 = Instant::now();
    let mut total_bytes = 0u64;
    for it in 0..iterations {
        // Stream 64 KiB chunks to match the C++ and Python workloads.
        let mut file = std::fs::File::open(path).expect("open input");
        let mut hasher = Sha256::new();
        let mut buf = [0u8; 64 * 1024];
        loop {
            let n = std::io::Read::read(&mut file, &mut buf).expect("read input");
            if n == 0 {
                break;
            }
            hasher.update(&buf[..n]);
            total_bytes += n as u64;
        }
        let digest = hasher.finalize();
        if it == 0 {
            println!("digest0={:02x}{:02x}{:02x}{:02x}", digest[0], digest[1], digest[2], digest[3]);
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    println!("mb_per_sec={:.2}", total_bytes as f64 / 1e6 / secs);
}

fn workload_exif(path: &str, iterations: u32) {
    let t0 = Instant::now();
    for it in 0..iterations {
        let file = std::fs::File::open(path).expect("open input");
        let mut reader = std::io::BufReader::new(file);
        let parsed = exif::Reader::new().read_from_container(&mut reader).expect("exif parse");
        if it == 0 {
            if let Some(make) = parsed.get_field(exif::Tag::Make, exif::In::PRIMARY) {
                println!("make={}", make.display_value());
            }
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    println!("ops_per_sec={:.2}", iterations as f64 / secs);
}

fn workload_jpeg(path: &str, iterations: u32) {
    use mozjpeg::{ColorSpace, Decompress};
    let t0 = Instant::now();
    let mut pixels = 0u64;
    for _ in 0..iterations {
        // Read the file per iteration to match the C++ and Python workloads.
        let data = std::fs::read(path).expect("open input");
        let mut dec = Decompress::new_mem(&data)
            .and_then(|d| d.to_colorspace(ColorSpace::JCS_RGB))
            .expect("jpeg header");
        pixels += (dec.width() * dec.height()) as u64;
        let _buf = dec.read_scanlines::<u8>().expect("jpeg decode");
    }
    let secs = t0.elapsed().as_secs_f64();
    println!("photos_per_sec={:.3} mpix_per_sec={:.2}", iterations as f64 / secs, pixels as f64 / 1e6 / secs);
}

fn workload_http_server(port: u16) {
    let server = tiny_http::Server::http(("127.0.0.1", port)).expect("bind");
    eprintln!("listening on {port}");
    let response_body = r#"{"ok":true,"faces":3,"elapsed_ms":42}"#;
    for mut req in server.incoming_requests() {
        if req.url() == "/ready" {
            let _ = req.respond(tiny_http::Response::from_string("ready"));
            continue;
        }
        let mut body = String::new();
        let _ = req.as_reader().read_to_string(&mut body);
        let parsed: Result<serde_json::Value, _> = serde_json::from_str(&body);
        let _ = parsed;
        let response = tiny_http::Response::from_string(response_body)
            .with_header("Content-Type: application/json".parse::<tiny_http::Header>().unwrap());
        let _ = req.respond(response);
    }
}

fn workload_sqlite(dir: &str, rows: u32) {
    let db_path = format!("{dir}/bench_rust.sqlite");
    let _ = std::fs::remove_file(&db_path);
    let conn = rusqlite::Connection::open(&db_path).expect("open db");
    conn.execute_batch(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;
         CREATE TABLE faces (id INTEGER PRIMARY KEY, photo TEXT, x REAL, y REAL, embedding BLOB);",
    )
    .expect("setup");
    let t0 = Instant::now();
    let tx = conn.unchecked_transaction().expect("tx");
    {
        let mut stmt = tx
            .prepare("INSERT INTO faces (photo, x, y, embedding) VALUES (?1, ?2, ?3, zeroblob(512))")
            .expect("prepare");
        for i in 0..rows {
            stmt.execute(rusqlite::params![
                "photo_12mp.jpg",
                0.1 * (i % 100) as f64,
                0.2 * (i % 50) as f64
            ])
            .expect("insert");
        }
    }
    tx.commit().expect("commit");
    let secs = t0.elapsed().as_secs_f64();
    drop(conn);
    let _ = std::fs::remove_file(&db_path);
    println!("rows_per_sec={:.2}", rows as f64 / secs);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: {} <workload> [args]", args[0]);
        std::process::exit(2);
    }
    let iterations: u32 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(10);
    match args[1].as_str() {
        "sha256" => workload_sha256(&args[2], iterations),
        "exif" => workload_exif(&args[2], iterations),
        "jpeg" => workload_jpeg(&args[2], iterations),
        "http-server" => workload_http_server(args[2].parse().expect("port")),
        "sqlite" => workload_sqlite(&args[2], iterations),
        other => {
            eprintln!("unknown workload: {other}");
            std::process::exit(2);
        }
    }
}
