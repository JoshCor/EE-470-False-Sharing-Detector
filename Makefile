# =============================================================================
# Makefile — false sharing detector
#
# Usage:
#   make                                          build default benchmark (simple_benchmark)
#   make PROGRAM=false_sharing_benchmark          build a specific benchmark
#   make run                                      run natively with default args
#   make run PROGRAM=false_sharing_benchmark SCENARIO=0 THREADS=4 ITERATIONS=10000000
#   make pin                                      build PIN tool (if stale) and run under PIN
#   make pin PROGRAM=false_sharing_benchmark SCENARIO=1
#   make clean                                    remove all build artifacts and logs
# =============================================================================

CC           = gcc
CFLAGS       = -O0 -g -pthread -Wall -Wextra

# -----------------------------------------------------------------------------
# Benchmark selection
# Override on the command line: make PROGRAM=false_sharing_benchmark
# Defaults to simple_benchmark which has exactly one false sharing instance —
# use this during early tool development before adding complexity.
# -----------------------------------------------------------------------------
PROGRAM      ?= simple_benchmark
SRC          = $(PROGRAM).c
TARGET       = $(PROGRAM)

# -----------------------------------------------------------------------------
# Workload parameters for false_sharing_benchmark
# SCENARIO selects which memory access pattern to exercise (0-4)
# These are ignored by simple_benchmark which takes no arguments
#   0: false sharing
#   1: no false sharing (padded)
#   2: producer/consumer false sharing
#   3: true sharing (data race, not false sharing)
#   4: no sharing
# -----------------------------------------------------------------------------
SCENARIO     ?= 0
THREADS      ?= 4
ITERATIONS   ?= 10000000

LOG_DIR      = logs

# -----------------------------------------------------------------------------
# PIN installation and tool paths
# PIN_ROOT defaults to ~/pin-4.2 but can be overridden:
#   make pin PIN_ROOT=/opt/pin-4.2
# -----------------------------------------------------------------------------
PIN_ROOT     ?= $(HOME)/pin-4.2
PIN          = $(PIN_ROOT)/pin
PIN_TOOL_DIR = $(PIN_ROOT)/source/tools/FalseSharingDetector
TOOL         = $(PIN_TOOL_DIR)/obj-intel64/false_sharing_detector.so


# =============================================================================
# Build benchmark binary
# =============================================================================
.PHONY: all
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)


# =============================================================================
# Run natively (no PIN)
# Output is printed to terminal and saved to logs/native-<program>-<scenario>.log
# Lets you measure baseline performance and verify counter values before
# running under PIN.
# =============================================================================
.PHONY: run
run: $(TARGET) $(LOG_DIR)
	./$(TARGET) $(SCENARIO) $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-$(PROGRAM)-$(SCENARIO).log


# =============================================================================
# Build PIN tool and run under PIN
# The $(TOOL) rule below checks timestamps — if false_sharing_detector.cpp
# is newer than the .so, the tool is rebuilt automatically before running.
# Output is saved to logs/pin-<program>-<scenario>.log
# =============================================================================
.PHONY: pin
pin: $(TARGET) $(TOOL) $(LOG_DIR)
	$(PIN) -t $(TOOL) -- ./$(TARGET) $(SCENARIO) $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-$(PROGRAM)-$(SCENARIO).log


# =============================================================================
# Build PIN tool
# Depends on the .cpp source so make detects changes and rebuilds when stale.
# This was the root cause of the stale .so problem — without this dependency
# make had no way to know the source had changed.
# =============================================================================
$(TOOL): $(PIN_TOOL_DIR)/false_sharing_detector.cpp
	$(MAKE) -C $(PIN_TOOL_DIR) obj-intel64/false_sharing_detector.so


# =============================================================================
# Create log directory
# =============================================================================
$(LOG_DIR):
	mkdir -p $(LOG_DIR)


# =============================================================================
# Clean
# Removes the benchmark binary, the compiled PIN tool .so, and all logs.
# Run this if you suspect a stale build.
# =============================================================================
.PHONY: clean
clean:
	rm -f $(TARGET)
	rm -f $(TOOL)
	rm -rf $(LOG_DIR)