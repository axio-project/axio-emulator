/**
 * @file config_loader.h
 * @brief Load Axio TOML schema v1 into typed configuration values.
 */
#pragma once

#include "axio/config/config_types.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace axio::config {

class ConfigError final : public std::runtime_error {
 public:
  ConfigError(std::string key, SourceLocation source, std::string message);

  const std::string& key() const { return this->key_; }
  const SourceLocation& source() const { return this->source_; }

 private:
  std::string key_;
  SourceLocation source_;
};

AxioConfig load_config(const std::filesystem::path& path);

}  // namespace axio::config
