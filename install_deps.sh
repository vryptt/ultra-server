#!/bin/bash

echo "Installing dependencies for Ultra-Fast HTTP Server..."

# Update package lists
sudo apt-get update

# Install build essentials
sudo apt-get install -y build-essential gcc-multilib

# Install performance tools
sudo apt-get install -y linux-tools-generic htop iotop

# Install benchmarking tools
sudo apt-get install -y apache2-utils wrk jq

# Install development libraries
sudo apt-get install -y libc6-dev linux-libc-dev

# Enable performance monitoring
echo "Enabling performance monitoring..."
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# Optimize system for high performance
echo "Applying system optimizations..."

# Increase file descriptor limits
echo "* soft nofile 1048576" | sudo tee -a /etc/security/limits.conf
echo "* hard nofile 1048576" | sudo tee -a /etc/security/limits.conf

# Optimize network stack
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.core.netdev_max_backlog=5000
sudo sysctl -w net.ipv4.tcp_fin_timeout=30
sudo sysctl -w net.ipv4.tcp_keepalive_time=120
sudo sysctl -w net.ipv4.tcp_keepalive_probes=3
sudo sysctl -w net.ipv4.tcp_keepalive_intvl=10
sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 6291456"
sudo sysctl -w net.ipv4.tcp_wmem="4096 16384 4194304"

# Make network optimizations permanent
sudo tee -a /etc/sysctl.conf << EOF
# Ultra-Fast HTTP Server optimizations
net.core.somaxconn=65535
net.ipv4.tcp_max_syn_backlog=65535
net.core.netdev_max_backlog=5000
net.ipv4.tcp_fin_timeout=30
net.ipv4.tcp_keepalive_time=120
net.ipv4.tcp_keepalive_probes=3
net.ipv4.tcp_keepalive_intvl=10
net.ipv4.tcp_rmem=4096 87380 6291456
net.ipv4.tcp_wmem=4096 16384 4194304
EOF

echo "Dependencies installed and system optimized!"
echo "Please reboot for all optimizations to take effect."
echo ""
echo "Usage:"
echo "1. Run: chmod +x *.sh"
echo "2. Build: make clean && make"
echo "3. Run server: sudo ./ultra_server"
echo "4. Benchmark: ./benchmark.sh"
echo "5. Stress test: ./stress_test.sh"