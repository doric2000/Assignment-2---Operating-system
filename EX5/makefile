# Flags configuration
CXX = gcc
CXXFLAGS = -Wall -g

all: atom_supplier.out drinks_bar.out molecule_requester.out

drinks_bar.out: drinks_bar.o
	$(CXX) $(CXXFLAGS) $^ -o $@

atom_supplier.out: atom_supplier.o
	$(CXX) $(CXXFLAGS) $^ -o $@

molecule_requester.out: molecule_requester.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# Convert all source files to object files
%.o: %.c
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o *.out
	clear

.PHONY: all clean