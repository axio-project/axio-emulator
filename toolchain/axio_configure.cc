/**
 * @file axio_configure.cc
 * @brief Native configuration CLI shared by developers, Meson, and PipeTune.
 */
#include "axio/config/build_config.h"
#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"

#include <toml++/toml.hpp>

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
namespace config = axio::config;

namespace {

using JsonScalar = std::variant<int64_t, double, bool, std::string>;

class JsonObjectParser {
 public:
  explicit JsonObjectParser(std::string input) : input_(std::move(input)) {}

  std::map<std::string, JsonScalar> parse() {
    std::map<std::string, JsonScalar> values;
    this->_skip_whitespace();
    this->_expect('{');
    this->_skip_whitespace();
    if (this->_consume('}')) {
      this->_finish();
      return values;
    }
    while (true) {
      this->_skip_whitespace();
      const std::string key = this->_parse_string();
      this->_skip_whitespace();
      this->_expect(':');
      this->_skip_whitespace();
      if (!values.emplace(key, this->_parse_scalar()).second) {
        this->_error("duplicate key '" + key + "'");
      }
      this->_skip_whitespace();
      if (this->_consume('}')) break;
      this->_expect(',');
    }
    this->_finish();
    return values;
  }

 private:
  std::string input_;
  size_t offset_ = 0;

  [[noreturn]] void _error(const std::string& message) const {
    throw std::runtime_error("invalid override JSON at byte " +
                             std::to_string(this->offset_) + ": " + message);
  }

  void _skip_whitespace() {
    while (this->offset_ < this->input_.size() &&
           std::isspace(static_cast<unsigned char>(this->input_[this->offset_]))) {
      ++this->offset_;
    }
  }

  bool _consume(char expected) {
    if (this->offset_ < this->input_.size() &&
        this->input_[this->offset_] == expected) {
      ++this->offset_;
      return true;
    }
    return false;
  }

  void _expect(char expected) {
    if (!this->_consume(expected)) {
      this->_error(std::string("expected '") + expected + "'");
    }
  }

  void _finish() {
    this->_skip_whitespace();
    if (this->offset_ != this->input_.size()) {
      this->_error("trailing data");
    }
  }

  std::string _parse_string() {
    this->_expect('"');
    std::string value;
    while (this->offset_ < this->input_.size()) {
      const char character = this->input_[this->offset_++];
      if (character == '"') return value;
      if (character != '\\') {
        if (static_cast<unsigned char>(character) < 0x20) {
          this->_error("control character in string");
        }
        value.push_back(character);
        continue;
      }
      if (this->offset_ == this->input_.size()) {
        this->_error("unterminated escape");
      }
      const char escaped = this->input_[this->offset_++];
      switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: this->_error("unsupported string escape");
      }
    }
    this->_error("unterminated string");
  }

  JsonScalar _parse_scalar() {
    if (this->offset_ == this->input_.size()) {
      this->_error("expected scalar value");
    }
    if (this->input_[this->offset_] == '"') return this->_parse_string();
    if (this->input_.compare(this->offset_, 4, "true") == 0) {
      this->offset_ += 4;
      return true;
    }
    if (this->input_.compare(this->offset_, 5, "false") == 0) {
      this->offset_ += 5;
      return false;
    }

    const size_t begin = this->offset_;
    if (this->input_[this->offset_] == '-') ++this->offset_;
    while (this->offset_ < this->input_.size() &&
           std::isdigit(static_cast<unsigned char>(this->input_[this->offset_]))) {
      ++this->offset_;
    }
    bool floating = false;
    if (this->offset_ < this->input_.size() &&
        this->input_[this->offset_] == '.') {
      floating = true;
      ++this->offset_;
      while (this->offset_ < this->input_.size() &&
             std::isdigit(
                 static_cast<unsigned char>(this->input_[this->offset_]))) {
        ++this->offset_;
      }
    }
    if (this->offset_ < this->input_.size() &&
        (this->input_[this->offset_] == 'e' ||
         this->input_[this->offset_] == 'E')) {
      floating = true;
      ++this->offset_;
      if (this->offset_ < this->input_.size() &&
          (this->input_[this->offset_] == '+' ||
           this->input_[this->offset_] == '-')) {
        ++this->offset_;
      }
      while (this->offset_ < this->input_.size() &&
             std::isdigit(
                 static_cast<unsigned char>(this->input_[this->offset_]))) {
        ++this->offset_;
      }
    }
    if (this->offset_ == begin ||
        (this->offset_ == begin + 1 && this->input_[begin] == '-')) {
      this->_error("only string, number, and boolean values are supported");
    }
    const std::string value = this->input_.substr(begin, this->offset_ - begin);
    try {
      return floating ? JsonScalar(std::stod(value))
                      : JsonScalar(std::stoll(value));
    } catch (const std::exception&) {
      this->_error("invalid number");
    }
  }
};

void require_valid(const config::AxioConfig& value) {
  const config::ValidationResult result = config::validate_config(value);
  if (!result.ok()) {
    throw std::runtime_error(result.format());
  }
}

toml::table config_table(const config::AxioConfig& value) {
  toml::table root;
  root.insert("schema_version", static_cast<int64_t>(value.schema_version));

  toml::table build;
  build.insert("role", std::string(config::to_string(value.build.role)));
  build.insert("backend", std::string(config::to_string(value.build.backend)));
  build.insert("roce_transport",
               std::string(config::to_string(value.build.roce_transport)));
  build.insert("mtu", static_cast<int64_t>(value.build.mtu));
  build.insert("rx_ring_entries",
               static_cast<int64_t>(value.build.rx_ring_entries));
  build.insert("tx_ring_entries",
               static_cast<int64_t>(value.build.tx_ring_entries));
  build.insert("mempool_size", static_cast<int64_t>(value.build.mempool_size));
  build.insert("mempool_handler",
               std::string(config::to_string(value.build.mempool_handler)));
  build.insert("mempool_cache_size",
               static_cast<int64_t>(value.build.mempool_cache_size));
  build.insert("message_handler",
               std::string(config::to_string(value.build.message_handler)));
  build.insert("packet_handler",
               std::string(config::to_string(value.build.packet_handler)));
  build.insert("apply_new_mbuf", value.build.apply_new_mbuf);
  build.insert("request_payload_bytes",
               static_cast<int64_t>(value.build.request_payload_bytes));
  build.insert("response_payload_bytes",
               static_cast<int64_t>(value.build.response_payload_bytes));
  build.insert("app_ticks_per_message",
               static_cast<int64_t>(value.build.app_ticks_per_message));
  build.insert("inflight_limit_enabled",
               value.build.inflight_limit_enabled);
  build.insert("inflight_messages",
               static_cast<int64_t>(value.build.inflight_messages));
  root.insert("build", std::move(build));

  toml::table runtime;
  runtime.insert("numa_node", static_cast<int64_t>(value.runtime.numa_node));
  runtime.insert("physical_port",
                 static_cast<int64_t>(value.runtime.physical_port));
  runtime.insert("iterations", static_cast<int64_t>(value.runtime.iterations));
  runtime.insert("window_seconds",
                 static_cast<int64_t>(value.runtime.window_seconds));
  runtime.insert("app_tx_batch_size",
                 static_cast<int64_t>(value.runtime.app_tx_batch_size));
  runtime.insert("app_rx_batch_size",
                 static_cast<int64_t>(value.runtime.app_rx_batch_size));
  runtime.insert("dispatcher_tx_batch_size",
                 static_cast<int64_t>(value.runtime.dispatcher_tx_batch_size));
  runtime.insert("dispatcher_rx_batch_size",
                 static_cast<int64_t>(value.runtime.dispatcher_rx_batch_size));
  runtime.insert("nic_tx_post_size",
                 static_cast<int64_t>(value.runtime.nic_tx_post_size));
  runtime.insert("nic_rx_post_size",
                 static_cast<int64_t>(value.runtime.nic_rx_post_size));
  root.insert("runtime", std::move(runtime));

  toml::table network;
  network.insert("local_ip", value.network.local_ip);
  network.insert("remote_ip", value.network.remote_ip);
  network.insert("local_mac", value.network.local_mac);
  network.insert("remote_mac", value.network.remote_mac);
  network.insert("device_pcie", value.network.device_pcie);
  network.insert("device_name", value.network.device_name);
  root.insert("network", std::move(network));

  toml::table metrics;
  metrics.insert("jsonl_path", value.metrics.jsonl_path.string());
  metrics.insert("human_output", value.metrics.human_output);
  root.insert("metrics", std::move(metrics));

  toml::table deployment;
  deployment.insert("host", value.deployment.host);
  deployment.insert("ssh_port",
                    static_cast<int64_t>(value.deployment.ssh_port));
  deployment.insert("ssh_user", value.deployment.ssh_user);
  deployment.insert("workdir", value.deployment.workdir.string());
  deployment.insert("use_sudo", value.deployment.use_sudo);
  root.insert("deployment", std::move(deployment));

  toml::table tuning;
  tuning.insert("max_iterations",
                static_cast<int64_t>(value.tuning.max_iterations));
  tuning.insert("latency_slo_us", value.tuning.latency_slo_us);
  tuning.insert("warmup_windows",
                static_cast<int64_t>(value.tuning.warmup_windows));
  tuning.insert("sample_windows",
                static_cast<int64_t>(value.tuning.sample_windows));
  tuning.insert("infrastructure_failure_limit",
                static_cast<int64_t>(
                    value.tuning.infrastructure_failure_limit));
  toml::table noise;
  noise.insert("throughput_relative_floor",
               value.tuning.noise.throughput_relative_floor);
  noise.insert("latency_relative_floor",
               value.tuning.noise.latency_relative_floor);
  noise.insert("stage_time_relative_floor",
               value.tuning.noise.stage_time_relative_floor);
  noise.insert("stall_time_relative_floor",
               value.tuning.noise.stall_time_relative_floor);
  noise.insert("miss_rate_percentage_point_floor",
               value.tuning.noise.miss_rate_percentage_point_floor);
  tuning.insert("noise", std::move(noise));
  toml::table resources;
  toml::array application_workspaces;
  for (const uint32_t id : value.tuning.resources.application_workspaces) {
    application_workspaces.push_back(static_cast<int64_t>(id));
  }
  resources.insert("application_workspaces", std::move(application_workspaces));
  toml::array dispatcher_workspaces;
  for (const uint32_t id : value.tuning.resources.dispatcher_workspaces) {
    dispatcher_workspaces.push_back(static_cast<int64_t>(id));
  }
  resources.insert("dispatcher_workspaces", std::move(dispatcher_workspaces));
  tuning.insert("resources", std::move(resources));
  root.insert("tuning", std::move(tuning));

  toml::array workspaces;
  for (const config::WorkspaceConfig& value_workspace : value.workspaces) {
    toml::table workspace;
    workspace.insert("id", static_cast<int64_t>(value_workspace.id));
    workspace.insert("cpu_core", static_cast<int64_t>(value_workspace.cpu_core));
    workspaces.push_back(std::move(workspace));
  }
  root.insert("workspaces", std::move(workspaces));

  toml::array workloads;
  for (const config::WorkloadConfig& value_workload : value.workloads) {
    toml::table workload;
    workload.insert("id", static_cast<int64_t>(value_workload.id));
    toml::array pipeline;
    for (const config::PipelinePhase phase : value_workload.pipeline) {
      pipeline.push_back(std::string(config::to_string(phase)));
    }
    workload.insert("pipeline", std::move(pipeline));
    toml::array remote_dispatchers;
    for (const uint32_t id : value_workload.remote_dispatchers) {
      remote_dispatchers.push_back(static_cast<int64_t>(id));
    }
    workload.insert("remote_dispatchers", std::move(remote_dispatchers));
    toml::array groups;
    for (const config::WorkloadGroupConfig& value_group : value_workload.groups) {
      toml::table group;
      group.insert("dispatcher", static_cast<int64_t>(value_group.dispatcher));
      toml::array applications;
      for (const uint32_t id : value_group.applications) {
        applications.push_back(static_cast<int64_t>(id));
      }
      group.insert("applications", std::move(applications));
      groups.push_back(std::move(group));
    }
    workload.insert("groups", std::move(groups));
    workloads.push_back(std::move(workload));
  }
  root.insert("workloads", std::move(workloads));
  return root;
}

std::string canonical_json(const config::AxioConfig& value) {
  const toml::table table = config_table(value);
  std::ostringstream output;
  output << toml::json_formatter{table} << '\n';
  return output.str();
}

std::string canonical_toml(const config::AxioConfig& value) {
  const toml::table table = config_table(value);
  std::ostringstream output;
  output << toml::toml_formatter{
                table, toml::toml_formatter::default_flags |
                           toml::format_flags::relaxed_float_precision}
         << '\n';
  return output.str();
}

std::string generated_header(const config::AxioConfig& value) {
  const config::BuildConfig& build = value.build;
  std::ostringstream output;
  output << "// Generated by axio-configure. Do not edit.\n"
         << "#pragma once\n\n"
         << "#define AXIO_CONFIG_SCHEMA_VERSION " << value.schema_version << '\n'
         << "#define AXIO_CONFIG_BUILD_FINGERPRINT \""
         << config::build_fingerprint(value) << "\"\n"
         << "#define AXIO_CONFIG_NODE_TYPE "
         << (build.role == config::Role::kClient ? 0 : 1) << '\n'
         << "#define AXIO_CONFIG_DPDK_MODE "
         << (build.backend == config::Backend::kDpdk ? 1 : 0) << '\n'
         << "#define AXIO_CONFIG_ROCE_MODE "
         << (build.backend == config::Backend::kRoce ? 1 : 0) << '\n'
         << "#define AXIO_CONFIG_ROCE_TRANSPORT_TYPE "
         << (build.roce_transport == config::RoceTransport::kRc ? 1 : 0)
         << '\n'
         << "#define AXIO_CONFIG_MTU " << build.mtu << '\n'
         << "#define AXIO_CONFIG_RX_RING_ENTRIES " << build.rx_ring_entries
         << '\n'
         << "#define AXIO_CONFIG_TX_RING_ENTRIES " << build.tx_ring_entries
         << '\n'
         << "#define AXIO_CONFIG_MEMPOOL_SIZE " << build.mempool_size << '\n'
         << "#define AXIO_CONFIG_MEMPOOL_HANDLER "
         << static_cast<unsigned int>(build.mempool_handler) << '\n'
         << "#define AXIO_CONFIG_MEMPOOL_HANDLER_NAME \""
         << config::to_string(build.mempool_handler) << "\"\n"
         << "#define AXIO_CONFIG_MEMPOOL_CACHE_SIZE "
         << build.mempool_cache_size << '\n'
         << "#define AXIO_CONFIG_MESSAGE_HANDLER "
         << static_cast<unsigned int>(build.message_handler) << '\n'
         << "#define AXIO_CONFIG_PACKET_HANDLER "
         << static_cast<unsigned int>(build.packet_handler) << '\n'
         << "#define AXIO_CONFIG_APPLY_NEW_MBUF "
         << (build.apply_new_mbuf ? 1 : 0) << '\n'
         << "#define AXIO_CONFIG_REQUEST_PAYLOAD_BYTES "
         << build.request_payload_bytes << '\n'
         << "#define AXIO_CONFIG_RESPONSE_PAYLOAD_BYTES "
         << build.response_payload_bytes << '\n'
         << "#define AXIO_CONFIG_APP_TICKS_PER_MESSAGE "
         << build.app_ticks_per_message << '\n'
         << "#define AXIO_CONFIG_INFLIGHT_LIMIT_ENABLED "
         << (build.inflight_limit_enabled ? 1 : 0) << '\n'
         << "#define AXIO_CONFIG_INFLIGHT_MESSAGES "
         << build.inflight_messages << '\n';
  return output.str();
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) return {};
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

fs::path temporary_path(const fs::path& output) {
  fs::path temporary = output;
  temporary += ".tmp." + std::to_string(static_cast<unsigned long>(getpid()));
  return temporary;
}

void write_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open output " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output.good()) {
    throw std::runtime_error("failed to write output " + path.string());
  }
}

void commit_temporary(const fs::path& temporary, const fs::path& output,
                      const std::string& contents) {
  if (fs::exists(output) && read_file(output) == contents) {
    fs::remove(temporary);
    return;
  }
  fs::rename(temporary, output);
}

void write_atomic(const fs::path& output, const std::string& contents) {
  const fs::path temporary = temporary_path(output);
  try {
    write_file(temporary, contents);
    commit_temporary(temporary, output, contents);
  } catch (...) {
    std::error_code error;
    fs::remove(temporary, error);
    throw;
  }
}

void write_validated_toml(const fs::path& output, const std::string& contents) {
  const fs::path temporary = temporary_path(output);
  try {
    write_file(temporary, contents);
    const config::AxioConfig reparsed = config::load_config(temporary);
    require_valid(reparsed);
    commit_temporary(temporary, output, contents);
  } catch (...) {
    std::error_code error;
    fs::remove(temporary, error);
    throw;
  }
}

std::vector<std::string> split_key(const std::string& key) {
  std::vector<std::string> parts;
  std::istringstream input(key);
  std::string part;
  while (std::getline(input, part, '.')) {
    if (part.empty()) throw std::runtime_error("invalid empty override key");
    parts.push_back(part);
  }
  if (parts.empty()) throw std::runtime_error("override key must not be empty");
  return parts;
}

void assign_scalar(toml::table* table, const std::string& key,
                   const toml::node& existing, const JsonScalar& value,
                   const std::string& full_key) {
  if (existing.is_integer() && std::holds_alternative<int64_t>(value)) {
    table->insert_or_assign(key, std::get<int64_t>(value));
    return;
  }
  if (existing.is_floating_point() && std::holds_alternative<double>(value)) {
    table->insert_or_assign(key, std::get<double>(value));
    return;
  }
  if (existing.is_boolean() && std::holds_alternative<bool>(value)) {
    table->insert_or_assign(key, std::get<bool>(value));
    return;
  }
  if (existing.is_string() && std::holds_alternative<std::string>(value)) {
    table->insert_or_assign(key, std::get<std::string>(value));
    return;
  }
  throw std::runtime_error("override type mismatch for " + full_key);
}

void apply_override(toml::table* root, const std::string& key,
                    const JsonScalar& value) {
  const std::vector<std::string> parts = split_key(key);
  toml::table* table = root;
  for (size_t index = 0; index < parts.size(); ++index) {
    toml::node* node = table->get(parts[index]);
    if (node == nullptr) {
      throw std::runtime_error("unknown override key " + key);
    }
    if (index + 1 == parts.size()) {
      assign_scalar(table, parts[index], *node, value, key);
      return;
    }
    table = node->as_table();
    if (table == nullptr) {
      throw std::runtime_error("override path is not a table: " + key);
    }
  }
}

config::Role parse_role(const std::string& value) {
  if (value == "client") return config::Role::kClient;
  if (value == "server") return config::Role::kServer;
  throw std::runtime_error("role must be client or server");
}

config::Backend parse_backend(const std::string& value) {
  if (value == "dpdk") return config::Backend::kDpdk;
  if (value == "roce") return config::Backend::kRoce;
  throw std::runtime_error("backend must be dpdk or roce");
}

void print_usage() {
  std::cerr
      << "usage:\n"
      << "  axio-configure validate CONFIG\n"
      << "  axio-configure validate-pair LOCAL PEER\n"
      << "  axio-configure dump CONFIG\n"
      << "  axio-configure generate CONFIG OUTPUT\n"
      << "  axio-configure materialize INPUT OUTPUT --set-json JSON\n"
      << "  axio-configure migrate-legacy INPUT OUTPUT --role ROLE "
         "--backend BACKEND\n";
}

int run_command(int argc, char** argv) {
  if (argc < 3) {
    print_usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "validate" && argc == 3) {
    require_valid(config::load_config(argv[2]));
    std::cout << "valid\n";
    return 0;
  }
  if (command == "validate-pair" && argc == 4) {
    require_valid(config::load_config(argv[2]));
    require_valid(config::load_config(argv[3]));
    std::cout << "valid pair\n";
    return 0;
  }
  if (command == "dump" && argc == 3) {
    const config::AxioConfig value = config::load_config(argv[2]);
    require_valid(value);
    std::cout << canonical_json(value);
    return 0;
  }
  if (command == "generate" && argc == 4) {
    const config::AxioConfig value = config::load_config(argv[2]);
    require_valid(value);
    write_atomic(argv[3], generated_header(value));
    return 0;
  }
  if (command == "materialize" && argc == 6 &&
      std::string(argv[4]) == "--set-json") {
    const config::AxioConfig value = config::load_config(argv[2]);
    require_valid(value);
    toml::table table = config_table(value);
    const std::map<std::string, JsonScalar> overrides =
        JsonObjectParser(argv[5]).parse();
    for (const auto& [key, override_value] : overrides) {
      apply_override(&table, key, override_value);
    }
    std::ostringstream output;
    output << toml::toml_formatter{
                  table, toml::toml_formatter::default_flags |
                             toml::format_flags::relaxed_float_precision}
           << '\n';
    write_validated_toml(argv[3], output.str());
    return 0;
  }
  if (command == "migrate-legacy" && argc == 8) {
    std::string role;
    std::string backend;
    for (int index = 4; index < argc; index += 2) {
      if (index + 1 >= argc) break;
      if (std::string(argv[index]) == "--role") role = argv[index + 1];
      if (std::string(argv[index]) == "--backend") backend = argv[index + 1];
    }
    if (role.empty() || backend.empty()) {
      throw std::runtime_error("migrate-legacy requires --role and --backend");
    }
    const config::AxioConfig value = config::load_legacy_config(
        argv[2], parse_role(role), parse_backend(backend));
    require_valid(value);
    write_validated_toml(argv[3], canonical_toml(value));
    return 0;
  }

  print_usage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run_command(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "axio-configure: " << error.what() << '\n';
    return 2;
  }
}
