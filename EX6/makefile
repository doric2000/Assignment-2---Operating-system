# Makefile for Stage 6 with GCOV support

# Compiler and flags
CC       = gcc
CFLAGS   = -g -O0 -fprofile-arcs -ftest-coverage -Wall -Wextra
LDFLAGS  =

# Executable names
DRINKS_BAR       = drinks_bar
ATOM_SUPPLIER    = atom_supplier
MOLECULE_REQUEST = molecule_requester

# Default target: build all executables
all: $(DRINKS_BAR) $(ATOM_SUPPLIER) $(MOLECULE_REQUEST)

# -----------------------------------------------------------------------------
# 1) Build drinks_bar (Stage 6 server)
# -----------------------------------------------------------------------------
$(DRINKS_BAR): drinks_bar.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# -----------------------------------------------------------------------------
# 2) Build atom_supplier (TCP client)
# -----------------------------------------------------------------------------
$(ATOM_SUPPLIER): atom_supplier.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# -----------------------------------------------------------------------------
# 3) Build molecule_requester (UDP/UDS client)
# -----------------------------------------------------------------------------
$(MOLECULE_REQUEST): molecule_requester.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# -----------------------------------------------------------------------------
# 4) Run gcov to generate coverage reports for all .c files
#
#    Usage:
#      1. make
#      2. run your tests or manually invoke the executables (e.g. ./drinks_bar …)
#      3. make gcov
#    This will produce .gcov files alongside the sources.
# -----------------------------------------------------------------------------
gcov:
	gcov -o . drinks_bar.c
	gcov -o . atom_supplier.c
	gcov -o . molecule_requester.c

# -----------------------------------------------------------------------------
# 5) Clean: remove executables, object files, and coverage artifacts (.gcda, .gcno, .gcov)
# -----------------------------------------------------------------------------
clean:
	rm -f $(DRINKS_BAR) $(ATOM_SUPPLIER) $(MOLECULE_REQUEST)
	rm -f *.gcda *.gcno *.gcov

.PHONY: all gcov clean
