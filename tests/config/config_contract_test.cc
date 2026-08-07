#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace config = axio::config;

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

void expect(bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    fail("failed to open fixture: " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string replace_once(std::string contents, const std::string& old_value,
                         const std::string& new_value) {
  const size_t offset = contents.find(old_value);
  if (offset == std::string::npos) {
    fail("fixture mutation target not found: " + old_value);
  }
  contents.replace(offset, old_value.size(), new_value);
  return contents;
}

class TempConfig {
 public:
  TempConfig(const fs::path& base, const std::string& old_value,
             const std::string& new_value, size_t case_index)
      : path_(fs::temp_directory_path() /
              ("axio-config-contract-" + std::to_string(case_index) + ".toml")) {
    std::ofstream output(this->path_, std::ios::trunc);
    if (!output.is_open()) {
      fail("failed to create temporary config: " + this->path_.string());
    }
    output << replace_once(read_file(base), old_value, new_value);
  }

  ~TempConfig() {
    std::error_code error;
    fs::remove(this->path_, error);
  }

  const fs::path& path() const { return this->path_; }

 private:
  fs::path path_;
};

void expect_source_location(const std::string& message) {
  const size_t toml = message.find(".toml:");
  expect(toml != std::string::npos,
         "diagnostic must contain TOML source path and location: " + message);
  const size_t line = toml + std::string(".toml:").size();
  expect(line < message.size() && message[line] >= '0' && message[line] <= '9',
         "diagnostic must contain a TOML line number: " + message);
}

void expect_load_error(const fs::path& path, const std::string& key) {
  try {
    static_cast<void>(config::load_config(path));
  } catch (const config::ConfigError& error) {
    const std::string message = error.what();
    expect(message.find(key) != std::string::npos,
           "load diagnostic must name key " + key + ": " + message);
    expect_source_location(message);
    return;
  }
  fail("expected load_config to reject " + key);
}

void expect_validation_error(const fs::path& path, const std::string& key) {
  const config::AxioConfig loaded = config::load_config(path);
  const config::ValidationResult result = config::validate_config(loaded);
  expect(!result.ok(), "expected validate_config to reject " + key);
  const std::string message = result.format();
  expect(message.find(key) != std::string::npos,
         "validation diagnostic must name key " + key + ": " + message);
  expect_source_location(message);
}

void test_valid_schema(const fs::path& fixture) {
  const config::AxioConfig loaded = config::load_config(fixture);
  const config::ValidationResult result = config::validate_config(loaded);

  expect(result.ok(), "valid schema-v1 fixture must pass: " + result.format());
  expect(loaded.schema_version == 1, "schema version must be typed");
  expect(loaded.deployment.transport == config::DeploymentTransport::kSsh,
         "deployment transport must be typed");
  expect(loaded.deployment.role == config::Role::kServer,
         "role must be typed");
  expect(loaded.network.backend == config::Backend::kDpdk,
         "backend must be typed");
  expect(loaded.handler.message_handler == config::MessageHandler::kThroughput,
         "message handler must be typed");
  expect(loaded.knobs.build.mtu == 2048, "MTU must be loaded");
  expect(loaded.knobs.build.inflight_messages == 1024,
         "inflight budget must be loaded");
  expect(loaded.knobs.runtime.application_core_count == 1,
         "application core count must be loaded");
  expect(loaded.knobs.runtime.dispatcher_queue_count == 1,
         "dispatcher queue count must be loaded");
  expect(loaded.other.iterations == 30, "other fields must be loaded");
  expect(loaded.metrics.enabled, "metrics.enabled must be loaded");
  expect(loaded.network.local_mac == "10:70:fd:00:00:01",
         "network identity must be loaded canonically");
  expect(loaded.deployment.topology.workspaces.size() == 2,
         "workspace array must be loaded");
  expect(loaded.deployment.topology.workloads.size() == 1,
         "workload array must be loaded");
}

void test_deployment_topology_schema(const fs::path& fixture) {
  const config::AxioConfig loaded = config::load_config(fixture);
  const config::ValidationResult result = config::validate_config(loaded);

  expect(result.ok(),
         "deployment topology fixture must pass: " + result.format());
  expect(loaded.deployment.topology.workspaces.size() == 2,
         "deployment topology workspace array must be loaded");
  expect(loaded.deployment.topology.workloads.size() == 1,
         "deployment topology workload array must be loaded");
}

void test_optional_tuning(const fs::path& fixture) {
  const config::AxioConfig loaded = config::load_config(fixture);
  const config::ValidationResult result = config::validate_config(loaded);

  expect(result.ok(), "schema without tuning must pass: " + result.format());
  expect(!loaded.tuning.has_value(),
         "missing tuning table must remain absent in the typed config");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      fail("usage: axio-config-contract-test <source-root>");
    }

    const fs::path fixture =
        fs::path(argv[1]) / "tests/config/schema-v1.valid.toml";
    const fs::path legacy_topology_fixture =
        fs::path(argv[1]) / "tests/config/schema-v1.legacy-topology.toml";
    const fs::path no_tuning_fixture =
        fs::path(argv[1]) / "tests/config/schema-v1.no-tuning.toml";
    test_valid_schema(fixture);
    test_deployment_topology_schema(fixture);
    test_optional_tuning(no_tuning_fixture);
    expect_load_error(legacy_topology_fixture, "work");

    size_t case_index = 0;
    {
      TempConfig config(fixture, "transport = \"ssh\"",
                        "transport = \"direct\"", ++case_index);
      expect_load_error(config.path(), "deployment.transport");
    }
    {
      TempConfig config(
          fixture,
          "transport = \"ssh\"\nrole = \"server\"\nnuma_node = 0\n"
          "host = \"axio-server.example.net\"\nssh_port = 22\n"
          "ssh_user = \"axio\"",
          "transport = \"local\"\nrole = \"server\"\nnuma_node = 0\n"
          "host = \"\"\nssh_port = 0\nssh_user = \"\"",
          ++case_index);
      const config::AxioConfig loaded = config::load_config(config.path());
      expect(config::validate_config(loaded).ok(),
             "local transport must ignore SSH identity");
    }
    {
      TempConfig config(fixture, "host = \"axio-server.example.net\"",
                        "host = \"legacy-unset\"", ++case_index);
      expect_validation_error(config.path(), "deployment.host");
    }
    {
      TempConfig config(fixture, "ssh_user = \"axio\"",
                        "ssh_user = \"legacy-unset\"", ++case_index);
      expect_validation_error(config.path(), "deployment.ssh_user");
    }
    {
      TempConfig config(fixture, "ssh_port = 22", "ssh_port = 0",
                        ++case_index);
      expect_validation_error(config.path(), "deployment.ssh_port");
    }
    {
      TempConfig config(fixture, "workdir = \"/opt/axio-emulator\"",
                        "workdir = \"\"", ++case_index);
      expect_validation_error(config.path(), "deployment.workdir");
    }
    {
      TempConfig config(fixture, "[metrics]\nenabled = true\n", "[metrics]\n",
                        ++case_index);
      expect_load_error(config.path(), "metrics.enabled");
    }
    {
      TempConfig config(fixture, "[metrics]\nenabled = true",
                        "[metrics]\nenabled = \"true\"", ++case_index);
      expect_load_error(config.path(), "metrics.enabled");
    }
    {
      TempConfig config(fixture, "[metrics]\nenabled = true",
                        "[metrics]\nenabled = false", ++case_index);
      const config::AxioConfig loaded = config::load_config(config.path());
      expect(config::validate_config(loaded).ok(),
             "metrics.enabled=false must remain a valid emulator config");
      expect(!loaded.metrics.enabled,
             "disabled metrics policy must remain typed false");
    }
    {
      TempConfig config(fixture, "max_iterations = 20\n", "", ++case_index);
      expect_load_error(config.path(), "tuning.max_iterations");
    }
    {
      TempConfig config(
          fixture, "[tuning.noise]",
          "resources = { application_workspaces = [4], "
          "dispatcher_workspaces = [0] }\n\n[tuning.noise]",
          ++case_index);
      expect_load_error(config.path(), "tuning.resources");
    }
    {
      TempConfig config(fixture, "cpu_core = 4",
                        "cpu_core = 4\n\n[[workspaces]]\n"
                        "id = 9\ncpu_core = 9",
                        ++case_index);
      expect_load_error(config.path(), "workspaces");
    }
    {
      TempConfig config(fixture, "cpu_core = 4",
                        "cpu_core = 4\n\n[[workloads]]\n"
                        "id = 9",
                        ++case_index);
      expect_load_error(config.path(), "workloads");
    }
    {
      TempConfig config(fixture, "window_seconds = 1",
                        "window_seconds = 1\nunknown_option = 7", ++case_index);
      expect_load_error(config.path(), "other.unknown_option");
    }
    {
      TempConfig config(fixture, "schema_version = 1", "schema_version = 2",
                        ++case_index);
      expect_load_error(config.path(), "schema_version");
    }
    {
      TempConfig config(fixture, "backend = \"dpdk\"", "backend = \"raw\"",
                        ++case_index);
      expect_load_error(config.path(), "network.backend");
    }
    {
      TempConfig config(fixture, "iterations = 30", "iterations = 0",
                        ++case_index);
      expect_validation_error(config.path(), "other.iterations");
    }
    {
      TempConfig config(fixture, "local_mac = \"10:70:fd:00:00:01\"",
                        "local_mac = \"10.70.fd.00.00.01\"", ++case_index);
      expect_validation_error(config.path(), "network.local_mac");
    }
    {
      TempConfig config(fixture, "request_payload_bytes = 982",
                        "request_payload_bytes = 0", ++case_index);
      expect_validation_error(config.path(), "handler.request_payload_bytes");
    }
    {
      TempConfig config(fixture, "rx_ring_entries = 2048",
                        "rx_ring_entries = 2000", ++case_index);
      expect_validation_error(config.path(), "network.rx_ring_entries");
    }
    {
      TempConfig config(fixture, "mempool_size = 8192",
                        "mempool_size = 1024", ++case_index);
      expect_validation_error(config.path(), "other.mempool_size");
    }
    {
      TempConfig config(fixture, "inflight_messages = 1024",
                        "inflight_messages = 9000", ++case_index);
      expect_validation_error(config.path(), "knobs.build.inflight_messages");
    }
    {
      TempConfig config(fixture, "application_core_count = 1",
                        "application_core_count = 2", ++case_index);
      expect_validation_error(config.path(),
                              "knobs.runtime.application_core_count");
    }
    {
      TempConfig config(fixture, "[handler]", "[build]", ++case_index);
      expect_load_error(config.path(), "build");
    }

    std::cout << "axio config contract test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "axio config contract test failed: " << error.what() << std::endl;
    return 1;
  }
}
