#!/bin/bash

# Enable error handling
set -e

echo "Starting comprehensive test script..."

# Global arrays for process tracking
declare -a SERVER_PIDS
declare -a SUPPLIER_PIDS
declare -a REQUESTER_PIDS

# Function to check if a process is running
is_running() {
    ps -p $1 > /dev/null 2>&1
    return $?
}

# Function to wait for server to start and verify it's running
wait_for_server() {
    local pid=$1
    local retries=5
    while [ $retries -gt 0 ]; do
        if is_running $pid; then
            sleep 2
            return 0
        fi
        sleep 1
        ((retries--))
    done
    echo "Server failed to start"
    return 1
}

# Enhanced cleanup function
cleanup() {
    echo "Cleaning up processes..."
    
    # Cleanup requesters
    for pid in "${REQUESTER_PIDS[@]}"; do
        if is_running $pid; then
            echo "Stopping requester (PID: $pid)"
            kill -SIGINT $pid 2>/dev/null || true
        fi
    done
    
    # Cleanup suppliers
    for pid in "${SUPPLIER_PIDS[@]}"; do
        if is_running $pid; then
            echo "Stopping supplier (PID: $pid)"
            kill -SIGINT $pid 2>/dev/null || true
        fi
    done
    
    # Cleanup servers
    for pid in "${SERVER_PIDS[@]}"; do
        if is_running $pid; then
            echo "Stopping server (PID: $pid)"
            kill -SIGINT $pid 2>/dev/null || true
        fi
    done
    
    # Final cleanup with pkill
    pkill -f "drinks_bar" 2>/dev/null || true
    pkill -f "atom_supplier" 2>/dev/null || true
    pkill -f "molecule_requester" 2>/dev/null || true
    
    sleep 2
    
    # Clear arrays
    SERVER_PIDS=()
    SUPPLIER_PIDS=()
    REQUESTER_PIDS=()
}

# Initialize test environment
init_test() {
    echo "Initializing test environment..."
    make clean
    make all
    cleanup
}

# Function to start server and wait for it
start_server() {
    local tcp_port=$1
    local udp_port=$2
    shift 2
    local extra_args=$@
    
    echo "Starting server: TCP=$tcp_port, UDP=$udp_port, Args: $extra_args"
    ./drinks_bar -T $tcp_port -U $udp_port $extra_args &
    local pid=$!
    SERVER_PIDS+=($pid)
    wait_for_server $pid
    return $?
}

# Function to start supplier
start_supplier() {
    local host=$1
    local port=$2
    
    echo "Starting supplier: $host:$port"
    ./atom_supplier $host $port &
    local pid=$!
    SUPPLIER_PIDS+=($pid)
    sleep 1
    return $?
}

# Function to start requester
start_requester() {
    local host=$1
    local port=$2
    local molecule=$3
    
    echo "Starting requester: $host:$port, Molecule: $molecule"
    ./molecule_requester $host $port "$molecule" &
    local pid=$!
    REQUESTER_PIDS+=($pid)
    sleep 1
    return $?
}

# Test server error cases
test_server_errors() {
    echo "Testing server error cases..."
    
    echo "Test: No arguments"
    ./drinks_bar || true
    sleep 1
    
    echo "Test: Invalid TCP port"
    ./drinks_bar -T 100000 -U 60000 -f atoms.txt || true
    sleep 1
    
    echo "Test: Invalid UDP port"
    ./drinks_bar -T 50000 -U 100000 -f atoms.txt || true
    sleep 1
    
    echo "Test: Missing file"
    ./drinks_bar -T 50000 -U 60000 -f nonexistent.txt || true
    sleep 1
    
    echo "Test: Invalid timeout"
    ./drinks_bar -T 50000 -U 60000 -t 0 -f atoms.txt || true
    sleep 1
}

# Test valid server scenarios
test_server_valid() {
    echo "Testing valid server scenarios..."
    
    # Create test atoms file
    echo "10 20 30" > test_atoms.txt
    
    # Start server with file
    start_server 50000 60000 "-f test_atoms.txt"
    
    # Start multiple suppliers
    start_supplier "127.0.0.1" 50000
    start_supplier "127.0.0.1" 50000
    
    # Test various molecules
    start_requester "127.0.0.1" 60000 "H2O"
    sleep 2
    start_requester "127.0.0.1" 60000 "CO2"
    sleep 2
    start_requester "127.0.0.1" 60000 "CH4"
    
    # Let them interact
    sleep 10
    
    # Cleanup
    cleanup
}

# Test timeout functionality
test_server_timeout() {
    echo "Testing server timeout..."
    
    # Start server with timeout
    start_server 50000 60000 "-t 5 -f atoms.txt"
    
    # Start one supplier and requester
    start_supplier "127.0.0.1" 50000
    start_requester "127.0.0.1" 60000 "H2O"
    
    # Wait for timeout
    sleep 7
    
    # Cleanup
    cleanup
}

# Main test sequence
main() {
    trap cleanup EXIT
    
    init_test
    
    # Run test cases
    test_server_errors
    sleep 2
    
    test_server_valid
    sleep 2
    
    test_server_timeout
    sleep 2
    
    # Generate coverage report
    echo "Generating coverage report..."
    make gcov
    
    # Display coverage summary
    echo "Coverage Summary:"
    for file in *.gcov; do
        echo "=== $file ==="
        grep -A1 "Lines executed" "$file"
    done
    
    # Final cleanup
    cleanup
    rm -f test_atoms.txt
    make clean
}

# Run the test suite
main
