# Makefile for DPDK Sender Application

# DPDK installation path - adjust as needed
DPDK_PATH ?= /usr/local
DPDK_INCLUDE = $(DPDK_PATH)/include
DPDK_LIB = $(DPDK_PATH)/lib

# Compiler and flags
CC = gcc
CFLAGS = -O3 -g -Wall -Wextra -std=c99
CFLAGS += -I$(DPDK_INCLUDE)
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += -march=native -DRTE_MACHINE_CPUFLAG_SSE -DRTE_MACHINE_CPUFLAG_SSE2 -DRTE_MACHINE_CPUFLAG_SSE3 -DRTE_MACHINE_CPUFLAG_SSSE3
CFLAGS += -DRTE_MACHINE_CPUFLAG_SSE4_1 -DRTE_MACHINE_CPUFLAG_SSE4_2
CFLAGS += -DRTE_MACHINE_CPUFLAG_AES -DRTE_MACHINE_CPUFLAG_PCLMULQDQ
CFLAGS += -DRTE_MACHINE_CPUFLAG_AVX -DRTE_MACHINE_CPUFLAG_RDRAND -DRTE_MACHINE_CPUFLAG_FSGSBASE
CFLAGS += -DRTE_MACHINE_CPUFLAG_F16C -DRTE_MACHINE_CPUFLAG_AVX2

# Linker flags
LDFLAGS = -L$(DPDK_LIB)
LDFLAGS += -Wl,--whole-archive
LDFLAGS += -lrte_eal -lrte_mempool -lrte_mbuf -lrte_net -lrte_mempool_ring
LDFLAGS += -lrte_pmd_virtio -lrte_pmd_vmxnet3_uio -lrte_pmd_i40e -lrte_pmd_ixgbe
LDFLAGS += -lrte_pmd_e1000 -lrte_pmd_ena -lrte_pmd_ring -lrte_pmd_null
LDFLAGS += -lrte_pmd_bond -lrte_pmd_cxgbe -lrte_pmd_ena_com -lrte_pmd_enic
LDFLAGS += -lrte_pmd_fm10k -lrte_pmd_i40e -lrte_pmd_ixgbe -lrte_pmd_mlx4
LDFLAGS += -lrte_pmd_mlx5 -lrte_pmd_nfp -lrte_pmd_qede -lrte_pmd_sfc_efx
LDFLAGS += -lrte_pmd_tap -lrte_pmd_thunderx_nicvf -lrte_pmd_virtio
LDFLAGS += -lrte_pmd_vmxnet3_uio -lrte_pmd_af_packet
LDFLAGS += -Wl,--no-whole-archive
LDFLAGS += -lpcap -lpthread -ldl -lnuma

# Targets
TARGETS = sender_improved sender_cmp

# Default target
all: $(TARGETS)

# Build improved sender
sender_improved: sender_improved.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Build original sender for comparison
sender_cmp: sender_cmp.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Clean build artifacts
clean:
	rm -f $(TARGETS)
	rm -f *.o
	rm -f *.d

# Install (optional)
install: sender_improved
	install -m 755 sender_improved /usr/local/bin/

# Uninstall
uninstall:
	rm -f /usr/local/bin/sender_improved

# Debug build
debug: CFLAGS += -DDEBUG -g3 -O0
debug: sender_improved

# Release build
release: CFLAGS += -DNDEBUG -O3 -march=native
release: sender_improved

# Check DPDK installation
check-dpdk:
	@echo "Checking DPDK installation..."
	@if [ ! -d "$(DPDK_INCLUDE)" ]; then \
		echo "Error: DPDK include directory not found at $(DPDK_INCLUDE)"; \
		echo "Please set DPDK_PATH to your DPDK installation directory"; \
		exit 1; \
	fi
	@if [ ! -d "$(DPDK_LIB)" ]; then \
		echo "Error: DPDK library directory not found at $(DPDK_LIB)"; \
		echo "Please set DPDK_PATH to your DPDK installation directory"; \
		exit 1; \
	fi
	@echo "DPDK installation found at $(DPDK_PATH)"

# Build with DPDK check
build: check-dpdk all

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build both sender applications"
	@echo "  sender_improved - Build improved sender only"
	@echo "  sender_cmp   - Build original sender only"
	@echo "  clean        - Remove build artifacts"
	@echo "  debug        - Build with debug symbols"
	@echo "  release      - Build optimized release version"
	@echo "  install      - Install to /usr/local/bin"
	@echo "  uninstall    - Remove from /usr/local/bin"
	@echo "  check-dpdk   - Verify DPDK installation"
	@echo "  build        - Check DPDK and build"
	@echo ""
	@echo "Environment variables:"
	@echo "  DPDK_PATH    - DPDK installation path (default: /usr/local)"

# Dependencies
sender_improved.o: sender_improved.c
sender_cmp.o: sender_cmp.c

# Phony targets
.PHONY: all clean install uninstall debug release check-dpdk build help