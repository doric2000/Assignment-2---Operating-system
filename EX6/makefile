CXX = gcc
CXXFLAGS = -Wall -g
# for gcov() only
GCOV_FLAGS = -fprofile-arcs -ftest-coverage

all: atom_supplier.out drinks_bar.out molecule_requester.out

drinks_bar.out: drinks_bar.o
	$(CXX) $(CXXFLAGS) $(GCOV_FLAGS) $^ -o $@

atom_supplier.out: atom_supplier.o
	$(CXX) $(CXXFLAGS) $(GCOV_FLAGS) $^ -o $@

molecule_requester.out: molecule_requester.o
	$(CXX) $(CXXFLAGS) $(GCOV_FLAGS) $^ -o $@

# Convert all source files to object files
%.o: %.c
	$(CXX) $(CXXFLAGS) $(GCOV_FLAGS) -c $< -o $@

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
	rm -f *.o *.gcda *.gcno *.gcov

.PHONY: all gcov clean
