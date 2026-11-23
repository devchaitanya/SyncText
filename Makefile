CXX := g++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wpedantic -MMD -MP
LDFLAGS := -pthread -lrt

BUILD_DIR := build
SRC_DIR := src

TARGET := $(BUILD_DIR)/editor
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

.PHONY: all clean dirs editor run test

all: dirs $(TARGET)

# Explicit target name requested: build binary named 'editor'
editor: all
	@echo "Built editor binary at $(TARGET)"

# Hassle-free run: make run DOC=notes [EXTRA_ENV="SYNCTEXT_STATUS=1 SYNCTEXT_DEBUG_LEVEL=2"]
run: editor
	@if [ -z "$(DOC)" ]; then echo "Specify DOC=<document_name> e.g. make run DOC=notes" >&2; exit 1; fi; \
	if [ -n "$(EXTRA_ENV)" ]; then echo "Running with EXTRA_ENV=$(EXTRA_ENV)"; eval $(EXTRA_ENV) ./build/editor $(DOC); else ./build/editor $(DOC); fi

# Automated multi-user convergence test (uses test.sh). Override env vars when invoking:
#   DEBUG_LEVEL=2 BLOCK_OPS_MODE=0 ROUNDS=2 make test
test: editor
	@chmod +x test.sh
	@echo "Running test script (users: default u1 u2 u3 u4). Override by: USERS='u1 u2' ./test.sh" >&2
	@./test.sh || { echo "Test failed" >&2; exit 1; }

dirs:
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)
