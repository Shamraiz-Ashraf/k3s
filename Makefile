# DPDK Packet Sender Makefile
# Supports both original and improved versions

# DPDK configuration
PKGCONF ?= pkg-config
PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)

# Compiler and flags
CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=c99
DPDK_CFLAGS = $(shell $(PKGCONF) --cflags libdpdk)
DPDK_LIBS = $(shell $(PKGCONF) --libs libdpdk)
LIBS = $(DPDK_LIBS) -lpcap

# Debug build flags
DEBUG_CFLAGS = -O0 -g -DDEBUG -Wall -Wextra -std=c99

# Target executables
TARGETS = sender_original sender_improved
DEBUG_TARGETS = sender_original_debug sender_improved_debug

# Default target
all: $(TARGETS)

# Release builds
sender_original: sender_cmp.c
	@echo "Building original sender..."
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(LIBS)

sender_improved: sender_improved.c
	@echo "Building improved sender..."
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(LIBS)

# Debug builds
debug: $(DEBUG_TARGETS)

sender_original_debug: sender_cmp.c
	@echo "Building original sender (debug)..."
	$(CC) $(DEBUG_CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(LIBS)

sender_improved_debug: sender_improved.c
	@echo "Building improved sender (debug)..."
	$(CC) $(DEBUG_CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(LIBS)

# Clean targets
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(TARGETS) $(DEBUG_TARGETS)

# Installation check
check-deps:
	@echo "Checking dependencies..."
	@$(PKGCONF) --exists libdpdk || (echo "Error: DPDK not found. Please install DPDK development packages." && exit 1)
	@$(PKGCONF) --exists libpcap || (echo "Error: libpcap not found. Please install libpcap development packages." && exit 1)
	@echo "All dependencies found."

# Performance testing helper
test-performance: sender_improved
	@echo "Running basic performance test..."
	@echo "Note: This requires a test.pcap file and appropriate permissions"
	@echo "sudo ./sender_improved -c 0x1 -n 2 -- --pcap test.pcap --replays 1 --pci 0000:01:00.0"

# Static analysis
static-analysis:
	@echo "Running static analysis..."
	@which cppcheck >/dev/null 2>&1 || (echo "cppcheck not found. Install with: apt-get install cppcheck" && exit 1)
	cppcheck --enable=all --std=c99 --suppress=missingIncludeSystem sender_cmp.c sender_improved.c

# Code formatting
format:
	@echo "Formatting code..."
	@which clang-format >/dev/null 2>&1 || (echo "clang-format not found. Install with: apt-get install clang-format" && exit 1)
	clang-format -i -style="{IndentWidth: 4, TabWidth: 4, UseTab: Never}" sender_cmp.c sender_improved.c

# Help target
help:
	@echo "DPDK Packet Sender Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all                - Build all release targets"
	@echo "  sender_original    - Build original sender only"
	@echo "  sender_improved    - Build improved sender only"
	@echo "  debug              - Build all debug targets"
	@echo "  clean              - Remove all build artifacts"
	@echo "  check-deps         - Check for required dependencies"
	@echo "  test-performance   - Show performance test command"
	@echo "  static-analysis    - Run static code analysis"
	@echo "  format             - Format source code"
	@echo "  help               - Show this help message"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CC                 - C compiler (default: gcc)"
	@echo "  PKGCONF            - pkg-config command (default: pkg-config)"
	@echo ""
	@echo "Examples:"
	@echo "  make all                    # Build both versions"
	@echo "  make sender_improved        # Build improved version only"
	@echo "  make debug                  # Build debug versions"
	@echo "  CC=clang make all          # Use clang compiler"

# Installation target (requires root)
install: sender_improved
	@echo "Installing improved sender to /usr/local/bin..."
	@[ $$(id -u) -eq 0 ] || (echo "Error: Installation requires root privileges. Use 'sudo make install'" && exit 1)
	install -m 755 sender_improved /usr/local/bin/
	@echo "Installation complete. You can now run 'sender_improved' from anywhere."

# Uninstall target (requires root)
uninstall:
	@echo "Removing sender_improved from /usr/local/bin..."
	@[ $$(id -u) -eq 0 ] || (echo "Error: Uninstallation requires root privileges. Use 'sudo make uninstall'" && exit 1)
	rm -f /usr/local/bin/sender_improved
	@echo "Uninstallation complete."

.PHONY: all debug clean check-deps test-performance static-analysis format help install uninstall