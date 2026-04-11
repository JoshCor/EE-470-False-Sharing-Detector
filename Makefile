# =============================================================================
# Makefile — false sharing benchmark
#
# Usage:
#   make              build the benchmark
#   make build-tool   build the PIN tool
#   make run          run all five scenarios natively, log output to logs/
#   make pin          run all five scenarios under your PIN tool
#   make clean        remove build artifacts and logs
# =============================================================================

CC           = gcc
CFLAGS       = -O0 -g -pthread -Wall -Wextra
TARGET       = false_sharing_benchmark
SRC          = false_sharing_benchmark.c

THREADS      = 4
ITERATIONS   = 10000000

LOG_DIR      = logs

# Path to your PIN installation
PIN_ROOT     = $(HOME)/pin
PIN          = $(PIN_ROOT)/pin

# PIN tool source directory and compiled output
PIN_TOOL_DIR = $(PIN_ROOT)/source/tools/FalseSharingDetector
TOOL         = $(PIN_TOOL_DIR)/obj-intel64/false_sharing_detector.so


# -----------------------------------------------------------------------------
# Build the benchmark binary
# -----------------------------------------------------------------------------
.PHONY: all
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)


# -----------------------------------------------------------------------------
# Build the PIN tool
# Run this once your false_share_detector.cpp is ready to compile
# -----------------------------------------------------------------------------
.PHONY: build-tool
build-tool:
	$(MAKE) -C $(PIN_TOOL_DIR) obj-intel64/false_sharing_detector.so


# -----------------------------------------------------------------------------
# Run all five scenarios natively (no PIN)
# Output is printed to the terminal and saved to logs/native-scenario-N.log
# -----------------------------------------------------------------------------
.PHONY: run
run: $(TARGET) $(LOG_DIR)
	@echo "--- Scenario 0: false sharing ---"
	./$(TARGET) 0 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-scenario-0.log
	@echo ""
	@echo "--- Scenario 1: padded, no false sharing ---"
	./$(TARGET) 1 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-scenario-1.log
	@echo ""
	@echo "--- Scenario 2: producer/consumer false sharing ---"
	./$(TARGET) 2 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-scenario-2.log
	@echo ""
	@echo "--- Scenario 3: true sharing / data race ---"
	./$(TARGET) 3 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-scenario-3.log
	@echo ""
	@echo "--- Scenario 4: no sharing ---"
	./$(TARGET) 4 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/native-scenario-4.log
	@echo ""
	@echo "Logs saved to $(LOG_DIR)/"


# -----------------------------------------------------------------------------
# Run all five scenarios under your PIN tool
# Output is printed to the terminal and saved to logs/pin-scenario-N.log
# -----------------------------------------------------------------------------
.PHONY: pin
pin: $(TARGET) $(LOG_DIR)
	@echo "--- Scenario 0: false sharing ---"
	$(PIN) -t $(TOOL) -- ./$(TARGET) 0 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-scenario-0.log
	@echo ""
	@echo "--- Scenario 1: padded, no false sharing ---"
	$(PIN) -t $(TOOL) -- ./$(TARGET) 1 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-scenario-1.log
	@echo ""
	@echo "--- Scenario 2: producer/consumer false sharing ---"
	$(PIN) -t $(TOOL) -- ./$(TARGET) 2 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-scenario-2.log
	@echo ""
	@echo "--- Scenario 3: true sharing / data race ---"
	$(PIN) -t $(TOOL) -- ./$(TARGET) 3 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-scenario-3.log
	@echo ""
	@echo "--- Scenario 4: no sharing ---"
	$(PIN) -t $(TOOL) -- ./$(TARGET) 4 $(THREADS) $(ITERATIONS) 2>&1 | tee $(LOG_DIR)/pin-scenario-4.log
	@echo ""
	@echo "Logs saved to $(LOG_DIR)/"


# -----------------------------------------------------------------------------
# Create log directory
# -----------------------------------------------------------------------------
$(LOG_DIR):
	mkdir -p $(LOG_DIR)


# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -f $(TARGET)
	rm -rf $(LOG_DIR)