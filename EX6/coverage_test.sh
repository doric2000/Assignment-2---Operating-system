#!/usr/bin/env bash
#
# coverage_test.sh
#
# Goal: achieve near-100% gcov coverage on:
#   - drinks_bar.c
#   - atom_supplier.c
#   - molecule_requester.c
#
# Steps:
#   0. Remove any old *.gcno / *.gcda / *.gcov
#   1. Compile each .c with -fprofile-arcs -ftest-coverage
#   2. Run all tests (during which each *_dbg program emits its own .gcda)
#      → including “dummy” servers so clients actually connect/send as needed
#   3. Rename all newly-created *.gcno and *.gcda from "<bin>-<source>.*" → "<source>.*"
#   4. Run gcov → *.gcov
#
# Usage:
#   chmod +x coverage_test.sh
#   ./coverage_test.sh
#
set -euo pipefail

############################
# 0. Variables & cleanup
############################

DRINKS_SRC="drinks_bar.c"
ATOM_SRC="atom_supplier.c"
MOL_SRC="molecule_requester.c"

DRINKS_BIN="drinks_bar_dbg"
ATOM_BIN="atom_supplier_dbg"
MOL_BIN="molecule_requester_dbg"

# Base ports for drinks_bar tests
TCP_BASE=50000
UDP_BASE=60000

FIFO_STDIN="/tmp/pipe_for_stdin"
UDS_STREAM="/tmp/test_stream.sock"
UDS_DGRAM="/tmp/test_dgram.sock"

ATOM_FILE_GOOD="atoms_good.txt"
ATOM_FILE_BAD1="atoms_bad1.txt"
ATOM_FILE_BAD2="atoms_bad2.txt"
ATOM_FILE_BAD3="atoms_bad3.txt"  # negative‐numbers case

# Ports for dummy “echo” servers (for atom_supplier & molecule_requester)
ATOM_TCP_ECHO_PORT=50010
MOL_UDP_ECHO_PORT=60010

echo "---- Step 0: Removing any old *.gcno / *.gcda / *.gcov ----"
rm -f ./*.gcno ./*.gcda ./*.gcov
echo "---- Old coverage data removed ----"
echo

############################
# 1. Compile each .c with coverage flags
############################

CFLAGS="-std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -fprofile-arcs -ftest-coverage"

echo "---- Step 1: Compiling with coverage flags ----"
gcc $CFLAGS -o "$DRINKS_BIN" "$DRINKS_SRC"   -lpthread
gcc $CFLAGS -o "$ATOM_BIN"   "$ATOM_SRC"
gcc $CFLAGS -o "$MOL_BIN"    "$MOL_SRC"
echo "---- Compilation complete ----"
echo

############################
# 2. Helper functions for drinks_bar_dbg (so STDIN never closes)
############################

# Global for drinks_bar_dbg server PID
SERVER_PID=0

run_drinks() {
    #
    # Usage: run_drinks "<arguments for drinks_bar_dbg>"
    #
    # We funnel drinks_bar_dbg’s stdin from a FIFO so that we can close it cleanly
    # (causing EOF) rather than SIGKILL.
    #
    local args="$*"
    [[ -p "$FIFO_STDIN" ]] || mkfifo "$FIFO_STDIN"

    ./"$DRINKS_BIN" $args < "$FIFO_STDIN" &
    SERVER_PID=$!
    sleep 0.2   # give it a moment to bind sockets

    # Keep the write-end open on FD 3 so server’s stdin is not EOF until we want it.
    exec 3> "$FIFO_STDIN"
}

stop_drinks() {
    #
    # Close FD 3 → server sees EOF on its stdin → it prints “Console closed …” and exits cleanly.
    # If it still lingers, send SIGTERM after a brief wait.
    #
    if [[ $SERVER_PID -ne 0 ]]; then
        exec 3>&- 2>/dev/null || true   # close FIFO write-end → EOF to server’s read-end

        # Give it a moment to exit on its own
        for i in {1..10}; do
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done

        # If still alive, send SIGTERM
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -SIGTERM "$SERVER_PID" 2>/dev/null || true
            sleep 0.2
        fi

        SERVER_PID=0
    fi
    sleep 0.1
}

############################
# 3. Run all tests
############################

########################
# 3a. atom_supplier_dbg branch coverage (incl. dummy TCP, IPv6 & UDS servers)
########################

echo "========================================"
echo "3a. atom_supplier_dbg branch coverage"
echo "========================================"

# (3a.1) No arguments → Usage error
./"$ATOM_BIN" < /dev/null || true

# (3a.2) Only -h 127.0.0.1 (missing -p) → Usage
./"$ATOM_BIN" -h 127.0.0.1 < /dev/null || true

# (3a.3) Only -p 50000 (missing -h) → Usage
./"$ATOM_BIN" -p 50000 < /dev/null || true

# (3a.4) -h without value → getopt error → Usage
./"$ATOM_BIN" -h < /dev/null || true

# (3a.5) -p without value → getopt error → Usage
./"$ATOM_BIN" -p < /dev/null || true

# (3a.6) -f without value → Usage
./"$ATOM_BIN" -f < /dev/null || true

# (3a.7) Invalid hostname → getaddrinfo fails
./"$ATOM_BIN" -h no.such.host -p 50000 < /dev/null || true

# (3a.8) Valid host but unreachable port → connect refused
./"$ATOM_BIN" -h 127.0.0.1 -p 99999 < /dev/null || true

# (3a.9) Both -h and -f present (conflict) → explicit error
./"$ATOM_BIN" -h 127.0.0.1 -p 50000 -f somefile < /dev/null || true
./"$ATOM_BIN" -f somefile -h 127.0.0.1 -p 50000 < /dev/null || true

# (3a.10) UDS_STREAM‐only invocation (invalid path) → connect fails
./"$ATOM_BIN" -f /tmp/nonexistent_stream.sock < /dev/null || true

# Now exercise the “successful” TCP path (IPv4):
nc -l -p "$ATOM_TCP_ECHO_PORT" >/dev/null 2>&1 &
TCP_ECHO_PID=$!
sleep 0.1

# (3a.11) Valid TCP connection → should print “client (TCP): connected to …”
timeout 1s ./"$ATOM_BIN" -h 127.0.0.1 -p "$ATOM_TCP_ECHO_PORT" < /dev/null || true

wait "$TCP_ECHO_PID" 2>/dev/null || true

# Cover the IPv6 path:
nc -l -6 -p "$ATOM_TCP_ECHO_PORT" >/dev/null 2>&1 &
TCP6_ECHO_PID=$!
sleep 0.1

# (3a.12) Valid IPv6 TCP → hits AF_INET6 branch
timeout 1s ./"$ATOM_BIN" -h ::1 -p "$ATOM_TCP_ECHO_PORT" < /dev/null || true

kill "$TCP6_ECHO_PID" 2>/dev/null || true

# Next, exercise UDS_STREAM “success” path:
ATOM_UDS_PATH="/tmp/atom_stream.sock"
[[ -e "$ATOM_UDS_PATH" ]] && rm -f "$ATOM_UDS_PATH"

nc -lU "$ATOM_UDS_PATH" >/dev/null 2>&1 &
UDSSTREAM_ECHO_PID=$!
sleep 0.1

# (3a.13) Valid UDS_STREAM connection:
timeout 1s ./"$ATOM_BIN" -f "$ATOM_UDS_PATH" < /dev/null || true

kill "$UDSSTREAM_ECHO_PID" 2>/dev/null || true
rm -f "$ATOM_UDS_PATH"

echo "---- atom_supplier_dbg run complete ----"
echo

########################
# 3b. molecule_requester_dbg branch coverage (incl. dummy UDP, IPv6 & UDS servers)
########################

echo "========================================"
echo "3b. molecule_requester_dbg branch coverage"
echo "========================================"

# (3b.1) No arguments → Usage
./"$MOL_BIN" < /dev/null || true

# (3b.2) Only -h 127.0.0.1 (missing -p) → Usage
./"$MOL_BIN" -h 127.0.0.1 < /dev/null || true

# (3b.3) Only -p 60000 (missing -h) → Usage
./"$MOL_BIN" -p 60000 < /dev/null || true

# (3b.4) -h without value → getopt error
./"$MOL_BIN" -h < /dev/null || true

# (3b.5) -p without value → getopt error
./"$MOL_BIN" -p < /dev/null || true

# (3b.6) -f without value → Usage
./"$MOL_BIN" -f < /dev/null || true

# (3b.7) Invalid hostname → getaddrinfo fails
./"$MOL_BIN" -h no.such.host -p 60000 < /dev/null || true

# (3b.8) Valid host but unreachable port → UDP “timeout” or error
timeout 1s ./"$MOL_BIN" -h 127.0.0.1 -p 99999 < /dev/null || true

# (3b.9) Both -h and -f present (conflict) → explicit error
./"$MOL_BIN" -h 127.0.0.1 -p 60000 -f somefile < /dev/null || true
./"$MOL_BIN" -f somefile -h 127.0.0.1 -p 60000 < /dev/null || true

# (3b.10) UDS_DGRAM‐only invocation (invalid path) → bind/bind‐error
./"$MOL_BIN" -f /tmp/nonexistent_dgram.sock < /dev/null || true

# Now exercise the “successful” UDP path (IPv4):
nc -u -l -p "$MOL_UDP_ECHO_PORT" -k >/dev/null 2>&1 &
UDP_ECHO_PID=$!
sleep 0.1

# (3b.11) Valid UDP send/receive
printf "DELIVER GLUCOSE 0\n" | timeout 1s ./"$MOL_BIN" -h 127.0.0.1 -p "$MOL_UDP_ECHO_PORT" || true

kill "$UDP_ECHO_PID" 2>/dev/null || true

# Cover the IPv6 UDP branch:
nc -u -l -6 -p "$MOL_UDP_ECHO_PORT" -k >/dev/null 2>&1 &
UDP6_ECHO_PID=$!
sleep 0.1

# (3b.12) Valid IPv6 UDP send/receive
printf "DELIVER WATER 0\n" | timeout 1s ./"$MOL_BIN" -h ::1 -p "$MOL_UDP_ECHO_PORT" || true

kill "$UDP6_ECHO_PID" 2>/dev/null || true

# Next: exercise UDS_DGRAM “success” path:
MOL_UDS_DGRAM_PATH="/tmp/mol_dgram.sock"
[[ -e "$MOL_UDS_DGRAM_PATH" ]] && rm -f "$MOL_UDS_DGRAM_PATH"

python3 - << 'EOF' &
import socket, os
path = "/tmp/mol_dgram.sock"
if os.path.exists(path): os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
while True:
    data, addr = s.recvfrom(1024)
    s.sendto(data, addr)
EOF
UDS_DGRAM_ECHO_PID=$!
sleep 0.2

# (3b.13) Valid UDS_DGRAM send/receive:
printf "DELIVER WATER 0\n" | timeout 1s ./"$MOL_BIN" -f "$MOL_UDS_DGRAM_PATH" || true

kill "$UDS_DGRAM_ECHO_PID" 2>/dev/null || true
rm -f "$MOL_UDS_DGRAM_PATH"

echo "---- molecule_requester_dbg run complete ----"
echo

########################
# 3c. drinks_bar_dbg – usage and invalid options
########################

echo "========================================"
echo "3c. drinks_bar_dbg – usage and invalid options"
echo "========================================"

# (3c.1) No arguments → Usage
./"$DRINKS_BIN" < /dev/null || true

# (3c.2) Missing -T or -U → Usage
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 5000 < /dev/null || true
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -U 6000 < /dev/null || true

# (3c.3) Missing one of -c, -o, -h → Usage
./"$DRINKS_BIN" -o 1 -h 1 -T 5000 -U 6000 < /dev/null || true
./"$DRINKS_BIN" -c 1 -h 1 -T 5000 -U 6000 < /dev/null || true
./"$DRINKS_BIN" -c 1 -o 1 -T 5000 -U 6000 < /dev/null || true

# (3c.4) Invalid/out-of-range ports
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 99999 -U 6000 < /dev/null || true
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 5000 -U 99999 < /dev/null || true

# (3c.5) Both -s and -d provided (conflict) → error
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 5000 -U 6000 -s /tmp/foo -d /tmp/bar < /dev/null || true

# (3c.6) -f without value → Usage
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 5000 -U 6000 -f < /dev/null || true

# (3c.7) -t without value → getopt error → Usage
./"$DRINKS_BIN" -c 1 -o 1 -h 1 -T 5000 -U 6000 -t < /dev/null || true

echo "---- drinks_bar_dbg usage tests complete ----"
echo

########################
# 3d. Prepare “atoms” files for the -f tests
########################

echo "---- Preparing file-based tests ----"
echo "5 5 5"       > "$ATOM_FILE_GOOD"    # valid “5 5 5”
> "$ATOM_FILE_BAD1"                       # empty
echo "hello world" > "$ATOM_FILE_BAD2"    # invalid text
echo "-1 -1 -1"   > "$ATOM_FILE_BAD3"     # negative numbers
echo "Files ready:"
ls -l "$ATOM_FILE_GOOD" "$ATOM_FILE_BAD1" "$ATOM_FILE_BAD2" "$ATOM_FILE_BAD3"
echo

########################
# 3e. drinks_bar_dbg – file load/save (-f)
########################

echo "========================================"
echo "3e. drinks_bar_dbg – file load/save (-f)"
echo "========================================"

# (3e.1) Nonexistent file → server must create and write back “2 3 4”
rm -f atoms_new.txt
run_drinks "-c 2 -o 3 -h 4 -T 7000 -U 7001 -f atoms_new.txt"
stop_drinks
echo "→ atoms_new.txt now contains:"
cat atoms_new.txt || true
echo

# (3e.2) Empty file → treat as nonexistent → initialize to (7,8,9)
run_drinks "-c 7 -o 8 -h 9 -T 7000 -U 7001 -f $ATOM_FILE_BAD1"
stop_drinks
echo "→ $ATOM_FILE_BAD1 now contains:"
cat "$ATOM_FILE_BAD1" || true
echo

# (3e.3) Invalid text “hello world” → treat as nonexistent → initialize to (1,2,3)
run_drinks "-c 1 -o 2 -h 3 -T 7000 -U 7001 -f $ATOM_FILE_BAD2"
stop_drinks
echo "→ $ATOM_FILE_BAD2 now contains:"
cat "$ATOM_FILE_BAD2" || true
echo

# (3e.4) Valid file “5 5 5” → load that, then stop (no change if no ADD)
run_drinks "-c 0 -o 0 -h 0 -T 7000 -U 7001 -f $ATOM_FILE_GOOD"
stop_drinks
echo "→ $ATOM_FILE_GOOD still contains:"
cat "$ATOM_FILE_GOOD" || true
echo

# (3e.5) Negative‐numbers file “-1 -1 -1” → treated as too small → init (4,5,6)
run_drinks "-c 4 -o 5 -h 6 -T 7000 -U 7001 -f $ATOM_FILE_BAD3"
stop_drinks
echo "→ $ATOM_FILE_BAD3 now contains (initialized to 4 5 6):"
cat "$ATOM_FILE_BAD3" || true
echo

########################
# 3f. drinks_bar_dbg – Stage 1: ADD via TCP
########################

echo "========================================"
echo "3f. drinks_bar_dbg – Stage 1: ADD via TCP"
echo "========================================"

# Start with inventory=0,0,0
printf "0 0 0" > "$ATOM_FILE_GOOD"

run_drinks "-c 0 -o 0 -h 0 -T $TCP_BASE -U $UDP_BASE -f $ATOM_FILE_GOOD"

# (3f.1) “ADD” alone → missing arguments
printf "ADD\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.2) “FOO BAR 1” → invalid command
printf "FOO BAR 1\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.3) “ADD NEON 5” → invalid atom type
printf "ADD NEON 5\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.4) “ADD CARBON xyz” → invalid number
printf "ADD CARBON xyz\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.5) “ADD OXYGEN 10000000000000000000” → “ERROR: number too large”
printf "ADD OXYGEN 10000000000000000000\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.6) “ADD CARBON 1000000000000000000” → “ERROR: capacity exceeded”
printf "ADD CARBON 1000000000000000000\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

# (3f.7) Valid “ADD HYDROGEN 42”
printf "ADD HYDROGEN 42\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true

stop_drinks

echo "---- Stage 1 (ADD via TCP) complete ----"
echo

########################
# 3g. drinks_bar_dbg – Stage 2: DELIVER via UDP
########################

echo "========================================"
echo "3g. drinks_bar_dbg – Stage 2: DELIVER via UDP"
echo "========================================"

# Start fresh (inventory=0,0,0)
printf "0 0 0" > "$ATOM_FILE_GOOD"
run_drinks "-c 0 -o 0 -h 0 -T $TCP_BASE -U $UDP_BASE -f $ATOM_FILE_GOOD"

# (3g.1) “DELIVER” alone → missing arguments
printf "DELIVER\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.2) “FOO BAR 1” → invalid command
printf "FOO BAR 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.3) “DELIVER HELIUM 1” → invalid molecule
printf "DELIVER HELIUM 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.4) “DELIVER WATER” → missing # argument
printf "DELIVER WATER\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.5) “DELIVER WATER 1 EXTRA” → “ERROR: too many arguments”
printf "DELIVER WATER 1 EXTRA\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.6) “DELIVER WATER xyz” → invalid number
printf "DELIVER WATER xyz\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.7) “DELIVER WATER 1” → not enough atoms
printf "DELIVER WATER 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.8) Add inventory so “DELIVER WATER 1” can succeed
printf "ADD OXYGEN 5\nADD HYDROGEN 5\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true
printf "DELIVER WATER 1\n"   | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.9) “DELIVER CARBON DIOXIDE 1” → not enough carbon
printf "DELIVER CARBON DIOXIDE 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.10) Add C & O & succeed
printf "ADD CARBON 5\nADD OXYGEN 5\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true
printf "DELIVER CARBON DIOXIDE 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.11) “DELIVER GLUCOSE 1” → not enough atoms
printf "DELIVER GLUCOSE 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.12) Add enough atoms & succeed
printf "ADD CARBON 6\nADD HYDROGEN 12\nADD OXYGEN 6\n" | timeout 1s nc -N 127.0.0.1 $TCP_BASE || true
printf "DELIVER GLUCOSE 1\n"        | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

# (3g.13) “DELIVER ALCOHOL 1” → valid if enough atoms
printf "DELIVER ALCOHOL 1\n" | timeout 1s nc -u 127.0.0.1 $UDP_BASE || true

stop_drinks

echo "---- Stage 2 (DELIVER via UDP) complete ----"
echo

########################
# 3h. drinks_bar_dbg – Stage 3: “GEN …” console
########################

echo "========================================"
echo "3h. drinks_bar_dbg – Stage 3: Console commands"
echo "========================================"

PORT3_TCP=$((TCP_BASE+100))
PORT3_UDP=$((UDP_BASE+100))

# (3h.1) “GEN” alone → missing drink type
timeout 1s ./"$DRINKS_BIN" -c 18 -o 18 -h 42 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN\n") || true

# (3h.2) “GEN COFFEE” → unknown drink type
timeout 1s ./"$DRINKS_BIN" -c 18 -o 18 -h 42 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN COFFEE\n") || true

# (3h.3) “GEN SOFT” (no “DRINK”) → “did you mean 'GEN SOFT DRINK'?”
timeout 1s ./"$DRINKS_BIN" -c 18 -o 18 -h 42 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN SOFT\n") || true

# (3h.4) “GEN SOFT DRINK”
timeout 1s ./"$DRINKS_BIN" -c 18 -o 18 -h 42 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN SOFT DRINK\n") || true

# (3h.5) “GEN VODKA”
timeout 1s ./"$DRINKS_BIN" -c 16 -o 16 -h 40 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN VODKA\n") || true

# (3h.6) “GEN CHAMPAGNE”
timeout 1s ./"$DRINKS_BIN" -c 12 -o 12 -h 36 -T $PORT3_TCP -U $PORT3_UDP < <(printf "GEN CHAMPAGNE\n") || true

echo "---- Stage 3 (Console GEN) complete ----"
echo

########################
# 3i. drinks_bar_dbg – Stage 4: Timeout (inactivity)
########################

echo "========================================"
echo "3i. drinks_bar_dbg – Stage 4: Timeout (inactivity)"
echo "========================================"

PORT4_TCP=$((TCP_BASE+200))
PORT4_UDP=$((UDP_BASE+200))

printf "0 0 0" > "$ATOM_FILE_GOOD"
run_drinks "-c 0 -o 0 -h 0 -T $PORT4_TCP -U $PORT4_UDP -t 1 -f $ATOM_FILE_GOOD"
sleep 2    # longer than 1‐second timeout
stop_drinks

echo "---- Stage 4 (Timeout) complete ----"
echo

########################
# 3j. drinks_bar_dbg – Stage 5: UDS_STREAM / UDS_DGRAM
########################

echo "========================================"
echo "3j. drinks_bar_dbg – Stage 5: UDS_STREAM and UDS_DGRAM"
echo "========================================"

PORT5_TCP=$((TCP_BASE+300))
PORT5_UDP=$((UDP_BASE+300))

printf "5 5 5" > "$ATOM_FILE_GOOD"
run_drinks "-c 0 -o 0 -h 0 -T $PORT5_TCP -U $PORT5_UDP -s $UDS_STREAM -d $UDS_DGRAM -f $ATOM_FILE_GOOD"
sleep 0.2

if command -v nc >/dev/null 2>&1; then
    # (3j.1) UDS_STREAM: valid ADD → need a UDS_STREAM server:
    [[ -e "$UDS_STREAM" ]] && rm -f "$UDS_STREAM"
    nc -lU "$UDS_STREAM" >/dev/null 2>&1 &
    UDS_STREAM_ECHO_PID=$!
    sleep 0.1

    # Valid ADD
    printf "ADD CARBON 3\n" | timeout 1s nc -U "$UDS_STREAM" || true
    # Invalid atom type
    printf "ADD NEON 1\n"   | timeout 1s nc -U "$UDS_STREAM" || true

    kill "$UDS_STREAM_ECHO_PID" 2>/dev/null || true

    # (3j.2) UDS_DGRAM: valid and invalid DELIVER → need a UDS_DGRAM server:
    [[ -e "$UDS_DGRAM" ]] && rm -f "$UDS_DGRAM"
    python3 - << 'EOF' &
import socket, os
pth = "$UDS_DGRAM"
if os.path.exists(pth): os.unlink(pth)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(pth)
while True:
    data, addr = s.recvfrom(1024)
    s.sendto(data, addr)
EOF
    UDS_DGRAM_ECHO_PID=$!
    sleep 0.2

    # Not enough atoms → error
    printf "DELIVER WATER 1\n"   | timeout 1s nc -u -U "$UDS_DGRAM" || true
    # Invalid molecule → error
    printf "DELIVER INVALID 1\n" | timeout 1s nc -u -U "$UDS_DGRAM" || true

    kill "$UDS_DGRAM_ECHO_PID" 2>/dev/null || true
    rm -f "$UDS_DGRAM"
else
    echo "→ Skipping UDS_STREAM / UDS_DGRAM tests: 'nc -U' not found."
fi

stop_drinks

echo "---- Stage 5 (UDS) complete ----"
echo

############################
# 4. Rename all *.gcno and *.gcda
############################

echo "---- Step 4: Renaming all newly-generated .gcno/.gcda files ----"

for src in "$DRINKS_SRC" "$ATOM_SRC" "$MOL_SRC"; do
    base="${src%.c}"

    # Rename any "<bin>-<base>.gcno" → "<base>.gcno"
    for note in ./*-"$base".gcno; do
        [[ -f "$note" ]] || continue
        mv -f "$note" "./$base.gcno"
        echo "  → Renamed $(basename "$note") → $base.gcno"
    done

    # Rename any "<bin>-<base>.gcda" → "<base>.gcda"
    for data in ./*-"$base".gcda; do
        [[ -f "$data" ]] || continue
        mv -f "$data" "./$base.gcda"
        echo "  → Renamed $(basename "$data") → $base.gcda"
    done
done

echo "---- Renaming of .gcno/.gcda complete ----"
echo

############################
# 5. Generate gcov output
############################

echo "========================================"
echo "5. Generating gcov reports"
echo "========================================"

gcov "$DRINKS_SRC"   || true
gcov "$ATOM_SRC"     || true
gcov "$MOL_SRC"      || true

echo
echo "---- Coverage summary (grep “Lines executed”) ----"
grep "Lines executed" *.gcov || true

echo
echo "---- All done. Check the *.gcov files for ~100% coverage. ----"
echo

############################
# 6. Cleanup
############################

# Kill any leftover processes, remove FIFOs/UDS sockets
kill 0 2>/dev/null || true
rm -f "$FIFO_STDIN" "$UDS_STREAM" "$UDS_DGRAM" /tmp/mol_dgram.sock /tmp/atom_stream.sock
echo "---- CLEANUP complete ----"


