/**
 * @file config_validator.h
 * @brief Validate cross-field invariants in typed Axio configuration.
 */
#pragma once

#include "axio/config/config_types.h"

#include <string>
#include <vector>

namespace axio::config {

struct ValidationIssue {
  std::string key;
  SourceLocation source;
  std::string message;
};

class ValidationResult {
 public:
  ValidationResult() = default;
  explicit ValidationResult(std::vector<ValidationIssue> issues);

  bool ok() const { return this->issues_.empty(); }
  const std::vector<ValidationIssue>& issues() const { return this->issues_; }
  std::string format() const;

 private:
  std::vector<ValidationIssue> issues_;
};

ValidationResult validate_config(const AxioConfig& config);

}  // namespace axio::config
