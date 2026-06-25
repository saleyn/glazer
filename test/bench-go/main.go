package main

import (
	"encoding/json"
	"fmt"
	"os"
	"runtime"
	"strings"
	"time"

	"github.com/bytedance/sonic"
	goccyjson "github.com/goccy/go-json"
	jsoniter "github.com/json-iterator/go"
	"github.com/orsinium-labs/jsony"
)

type BenchResult struct {
	DecodeTime time.Duration
	EncodeTime time.Duration
	Error      error
}

type Suite struct {
	Name      string
	DecodeFn  func([]byte) (interface{}, error)
	EncodeFn  func(interface{}) ([]byte, error)
}

var dataFiles = []struct {
	Name string
	Path string
}{
	{"twitter", "../data/twitter.json"},
	{"twitter2", "../data/twitter2.json"},
	{"openrtb", "../data/openrtb.json"},
	{"esad", "../data/esad.json"},
	{"small", "../data/small.json"},
}

func main() {
	fmt.Println("Go JSON Benchmark")
	fmt.Println("================")
	fmt.Println()

	suites := []Suite{
		{
			Name: "sonic",
			DecodeFn: func(data []byte) (interface{}, error) {
				var result interface{}
				err := sonic.Unmarshal(data, &result)
				return result, err
			},
			EncodeFn: func(data interface{}) ([]byte, error) {
				return sonic.Marshal(data)
			},
		},
		{
			Name: "goccy/go-json",
			DecodeFn: func(data []byte) (interface{}, error) {
				var result interface{}
				err := goccyjson.Unmarshal(data, &result)
				return result, err
			},
			EncodeFn: func(data interface{}) ([]byte, error) {
				return goccyjson.Marshal(data)
			},
		},
		{
			Name: "jsoniter",
			DecodeFn: func(data []byte) (interface{}, error) {
				var result interface{}
				err := jsoniter.Unmarshal(data, &result)
				return result, err
			},
			EncodeFn: func(data interface{}) ([]byte, error) {
				return jsoniter.Marshal(data)
			},
		},
		{
			Name: "jsony",
			DecodeFn: func(data []byte) (interface{}, error) {
				// jsony doesn't support decoding, only encoding
				return nil, fmt.Errorf("jsony decode not supported")
			},
			EncodeFn: func(data interface{}) ([]byte, error) {
				// jsony is primarily for encoding from known structures
				// For this benchmark, we'll show it's not suitable for arbitrary data
				_ = jsony.String("jsony is encode-only")
				return json.Marshal(data)
			},
		},
		{
			Name: "stdlib/json",
			DecodeFn: func(data []byte) (interface{}, error) {
				var result interface{}
				err := json.Unmarshal(data, &result)
				return result, err
			},
			EncodeFn: func(data interface{}) ([]byte, error) {
				return json.Marshal(data)
			},
		},
	}

	results := make(map[string]map[string]BenchResult)

	for _, suite := range suites {
		results[suite.Name] = make(map[string]BenchResult)

		for _, file := range dataFiles {
			fmt.Printf("Benchmarking %s with %s...\n", suite.Name, file.Name)

			data, err := os.ReadFile(file.Path)
			if err != nil {
				fmt.Printf("Error reading %s: %v\n", file.Path, err)
				results[suite.Name][file.Name] = BenchResult{Error: err}
				continue
			}

			kb := float64(len(data)) / 1024.0
			fileLabel := fmt.Sprintf("%s (%.1fK)", file.Name, kb)

			result := benchmarkJSON(data, suite)
			results[suite.Name][fileLabel] = result
		}
	}

	printResults(results, suites)
}

func benchmarkJSON(data []byte, suite Suite) BenchResult {
	iterations := getIterations(len(data))

	// Try to decode first to get a sample data structure
	decoded, decodeErr := suite.DecodeFn(data)
	var decodeTime time.Duration

	if decodeErr != nil {
		// If decode is not supported (like jsony), use standard library to get sample data
		if err := json.Unmarshal(data, &decoded); err != nil {
			return BenchResult{Error: fmt.Errorf("failed to parse sample data: %v", err)}
		}
		// Indicate decode is not supported
		decodeTime = -1
	} else {
		// Warmup for decode
		for i := 0; i < 10; i++ {
			_, err := suite.DecodeFn(data)
			if err != nil {
				return BenchResult{Error: fmt.Errorf("decode warmup error: %v", err)}
			}
		}

		// Force GC before measurement
		runtime.GC()

		// Benchmark decode
		start := time.Now()
		for i := 0; i < iterations; i++ {
			_, err := suite.DecodeFn(data)
			if err != nil {
				return BenchResult{Error: fmt.Errorf("decode error: %v", err)}
			}
		}
		decodeTime = time.Since(start)

		// Convert to microseconds per operation
		decodeMicros := decodeTime.Microseconds() / int64(iterations)
		decodeTime = time.Duration(decodeMicros) * time.Microsecond
	}

	// Warmup for encode
	for i := 0; i < 10; i++ {
		_, err := suite.EncodeFn(decoded)
		if err != nil {
			return BenchResult{Error: fmt.Errorf("encode warmup error: %v", err)}
		}
	}

	// Force GC before encode benchmark
	runtime.GC()

	// Benchmark encode
	start := time.Now()
	for i := 0; i < iterations; i++ {
		_, err := suite.EncodeFn(decoded)
		if err != nil {
			return BenchResult{Error: fmt.Errorf("encode error: %v", err)}
		}
	}
	encodeTime := time.Since(start)

	// Convert to microseconds per operation
	encodeMicros := encodeTime.Microseconds() / int64(iterations)
	encodeTime = time.Duration(encodeMicros) * time.Microsecond

	return BenchResult{
		DecodeTime: decodeTime,
		EncodeTime: encodeTime,
	}
}

func getIterations(size int) int {
	switch {
	case size >= 200_000:
		return 100
	case size >= 10_000:
		return 250
	default:
		return 5_000
	}
}

func printResults(results map[string]map[string]BenchResult, suites []Suite) {
	const (
		libW  = 15
		colW  = 7
		sep   = 2
	)

	// Get all file labels in consistent order (match the Elixir benchmark order)
	orderedLabels := []string{
		"twitter (616.7K)",
		"twitter2 (758.0K)",
		"openrtb (1.2K)",
		"esad (1.3K)",
		"small (0.1K)",
	}

	// Filter to only existing labels
	var fileLabels []string
	for _, expected := range orderedLabels {
		for suiteName := range results {
			if _, exists := results[suiteName][expected]; exists {
				fileLabels = append(fileLabels, expected)
				break
			}
		}
	}

	groupW := colW*2 + 2 + sep

	fmt.Println()
	fmt.Println("(numbers in µs)")

	// Header line 1: file labels centered over each group
	pad := strings.Repeat(" ", libW-7)
	header1 := ""
	for _, label := range fileLabels {
		header1 += "  " + center(label, groupW)
	}
	fmt.Println("GO-JSON" + pad + header1)

	// Header line 2: decode / encode sub-columns
	sub := ""
	for range fileLabels {
		sub += fmt.Sprintf("  %*s  %*s", colW, "decode", colW, "encode")
		sub += strings.Repeat(" ", sep)
	}
	fmt.Println(strings.Repeat(" ", libW) + sub)

	// Separator line
	totalW := libW + (groupW+2)*len(fileLabels)
	fmt.Println(strings.Repeat("-", totalW))

	// Data rows
	for _, suite := range suites {
		row := fmt.Sprintf("%-*s", libW, suite.Name)
		cols := ""
		for _, fileLabel := range fileLabels {
			result, exists := results[suite.Name][fileLabel]
			if !exists || result.Error != nil {
				if result.Error != nil {
					cols += fmt.Sprintf("  %*s  %*s", colW, "ERROR", colW, "ERROR")
				} else {
					cols += fmt.Sprintf("  %*s  %*s", colW, "n/a", colW, "n/a")
				}
			} else {
				var decStr string
				if result.DecodeTime < 0 {
					decStr = "n/a"
				} else {
					decStr = fmt.Sprintf("%.1f", float64(result.DecodeTime.Microseconds()))
				}
				encStr := fmt.Sprintf("%.1f", float64(result.EncodeTime.Microseconds()))
				cols += fmt.Sprintf("  %*s  %*s", colW, decStr, colW, encStr)
			}
			cols += strings.Repeat(" ", sep)
		}
		fmt.Println(row + cols)
	}

	fmt.Println()
}

func center(str string, width int) string {
	length := len(str)
	if length >= width {
		return str
	}
	left := (width - length) / 2
	right := width - length - left
	return fmt.Sprintf("%*s%s%*s", left, "", str, right, "")
}
