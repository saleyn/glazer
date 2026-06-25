# Go JSON Benchmark

This benchmark compares the performance of different Go JSON libraries using the same input files as the glazer Elixir benchmark.

## Libraries Tested

- **goccy/go-json**: A high-performance JSON library (https://github.com/goccy/go-json)
- **jsoniter**: A high-performance JSON library compatible with the standard library (https://github.com/json-iterator/go)
- **sonic**: ByteDance's high-performance JSON library (https://github.com/bytedance/sonic)
- **jsony**: Encoding-only JSON library with builder pattern (https://github.com/orsinium-labs/jsony)
- **stdlib/json**: Go's standard library JSON package

Note: jsony only supports encoding (not decoding), so decode results show "n/a".

## Usage

From the glazer project root:

```bash
./test/bench-go/run.sh
```

Or manually:

```bash
cd test/bench-go
go run main.go
```

## Test Data

The benchmark uses the same JSON files as the Elixir benchmark:
- `../data/twitter.json` (616.7K)
- `../data/twitter2.json` (758.0K)
- `../data/openrtb.json` (1.2K)
- `../data/esad.json` (1.3K)
- `../data/small.json` (0.1K)

## Output Format

The output matches the same format as the Elixir `mix bench_json` command:
- Numbers are shown in microseconds (μs) per operation
- Two columns per file: decode and encode performance
- Libraries are listed as rows with results for each file
- "n/a" indicates functionality not supported by the library

## Performance Results

In typical runs, the performance ranking is:
1. **sonic**: Fastest overall, especially for large files
2. **goccy/go-json**: Very fast, good all-around performance
3. **jsoniter**: Fast encoding, competitive decoding
4. **jsony**: Encode-only, slower than others for encoding arbitrary data
5. **stdlib/json**: Standard library baseline

## Dependencies

- Go 1.18+
- github.com/goccy/go-json v0.10.6
- github.com/json-iterator/go v1.1.12
- github.com/bytedance/sonic v1.15.2
- github.com/orsinium-labs/jsony v1.2.0