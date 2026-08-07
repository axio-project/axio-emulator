#include "axio/config/runtime_options.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace config = axio::config;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_local_config_only() {
  char program[] = "axio";
  char config_flag[] = "--config";
  char config_path[] = "config/client.toml";
  char* argv[] = {program, config_flag, config_path};

  const auto options = config::parse_runtime_options(3, argv);
  expect(options.has_value(), "--config LOCAL must be accepted");
  expect(options->config_path == config_path,
         "the local configuration path must be preserved");
  expect(!options->peer_config_path.has_value(),
         "the peer configuration must be optional");
}

void test_optional_peer_config() {
  char program[] = "axio";
  char config_flag[] = "--config";
  char config_path[] = "config/client.toml";
  char peer_flag[] = "--peer-config";
  char peer_path[] = "config/server.toml";
  char* argv[] = {program, config_flag, config_path, peer_flag, peer_path};

  const auto options = config::parse_runtime_options(5, argv);
  expect(options.has_value(), "--peer-config PEER must remain supported");
  expect(options->config_path == config_path,
         "the local configuration path must be preserved");
  expect(options->peer_config_path == peer_path,
         "the peer configuration path must be preserved");
}

void test_malformed_arguments() {
  char program[] = "axio";
  char config_flag[] = "--config";
  char config_path[] = "config/client.toml";
  char peer_flag[] = "--peer-config";
  char peer_path[] = "config/server.toml";
  char wrong_flag[] = "--peer";

  char* missing_config[] = {program};
  expect(!config::parse_runtime_options(1, missing_config).has_value(),
         "a missing local configuration must be rejected");

  char* wrong_local_flag[] = {program, wrong_flag, config_path};
  expect(!config::parse_runtime_options(3, wrong_local_flag).has_value(),
         "an unknown local flag must be rejected");

  char* wrong_peer_flag[] = {program, config_flag, config_path, wrong_flag,
                             peer_path};
  expect(!config::parse_runtime_options(5, wrong_peer_flag).has_value(),
         "an unknown peer flag must be rejected");

  char* incomplete_peer[] = {program, config_flag, config_path, peer_flag};
  expect(!config::parse_runtime_options(4, incomplete_peer).has_value(),
         "a peer flag without a path must be rejected");
}

}  // namespace

int main() {
  try {
    test_local_config_only();
    test_optional_peer_config();
    test_malformed_arguments();
    std::cout << "Axio runtime options test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio runtime options test failed: " << error.what() << '\n';
    return 1;
  }
}
