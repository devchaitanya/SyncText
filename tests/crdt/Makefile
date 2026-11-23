# Unit tests for the sequence CRDT in src/rga.{h,cpp}.
#   make test    run the tests
#   make asan    run them under AddressSanitizer + UndefinedBehaviorSanitizer
#   make bench   also print the position() benchmark
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic
SRC      := ../../src
BUILD    := build
DEPS     := $(SRC)/rga.cpp $(SRC)/rga.h test_rga.cpp

.PHONY: test asan bench perf clean

$(BUILD)/test_rga: $(DEPS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) $(SRC)/rga.cpp test_rga.cpp -o $@

$(BUILD)/test_rga_asan: $(DEPS)
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	       -Wall -Wextra -I$(SRC) $(SRC)/rga.cpp test_rga.cpp -o $@

test: $(BUILD)/test_rga
	./$(BUILD)/test_rga

asan: $(BUILD)/test_rga_asan
	./$(BUILD)/test_rga_asan

bench: $(BUILD)/test_rga
	./$(BUILD)/test_rga --bench

# Benchmarks: CRDT core, wire encoding, transports, memory (links all of src/ except main.cpp)
$(BUILD)/bench_rga: bench_rga.cpp $(wildcard $(SRC)/*.cpp) $(wildcard $(SRC)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) -std=gnu++17 -O2 bench_rga.cpp $(filter-out $(SRC)/main.cpp,$(wildcard $(SRC)/*.cpp)) -o $@ -pthread -lrt

perf: $(BUILD)/bench_rga
	./$(BUILD)/bench_rga
	./$(BUILD)/bench_rga --mem 100000
	./$(BUILD)/bench_rga --mem 1000000

clean:
	rm -rf $(BUILD)
