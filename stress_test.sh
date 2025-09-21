#!/bin/bash

echo "Ultra-Fast HTTP Server Stress Test"
echo "=================================="

# Check dependencies
command -v wrk >/dev/null 2>&1 || { echo "Installing wrk..."; sudo apt-get install -y wrk; }

echo "Starting extreme stress test..."

# Test 1: Maximum RPS test
echo ""
echo "Test 1: Maximum RPS - 12 threads, 400 connections, 60 seconds"
echo "============================================================="
wrk -t12 -c400 -d60s --latency http://localhost:8080/

# Test 2: Memory pressure test
echo ""
echo "Test 2: Memory pressure - Stats endpoint intensive"
echo "================================================"
wrk -t8 -c200 -d30s --latency http://localhost:8080/stats

# Test 3: Ultra high concurrency
echo ""
echo "Test 3: Ultra high concurrency - 1000 connections"
echo "================================================"
wrk -t16 -c1000 -d30s --latency http://localhost:8080/

# Test 4: Burst test
echo ""
echo "Test 4: Burst pattern simulation"
echo "==============================="
for i in {1..5}; do
    echo "Burst $i/5..."
    wrk -t8 -c200 -d10s http://localhost:8080/ > /dev/null 2>&1 &
    sleep 5
    kill %1 2>/dev/null
    wait
    sleep 2
done

echo ""
echo "Final server status after stress test:"
curl -s http://localhost:8080/stats | jq '.performance, .connections, .errors' 2>/dev/null

echo ""
echo "Stress test completed!"