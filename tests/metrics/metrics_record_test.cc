#include "metrics/metrics_record.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace metrics = axio::metrics;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_metric_value_availability_is_explicit() {
  const metrics::MetricValue unavailable = metrics::MetricValue::unavailable();
  const metrics::MetricValue available = metrics::MetricValue::from_value(12.5);
  expect(!unavailable.available, "unavailable metric must be explicit");
  expect(available.available && available.value == 12.5,
         "available metric must retain its value");
}

void test_record_defaults_to_the_public_schema() {
  const metrics::MetricsRecord record;
  expect(record.schema == "axio.metrics/v1",
         "record must default to the public v1 schema");
  expect(!record.measurement_valid,
         "an unpopulated record must not claim valid measurement data");
}

void test_build_metadata_has_explicit_fallbacks() {
  expect(!metrics::build_version().empty(), "build version must not be empty");
  expect(!metrics::build_git_commit().empty(),
         "build Git revision must not be empty");
}

void test_run_id_is_deterministic_and_auditable() {
  expect(metrics::make_run_id(123, 456) == "pid-123-monotonic-ns-456",
         "run ID must retain process and monotonic-start identity");
}

}  // namespace

int main() {
  try {
    test_metric_value_availability_is_explicit();
    test_record_defaults_to_the_public_schema();
    test_build_metadata_has_explicit_fallbacks();
    test_run_id_is_deterministic_and_auditable();
    std::cout << "Axio metrics record test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio metrics record test failed: " << error.what()
              << std::endl;
    return 1;
  }
}
