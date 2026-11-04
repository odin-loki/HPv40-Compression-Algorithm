# hpd — day-to-day context-mixing compressor (from HP40-Lite).
CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++20 -pthread
BIN      ?= build

all: $(BIN)/hpd

$(BIN):
	mkdir -p $(BIN)

$(BIN)/hpd: src/hpd.cpp $(wildcard include/hp/*.hpp) | $(BIN)
	$(CXX) $(CXXFLAGS) -I include -o $@ src/hpd.cpp

# Original single-stream HP40-Lite CLI (archive format of hp, not hpd).
$(BIN)/hp_lite: src/hp_main.cpp $(wildcard include/hp/*.hpp) | $(BIN)
	$(CXX) $(CXXFLAGS) -I include -o $@ src/hp_main.cpp

test: $(BIN)/hpd
	bash test/roundtrip.sh $(BIN)/hpd

clean:
	rm -rf $(BIN)

.PHONY: all test clean
