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

# hpd-gpu: the same program with -G (model loop on an NVIDIA GPU). Needs clang
# with CUDA support (tested: clang 23.1 + CUDA 12.4, RTX 3090 = sm_86).
CLANG     ?= clang++
CUDA_PATH ?= /usr/local/cuda
GPU_ARCH  ?= sm_86
$(BIN)/hpd-gpu: src/hpd.cpp src/hpd_gpu.cuh $(wildcard include/hp/*.hpp) | $(BIN)
	$(CLANG) -x cuda -std=c++20 -O2 -march=native -DHPD_GPU --cuda-gpu-arch=$(GPU_ARCH) --cuda-path=$(CUDA_PATH) \
	    -I include src/hpd.cpp -o $@ -L$(CUDA_PATH)/lib64 -lcudart -ldl -lrt -pthread

gpu: $(BIN)/hpd-gpu

gpu-test: $(BIN)/hpd $(BIN)/hpd-gpu
	bash test/gpu_check.sh $(BIN)/hpd $(BIN)/hpd-gpu

clean:
	rm -rf $(BIN)

.PHONY: all test clean gpu gpu-test
