#include "common.h"

// 空转 x 个 CPU 循环
void spinCycles(unsigned int cycles) {
    volatile unsigned int i = 0;
    for (unsigned int j = 0; j < cycles; ++j) {
        ++i;
    }
}

// 进行 n 次内存访问
void accessMemory(unsigned int n, unsigned int size) {
    unsigned int* memory = new unsigned int[n];
    unsigned int accessSize = size / sizeof(unsigned int);
    for (unsigned int i = 0; i < n; ++i) {
        // 使用 memset 进行内存访问，将每个内存块填充为 0
        memset(&memory[i * accessSize], 0, size);
    }
    delete[] memory;
}

// 空转 x 个 CPU 循环并进行 n 次内存访问
void performOperations(unsigned int cycles, unsigned int n, unsigned int size) {
    spinCycles(cycles);
    accessMemory(n, size);
}