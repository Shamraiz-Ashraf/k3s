#!/bin/bash

# Test script for DPDK Sender - Improved Version
# This script validates the sender improvements and provides performance comparison

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
TEST_DURATION=30
TEST_BITRATE=1000
TEST_PCAP="test.pcap"
TEST_PCI="0000:01:00.0"  # Adjust to your NIC PCI address

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to check DPDK environment
check_dpdk_env() {
    print_status "Checking DPDK environment..."
    
    if ! command_exists dpdk-devbind.py; then
        print_error "dpdk-devbind.py not found. Please install DPDK first."
        exit 1
    fi
    
    if [ ! -f /proc/sys/vm/nr_hugepages ]; then
        print_error "Hugepages not configured. Please configure hugepages for DPDK."
        exit 1
    fi
    
    HUGE_PAGES=$(cat /proc/sys/vm/nr_hugepages)
    if [ "$HUGE_PAGES" -lt 1024 ]; then
        print_warning "Hugepages count is low ($HUGE_PAGES). Recommended: 1024 or more."
    fi
    
    print_success "DPDK environment check passed"
}

# Function to create test PCAP file
create_test_pcap() {
    print_status "Creating test PCAP file..."
    
    if command_exists tcpdump; then
        # Create a simple test PCAP with ping packets
        ping -c 10 8.8.8.8 > /dev/null 2>&1 &
        sleep 2
        sudo tcpdump -i any -w "$TEST_PCAP" -c 100 icmp > /dev/null 2>&1 || true
        sleep 2
        kill %1 2>/dev/null || true
        
        if [ -f "$TEST_PCAP" ] && [ -s "$TEST_PCAP" ]; then
            print_success "Test PCAP created: $TEST_PCAP"
        else
            print_warning "Could not create test PCAP with tcpdump, using fallback method"
            create_fallback_pcap
        fi
    else
        print_warning "tcpdump not found, using fallback method"
        create_fallback_pcap
    fi
}

# Function to create fallback PCAP
create_fallback_pcap() {
    print_status "Creating fallback test PCAP..."
    
    # Create a minimal PCAP file with a simple UDP packet
    cat > create_pcap.py << 'EOF'
#!/usr/bin/env python3
import struct

# PCAP file header
pcap_header = struct.pack('<IHHiIII',
    0xa1b2c3d4,  # magic number
    2, 4,        # version major, minor
    0,           # timezone
    0,           # sigfigs
    65535,       # snaplen
    1            # network (Ethernet)
)

# Simple UDP packet (Ethernet + IP + UDP)
packet_data = (
    b'\x00\x11\x22\x33\x44\x55'  # dst MAC
    b'\x66\x77\x88\x99\xaa\xbb'  # src MAC
    b'\x08\x00'                  # EtherType (IPv4)
    b'\x45\x00\x00\x28'          # IP header
    b'\x00\x00\x40\x00\x40\x11'  # IP flags, TTL, protocol (UDP)
    b'\x00\x00'                  # IP checksum
    b'\xc0\xa8\x01\x01'          # src IP
    b'\xc0\xa8\x01\x02'          # dst IP
    b'\x12\x34\x56\x78'          # src port, dst port
    b'\x00\x14\x00\x00'          # UDP length, checksum
    b'Hello DPDK World!'          # payload
)

# PCAP packet header
packet_header = struct.pack('<IIII',
    0, 0,                        # timestamp
    len(packet_data),            # captured length
    len(packet_data)             # original length
)

with open('test.pcap', 'wb') as f:
    f.write(pcap_header)
    f.write(packet_header)
    f.write(packet_data)
EOF
    
    python3 create_pcap.py
    rm create_pcap.py
    
    if [ -f "$TEST_PCAP" ] && [ -s "$TEST_PCAP" ]; then
        print_success "Fallback test PCAP created: $TEST_PCAP"
    else
        print_error "Failed to create test PCAP file"
        exit 1
    fi
}

# Function to build applications
build_applications() {
    print_status "Building applications..."
    
    if [ ! -f "Makefile" ]; then
        print_error "Makefile not found. Please run this script from the project directory."
        exit 1
    fi
    
    make clean > /dev/null 2>&1 || true
    make build
    
    if [ -f "sender_improved" ] && [ -f "sender_cmp" ]; then
        print_success "Applications built successfully"
    else
        print_error "Failed to build applications"
        exit 1
    fi
}

# Function to find available NICs
find_nic() {
    print_status "Finding available NICs..."
    
    # Try to find a DPDK-compatible NIC
    NICS=$(sudo dpdk-devbind.py --status | grep -E "if=.*drv=.*" | head -5)
    
    if [ -z "$NICS" ]; then
        print_error "No DPDK-compatible NICs found"
        exit 1
    fi
    
    print_status "Available NICs:"
    echo "$NICS"
    
    # Use the first available NIC
    TEST_PCI=$(echo "$NICS" | head -1 | awk '{print $1}')
    print_status "Using NIC: $TEST_PCI"
}

# Function to run performance test
run_performance_test() {
    local app_name=$1
    local app_binary=$2
    
    print_status "Running performance test for $app_name..."
    
    # Start the sender in background
    sudo timeout $TEST_DURATION ./$app_binary \
        --pcap "$TEST_PCAP" \
        --replays 0 \
        --pci "$TEST_PCI" \
        --bitrate "$TEST_BITRATE" \
        --run-duration $TEST_DURATION > "${app_name}_output.log" 2>&1 &
    
    local sender_pid=$!
    
    # Wait for completion
    wait $sender_pid 2>/dev/null || true
    
    # Extract performance metrics
    if [ -f "${app_name}_output.log" ]; then
        local throughput=$(grep "Average throughput" "${app_name}_output.log" | tail -1 | awk '{print $4}')
        local pps=$(grep "Average packet rate" "${app_name}_output.log" | tail -1 | awk '{print $4}')
        local dropped=$(grep "Dropped packets" "${app_name}_output.log" | tail -1 | awk '{print $3}')
        
        echo "$app_name,$throughput,$pps,$dropped" >> performance_results.csv
        print_success "$app_name test completed"
    else
        print_error "No output log found for $app_name"
    fi
}

# Function to display results
display_results() {
    print_status "Performance Test Results:"
    echo ""
    echo "Application,Throughput (Mbps),Packet Rate (PPS),Dropped Packets"
    echo "----------,------------------,----------------,----------------"
    
    if [ -f "performance_results.csv" ]; then
        cat performance_results.csv
    else
        print_warning "No performance results found"
    fi
    
    echo ""
    print_status "Detailed logs:"
    echo "- Original sender: sender_cmp_output.log"
    echo "- Improved sender: sender_improved_output.log"
}

# Function to cleanup
cleanup() {
    print_status "Cleaning up..."
    
    # Kill any running processes
    pkill -f "sender_" 2>/dev/null || true
    
    # Remove test files
    rm -f "$TEST_PCAP" 2>/dev/null || true
    rm -f performance_results.csv 2>/dev/null || true
    
    print_success "Cleanup completed"
}

# Main execution
main() {
    echo "=========================================="
    echo "DPDK Sender Performance Test"
    echo "=========================================="
    echo ""
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then
        print_error "This script must be run as root (use sudo)"
        exit 1
    fi
    
    # Setup cleanup on exit
    trap cleanup EXIT
    
    # Run tests
    check_dpdk_env
    build_applications
    find_nic
    create_test_pcap
    
    # Initialize results file
    echo "Application,Throughput (Mbps),Packet Rate (PPS),Dropped Packets" > performance_results.csv
    
    # Run performance tests
    run_performance_test "Original" "sender_cmp"
    sleep 2
    run_performance_test "Improved" "sender_improved"
    
    # Display results
    display_results
    
    print_success "Performance test completed!"
}

# Run main function
main "$@"