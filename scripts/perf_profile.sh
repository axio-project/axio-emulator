#!/bin/bash

# L1 cache profiling
perf stat -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores,L1-dcache-store-misses,L1-dcache-prefetches,L1-dcache-prefetch-misses ./test

# LLC cache profiling (LLC = L2 + L3)
perf stat -e cache-references,cache-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses,LLC-prefetches,LLC-prefetch-misses ./test

# TLB profiling
perf stat -e dTLB-loads,dTLB-load-misses,dTLB-stores,dTLB-store-misses,iTLB-loads,iTLB-load-misses ./test

# Branch profiling
perf stat -e branches,branch-misses ./test