# C0VM – Makefile
#
# Targets:
#   make          – build the c0vm binary (debug build with AddressSanitizer)
#   make release  – optimised build without sanitizers
#   make test     – run hand-crafted .bc0 test suite
#   make clean    – remove generated files

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
          -Iinclude

# Debug build (default)
DEBUG_FLAGS   = -g3 -O0 -fno-omit-frame-pointer
RELEASE_FLAGS = -O2 -DNDEBUG

SRCS = src/c0vm_main.c \
       src/bc0_reader.c \
       src/c0vm.c       \
       src/c0_native.c

TARGET = c0vm

.PHONY: all release test clean

all: $(TARGET)

$(TARGET): $(SRCS) include/c0vm.h include/c0vm_abort.h include/c0_native.h
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -o $@ $(SRCS)
	@echo "Built binary: ./$@"

release: $(SRCS) include/c0vm.h include/c0vm_abort.h include/c0_native.h
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) -o $(TARGET) $(SRCS)
	@echo "Built release binary: ./$(TARGET)"

test: $(TARGET)
	@echo "===== Running Complete C0VM Test Suite ====="
	@python3 run_all_tests.py
	@echo "===== Done ====="

clean:
	rm -f $(TARGET) $(TARGET).exe *.o
	find tests -name "*.out" -delete 2>/dev/null || true
	find tests/c0 -name "*.bc0" -delete 2>/dev/null || true
	find examples -name "*.bc0" -delete 2>/dev/null || true

