#!/bin/bash

# Navigate to the benchmark directory
cd "$(dirname "$0")"

# Build and run the Go benchmark
echo "Building Go JSON benchmark..."
go build -o bench main.go

if [ $? -eq 0 ]; then
    echo "Running benchmark..."
    ./bench
else
    echo "Build failed"
    exit 1
fi