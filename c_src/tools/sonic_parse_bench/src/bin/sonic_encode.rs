// Encode-only reference: sonic-rs serializes an already-parsed Value.
// Parsing happens once, outside the timed loop. Compare with json_encode_bench.
//
//   cargo build --release --bin sonic_encode
//   PARSE_ITERS=N target/release/sonic_encode FILE...
use sonic_rs::{to_vec, Value};
use std::hint::black_box;
use std::time::{Duration, Instant};

fn main() {
    let paths: Vec<String> = std::env::args().skip(1).collect();
    for path in &paths {
        let data = std::fs::read(path).expect("read");
        let value: Value = sonic_rs::from_slice(&data).expect("parse");
        let out_len = to_vec(&value).expect("encode").len();
        if let Ok(v) = std::env::var("PARSE_ITERS") {
            let n: u64 = v.parse().expect("PARSE_ITERS");
            for _ in 0..n {
                black_box(to_vec(black_box(&value)).unwrap());
            }
            println!("{:<28} {} encodes", path, n);
            continue;
        }
        let t0 = Instant::now();
        let mut probe = 0u64;
        while t0.elapsed() < Duration::from_millis(50) {
            black_box(to_vec(black_box(&value)).unwrap());
            probe += 1;
        }
        let n = (probe * 10).max(1);
        let mut best = f64::INFINITY;
        for _ in 0..7 {
            let s = Instant::now();
            for _ in 0..n {
                black_box(to_vec(black_box(&value)).unwrap());
            }
            best = best.min(s.elapsed().as_secs_f64() * 1e6 / n as f64);
        }
        println!("{:<28} {:>10.3} us/encode  ({} bytes out)", path, best, out_len);
    }
}
