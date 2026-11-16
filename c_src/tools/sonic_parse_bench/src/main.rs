// Parse-only benchmark for sonic-rs: drives the push-based JsonVisitor over
// each input and counts tokens, building nothing. Measures the same thing as
// the C++ json_parse_bench harness (scanning and dispatch, no term building).
//
//   cargo build --release && target/release/sonic_parse_bench ../../../test/data/*.json
//
// Needs the torque vendor tree (deps/torque) from `rebar3 deps`/`mix deps.get`.

use sonic_rs::{parse_into_visitor, JsonVisitor};
use std::hint::black_box;
use std::time::{Duration, Instant};

#[derive(Default)]
struct Counter {
    tokens: u64,
}

// Every callback succeeds (returns true). A false return aborts the parse.
impl<'de> JsonVisitor<'de> for Counter {
    fn visit_dom_start(&mut self) -> bool { self.tokens += 1; true }
    fn visit_dom_end(&mut self) -> bool { true }
    fn visit_null(&mut self) -> bool { self.tokens += 1; true }
    fn visit_bool(&mut self, _v: bool) -> bool { self.tokens += 1; true }
    fn visit_u64(&mut self, _v: u64) -> bool { self.tokens += 1; true }
    fn visit_i64(&mut self, _v: i64) -> bool { self.tokens += 1; true }
    fn visit_f64(&mut self, _v: f64) -> bool { self.tokens += 1; true }
    fn visit_overflow_int(&mut self, _raw: &str, _f: f64) -> bool { self.tokens += 1; true }
    fn visit_raw_number(&mut self, _v: &str) -> bool { self.tokens += 1; true }
    fn visit_borrowed_raw_number(&mut self, _v: &str) -> bool { self.tokens += 1; true }
    fn visit_str(&mut self, _v: &str) -> bool { self.tokens += 1; true }
    fn visit_borrowed_str(&mut self, _v: &'de str) -> bool { self.tokens += 1; true }
    fn visit_object_start(&mut self, _hint: usize) -> bool { self.tokens += 1; true }
    fn visit_object_end(&mut self, _len: usize) -> bool { true }
    fn visit_array_start(&mut self, _hint: usize) -> bool { self.tokens += 1; true }
    fn visit_array_end(&mut self, _len: usize) -> bool { true }
    fn visit_key(&mut self, _k: &str) -> bool { self.tokens += 1; true }
    fn visit_borrowed_key(&mut self, _k: &'de str) -> bool { self.tokens += 1; true }
}

fn parse_once(data: &[u8]) -> Result<u64, String> {
    let mut c = Counter::default();
    parse_into_visitor(data, &mut c).map_err(|e| format!("{e:?}"))?;
    Ok(c.tokens)
}

fn main() {
    let paths: Vec<String> = std::env::args().skip(1).collect();
    if paths.is_empty() {
        eprintln!("usage: sonic_parse_bench FILE...");
        std::process::exit(1);
    }

    for path in &paths {
        let data = std::fs::read(path).unwrap_or_else(|e| {
            eprintln!("cannot read {path}: {e}");
            std::process::exit(1);
        });

        // PARSE_ITERS=N runs exactly N parses and skips calibration, so that
        // under callgrind both parsers run the same N (see json_parse_bench.cpp).
        if let Ok(v) = std::env::var("PARSE_ITERS") {
            let n: u64 = v.parse().expect("PARSE_ITERS must be an integer");
            for _ in 0..n {
                black_box(parse_once(black_box(&data)).unwrap());
            }
            println!("{:<28} {} parses", path, n);
            continue;
        }

        // Calibrate: run for ~50 ms to pick an iteration count.
        let t0 = Instant::now();
        let mut probe = 0u64;
        while t0.elapsed() < Duration::from_millis(50) {
            if let Err(e) = parse_once(black_box(&data)) {
                eprintln!("{path}: parse failed: {e}");
                std::process::exit(1);
            }
            probe += 1;
        }
        let n = (probe * 10).max(1);

        // Min over 7 rounds of the mean per-parse time.
        let mut best_us = f64::INFINITY;
        for _ in 0..7 {
            let start = Instant::now();
            for _ in 0..n {
                black_box(parse_once(black_box(&data)).unwrap());
            }
            let us = start.elapsed().as_secs_f64() * 1e6 / n as f64;
            best_us = best_us.min(us);
        }
        println!("{:<28} {:>12.2} us/parse  ({} bytes)", path, best_us, data.len());
    }
}
