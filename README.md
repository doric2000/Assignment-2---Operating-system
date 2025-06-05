# Operating Systems Assignment 2 - Socket Programming

This repository contains 6 exercises (EX1-EX6) that demonstrate progressive implementation of socket programming concepts in C, focusing on TCP/UDP server-client communication, I/O multiplexing, and Unix Domain Sockets.

## Overview

The assignment implements a distributed system for managing atoms and molecules, where:
- **Atoms** (Carbon, Oxygen, Hydrogen) are added via TCP connections
- **Molecules** (Water, Carbon Dioxide, Alcohol, Glucose) are delivered via UDP connections  
- **Beverages** (Soft Drink, Vodka, Champagne) can be generated from available molecules

## Exercise Descriptions

### EX1: Basic TCP Server with I/O Multiplexing
**Files:** `atom_supplier.c`, `atom_warehouse.c`

**Purpose:** Implements basic TCP client-server communication using `select()` for I/O multiplexing.

**Components:**
- **`atom_supplier`** (TCP Client): Connects to server and sends "ADD CARBON/OXYGEN/HYDROGEN <quantity>" commands
- **`atom_warehouse`** (TCP Server): Accepts multiple TCP connections, stores atom counts, uses `select()` to handle multiple clients simultaneously

**Key Features:**
- TCP-only communication
- I/O multiplexing with `select()` 
- Handles up to 10^18 atoms per type
- Multiple concurrent client support

**Build & Run:**
```bash
cd EX1
make
./atom_warehouse.out <port>           # Server
./atom_supplier.out <hostname> <port> # Client
```

---

### EX2: Combined TCP+UDP Server
**Files:** `atom_supplier.c`, `molecule_requester.c`, `molecule_supplier.c`

**Purpose:** Extends EX1 by adding UDP communication for molecule delivery alongside existing TCP atom management.

**Components:**
- **`atom_supplier`** (TCP Client): Same as EX1 - adds atoms via TCP
- **`molecule_requester`** (UDP Client): Sends "DELIVER WATER/CARBON_DIOXIDE/ALCOHOL/GLUCOSE <quantity>" commands via UDP
- **`molecule_supplier`** (TCP+UDP Server): Combined server handling both TCP (atoms) and UDP (molecules) on the same port

**Key Features:**
- Dual protocol server (TCP + UDP)
- Same port for both protocols
- Molecule recipes (e.g., Water = 2H + 1O, Glucose = 6C + 12H + 6O)
- Inventory management for both atoms and molecules

**Build & Run:**
```bash
cd EX2
make
./molecule_supplier.out <port>         # Server (TCP+UDP)
./atom_supplier.out <hostname> <port>  # TCP Client
./molecule_requester.out <hostname> <port> # UDP Client
```

---

### EX3: Interactive Console + Beverage Generation
**Files:** `atom_supplier.c`, `molecule_requester.c`, `drinks_bar.c`

**Purpose:** Adds interactive console commands to generate beverages from available molecules.

**Components:**
- **`atom_supplier`** (TCP Client): Adds atoms via TCP
- **`molecule_requester`** (UDP Client): Delivers molecules via UDP  
- **`drinks_bar`** (TCP+UDP+Console Server): Handles network requests + interactive console for beverage generation

**New Features:**
- Interactive console commands: "GEN SOFT DRINK", "GEN VODKA", "GEN CHAMPAGNE"
- Beverage recipes using molecules as ingredients
- Real-time inventory display
- Keyboard input monitoring via `select()`

**Beverage Recipes:**
- **Soft Drink**: 1 Water + 1 Carbon Dioxide
- **Vodka**: 1 Alcohol  
- **Champagne**: 1 Alcohol + 1 Carbon Dioxide

**Build & Run:**
```bash
cd EX3
make
./drinks_bar.out <port>                # Server
# Type console commands: GEN SOFT DRINK, GEN VODKA, GEN CHAMPAGNE
```

---

### EX4: Timeout Functionality with SIGALRM
**Files:** `atom_supplier.c`, `molecule_requester.c`, `drinks_bar.c`

**Purpose:** Adds optional timeout functionality that automatically shuts down the server after a period of inactivity.

**New Features:**
- Optional timeout parameter
- SIGALRM signal handling for timeout detection
- Automatic server shutdown after inactivity period
- Activity resets timeout counter

**Command Line:**
```bash
./drinks_bar.out -T <tcp_port> -U <udp_port> [-t <timeout_seconds>]
```

**Key Implementation:**
- Uses `alarm()` system call to set timeout
- SIGALRM handler sets timeout flag
- Server monitors activity and resets alarm on each operation
- Graceful shutdown when timeout expires

**Build & Run:**
```bash
cd EX4
make
./drinks_bar.out -T 5555 -U 6666 -t 30  # 30-second timeout
```

---

### EX5: Unix Domain Sockets (UDS)
**Files:** `atom_supplier.c`, `molecule_requester.c`, `drinks_bar.c`

**Purpose:** Extends the server to optionally support Unix Domain Sockets (UDS) in addition to TCP/UDP.

**New Features:**
- Optional UDS-STREAM socket (like TCP, but local)
- Optional UDS-DGRAM socket (like UDP, but local)  
- Mandatory command-line arguments for initial atom counts
- Enhanced argument parsing with `getopt_long()`

**Command Line Arguments:**
- **Mandatory:** `-c <carbon>`, `-o <oxygen>`, `-h <hydrogen>`, `-T <tcp_port>`, `-U <udp_port>`
- **Optional:** `-t <timeout>`, `-s <uds_stream_path>`, `-d <uds_dgram_path>`

**Usage Examples:**
```bash
# TCP+UDP only
./drinks_bar.out -c 100 -o 50 -h 200 -T 5555 -U 6666

# TCP+UDP + UDS-STREAM  
./drinks_bar.out -c 10 -o 10 -h 10 -T 5555 -U 6666 -s /tmp/stream.sock

# TCP+UDP + UDS-DGRAM
./drinks_bar.out -c 10 -o 10 -h 10 -T 5555 -U 6666 -d /tmp/dgram.sock
```

**Build & Run:**
```bash
cd EX5
make
./drinks_bar.out -c 100 -o 100 -h 100 -T 5555 -U 6666 -s /tmp/my_stream.sock
```

---

### EX6: Code Coverage with gcov
**Files:** `atom_supplier.c`, `molecule_requester.c`, `drinks_bar.c`, `coverage_test.sh`

**Purpose:** Same functionality as EX5 but with added code coverage analysis using `gcov`.

**New Features:**
- Compiler flags for code coverage (`-fprofile-arcs -ftest-coverage`)
- `gcov` target in Makefile
- Test script for coverage analysis
- Coverage reports showing code execution statistics

**Coverage Analysis:**
1. Compile with coverage flags
2. Run tests/execute code  
3. Generate coverage reports
4. Analyze which code paths were executed

**Build & Coverage:**
```bash
cd EX6
make                    # Compile with coverage flags
./drinks_bar.out ...    # Run the program
make gcov               # Generate coverage reports
ls *.gcov               # View coverage files
```

## Common Features Across Exercises

### Network Protocols
- **TCP**: Reliable connection-oriented communication for atom additions
- **UDP**: Connectionless communication for molecule deliveries
- **UDS**: Local inter-process communication (EX5+)

### I/O Multiplexing
- `select()` system call for monitoring multiple file descriptors
- Non-blocking server architecture
- Simultaneous handling of multiple clients

### Data Management
- 64-bit integers for large quantities (up to 10^18)
- Atomic operations for inventory management
- Recipe-based molecule/beverage synthesis

### Signal Handling
- SIGALRM for timeout functionality (EX4+)

## Building and Running

Each exercise has its own Makefile. Use the recursive Makefile in the root directory:

```bash
# Build all exercises
make

# Clean all exercises  
make clean

# Build specific exercise
cd EX<number>
make
```

## Technical Implementation Details

### Socket Programming Concepts Demonstrated
1. **Client-Server Architecture**: TCP clients connecting to servers
2. **I/O Multiplexing**: Using `select()` to handle multiple connections
3. **Protocol Design**: Custom message formats for different operations
4. **Error Handling**: Robust error checking and recovery
5. **Signal Handling**: Proper signal handling for timeouts and cleanup
6. **Memory Management**: Careful buffer management and leak prevention

### System Programming Features
- File descriptor management
- Address resolution with `getaddrinfo()`
- Socket options (`SO_REUSEADDR`)
- Unix Domain Sockets for local IPC
- Signal handling and timeouts
- Code coverage analysis

## Learning Objectives

This assignment teaches:
- Socket programming in C (TCP/UDP/UDS)
- I/O multiplexing with `select()`
- Signal handling in Unix systems
- Network protocol design
- Multi-client server architecture
- Code testing and coverage analysis
- System programming best practices

## Dependencies

- GCC compiler
- POSIX-compliant Unix system
- Standard C libraries
- gcov for code coverage (EX6)

## Notes

- All servers support IPv4 only for simplicity
- Maximum atom quantities: 10^18 per type
- Default backlog: 10 pending connections
- Buffer sizes: 1024 bytes for network communication
- Error handling includes detailed error messages and graceful cleanup

Baruh Ifraimov & Dor Cohen