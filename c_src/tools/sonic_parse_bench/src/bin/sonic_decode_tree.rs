// Tree decode reference: sonic-rs parses each input into a Value tree, which
// allocates for objects, arrays and strings. That is the closest analogue of
// glazer's decoder building Erlang terms. Timing matches sonic_parse_bench.
//
//   cargo build --release --bin sonic_decode_tree
//   target/release/sonic_decode_tree FILE...        (timed)
//   PARSE_ITERS=N target/release/sonic_decode_tree FILE...  (fixed count)
use sonic_rs::Value;
use std::hint::black_box;
use std::time::{Duration, Instant};

fn main() {
    let paths: Vec<String> = std::env::args().skip(1).collect();
    for path in &paths {
        let data = std::fs::read(path).expect("read");
        // Checked once, outside the timed loop.
        black_box(sonic_rs::from_slice::<Value>(&data).expect("parse"));
        if let Ok(v) = std::env::var("PARSE_ITERS") {
            let n: u64 = v.parse().expect("PARSE_ITERS");
            for _ in 0..n {
                black_box(sonic_rs::from_slice::<Value>(black_box(&data)).unwrap());
            }
            println!("{:<28} {} decodes", path, n);
            continue;
        }
        let t0 = Instant::now();
        let mut probe = 0u64;
        while t0.elapsed() < Duration::from_millis(50) {
            black_box(sonic_rs::from_slice::<Value>(black_box(&data)).unwrap());
            probe += 1;
        }
        let n = (probe * 10).max(1);
        let mut best = f64::INFINITY;
        for _ in 0..7 {
            let s = Instant::now();
            for _ in 0..n {
                black_box(sonic_rs::from_slice::<Value>(black_box(&data)).unwrap());
            }
            best = best.min(s.elapsed().as_secs_f64() * 1e6 / n as f64);
        }
        println!("{:<28} {:>10.3} us/decode  ({} bytes)", path, best, data.len());
    }
}
