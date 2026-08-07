/**
 * @file runtime_options.cc
 * @brief Parse Axio datapath runtime command-line options.
 */
#include "axio/config/runtime_options.h"

#include <string>

namespace axio::config {

std::optional<RuntimeOptions> parse_runtime_options(int argc,
                                                    char* const argv[]) {
  if ((argc != 3 && argc != 5) || std::string(argv[1]) != "--config") {
    return std::nullopt;
  }

  RuntimeOptions options{argv[2], std::nullopt};
  if (argc == 5) {
    if (std::string(argv[3]) != "--peer-config") {
      return std::nullopt;
    }
    options.peer_config_path = argv[4];
  }
  return options;
}

}  // namespace axio::config
