#!/bin/bash

echo "Ultra-Fast HTTP Server Benchmark"
echo "================================="

# Check if server is running
if ! curl -s http://localhost:8080/ > /dev/null; then
    echo "Error: Server is not running on port 8080"
    exit 1
fi

echo "Starting comprehensive benchmark..."

# Warm up
echo "Warming up server..."
curl -s http://localhost:8080/ > /dev/null
curl -s http://localhost:8080/stats > /dev/null

# Test 1: Root endpoint performance
echo ""
echo "Test 1: Root endpoint (/) - Maximum throughput"
echo "=============================================="
ab -n 100000 -c 100 -k http://localhost:8080/ | grep -E "(Requests per second|Time per request|Transfer rate)"

# Test 2: Stats endpoint
echo ""
echo "Test 2: Stats endpoint (/stats) - JSON performance"
echo "================================================"
ab -n 10000 -c 50 -k http://localhost:8080/stats | grep -E "(Requests per second|Time per request|Transfer rate)"

# Test 3: Mixed workload
echo ""
echo "Test 3: Mixed workload simulation"
echo "================================"
echo "Running concurrent tests..."

# Background jobs for mixed load
for i in {1..5}; do
    ab -n 20000 -c 20 -k http://localhost:8080/ > /dev/null 2>&1 &
done

for i in {1..2}; do
    ab -n 5000 -c 10 -k http://localhost:8080/stats > /dev/null 2>&1 &
done

wait

# Test 4: High concurrency
echo ""
echo "Test 4: High concurrency test"
echo "============================="
ab -n 50000 -c 500 -k http://localhost:8080/ | grep -E "(Requests per second|Time per request|Failed requests)"

# Test 5: Sustained load
echo ""
echo "Test 5: Sustained load (30 seconds)"
echo "==================================="
ab -t 30 -c 200 -k http://localhost:8080/ | grep -E "(Requests per second|Time per request|Complete requests)"

echo ""
echo "Final server statistics:"
echo "======================="
curl -s http://localhost:8080/stats | jq '.' 2>/dev/null || curl -s http://localhost:8080/stats

echo ""
echo "Benchmark completed!"