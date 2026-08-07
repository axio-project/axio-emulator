#pragma once

#include <cstring>

namespace axio {

inline void spin_cycles(unsigned int cycles) {
  volatile unsigned int completed_cycles = 0;
  for (unsigned int i = 0; i < cycles; ++i) {
    ++completed_cycles;
  }
}

inline void access_memory(unsigned int operation_count,
                          unsigned int operation_size) {
  unsigned int* memory = new unsigned int[operation_count];
  const unsigned int access_size = operation_size / sizeof(unsigned int);
  for (unsigned int i = 0; i < operation_count; ++i) {
    std::memset(&memory[i * access_size], 0, operation_size);
  }
  delete[] memory;
}

inline void perform_operations(unsigned int cycles,
                               unsigned int operation_count,
                               unsigned int operation_size) {
  spin_cycles(cycles);
  access_memory(operation_count, operation_size);
}

}  // namespace axio
