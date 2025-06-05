# List of subdirectories

# Default target: build all subdirectories
all:
	make -C EX1
	make -C EX2
	make -C EX3
	make -C EX4
	make -C EX5
	make -C EX6


# Clean target: clean all subdirectories
clean:
	make -C EX1 clean
	make -C EX2 clean
	make -C EX3 clean
	make -C EX4 clean
	make -C EX5 clean
	make -C EX6 clean

.PHONY: all clean