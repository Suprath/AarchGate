# (c) 2026 Suprath PS. All rights reserved.
# AarchGate Guest Agent Makefile

CLANG ?= clang
BPFTOOL ?= bpftool
CXX ?= g++

# Target output files
BPF_OBJ = tracer.bpf.o
BPF_SKEL = tracer.skel.h
AGENT_BIN = aarchgate_agent

# Include paths
INCLUDES = -I../../common

.PHONY: all clean

all: $(AGENT_BIN)

# Generate local vmlinux.h from running kernel BTF symbols
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# Compile eBPF program to bytecode
$(BPF_OBJ): tracer.bpf.c vmlinux.h
	$(CLANG) -target bpf -O2 -g -c $(INCLUDES) -o $@ $<

# Compile eBPF skeleton header for standard C/C++ integration
$(BPF_SKEL): $(BPF_OBJ)
	$(BPFTOOL) gen skeleton $< > $@

# Compile guest agent and link libbpf
$(AGENT_BIN): agent.cpp $(BPF_SKEL)
	$(CXX) -O2 -Wall -std=c++20 $(INCLUDES) agent.cpp -lbpf -lelf -lz -o $@

clean:
	rm -f $(BPF_OBJ) $(BPF_SKEL) $(AGENT_BIN) vmlinux.h
