#include "axio/datapath_batching.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  try {
    expect(!axio::dispatcher_batch_ready(31, 32),
           "dispatcher batch must wait below its threshold");
    expect(axio::dispatcher_batch_ready(32, 32) &&
               axio::dispatcher_batch_ready(64, 32),
           "dispatcher batch must run at or above its threshold");
    expect(axio::nic_post_count(0, 32) == 0 &&
               axio::nic_post_count(16, 32) == 16 &&
               axio::nic_post_count(64, 32) == 32,
           "NIC post size must cap hardware batch length");
    std::cout << "Axio datapath batching policy test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio datapath batching policy test failed: " << error.what()
              << '\n';
    return 1;
  }
}
