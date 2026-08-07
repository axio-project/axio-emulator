/**
 * @file runtime_options.h
 * @brief Parse Axio datapath runtime command-line options.
 */
#pragma once

#include <filesystem>
#include <optional>

namespace axio::config {

struct RuntimeOptions {
  std::filesystem::path config_path;
  std::optional<std::filesystem::path> peer_config_path;
};

std::optional<RuntimeOptions> parse_runtime_options(int argc,
                                                    char* const argv[]);

}  // namespace axio::config
