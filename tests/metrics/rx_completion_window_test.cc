#include "metrics/rx_completion_window.h"

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
  return condition;
}

bool near(double actual, double expected) {
  return std::fabs(actual - expected) < 1e-9;
}

bool test_cumulative_slope_excludes_anchor_burst() {
  axio::metrics::RxCompletionWindow window;
  window.observe({100, 7}, 4);
  window.observe_empty_poll();
  window.observe({180, 7}, 2);
  window.observe({300, 7}, 3);

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(sample.total_completion_count == 9,
                "total completion count must include the anchor burst") &&
         expect(sample.timed_completion_count == 5,
                "timed completion count must exclude the anchor burst") &&
         expect(sample.successful_poll_count == 3,
                "successful polls must be counted") &&
         expect(sample.empty_poll_count == 1,
                "empty polls must be counted separately") &&
         expect(sample.mean_interval_cycles.has_value(),
                "two successful observations must produce an interval") &&
         expect(near(*sample.mean_interval_cycles, 40.0),
                "interval must use the cumulative completion slope");
}

bool test_cpu_migration_invalidates_interval() {
  axio::metrics::RxCompletionWindow window;
  window.observe({100, 7}, 4);
  window.observe({180, 8}, 2);

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(!sample.clock_valid,
                "a changed TSC AUX CPU ID must invalidate the clock") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "a migrated window must not publish an interval");
}

bool test_non_monotonic_tsc_invalidates_interval() {
  axio::metrics::RxCompletionWindow window;
  window.observe({200, 7}, 1);
  window.observe({199, 7}, 1);

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(!sample.clock_valid,
                "a non-monotonic TSC must invalidate the clock") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "a non-monotonic window must not publish an interval");
}

bool test_completion_error_invalidates_interval() {
  axio::metrics::RxCompletionWindow window;
  window.observe({100, 7}, 1);
  window.observe({140, 7}, 2);
  window.observe_completion_errors(1);

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(sample.completion_error_count == 1,
                "completion errors must be retained") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "a completion error must suppress the interval");
}

bool test_reset_starts_an_independent_window() {
  axio::metrics::RxCompletionWindow window;
  window.observe({100, 7}, 4);
  window.observe({180, 7}, 2);
  window.observe_empty_poll();
  window.observe_completion_errors(1);
  window.reset();

  window.observe({500, 9}, 3);
  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(sample.window_generation == 1,
                "reset must advance the window generation") &&
         expect(sample.total_completion_count == 3,
                "reset must clear completion counts") &&
         expect(sample.successful_poll_count == 1,
                "reset must clear successful poll counts") &&
         expect(sample.empty_poll_count == 0,
                "reset must clear empty poll counts") &&
         expect(sample.completion_error_count == 0,
                "reset must clear completion errors") &&
         expect(sample.clock_valid, "reset must restore clock validity") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "one post-reset observation must not publish an interval");
}

bool test_zero_success_observation_is_empty() {
  axio::metrics::RxCompletionWindow window;
  window.observe({100, 7}, 0);
  window.observe({200, 7}, 2);

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(sample.total_completion_count == 2,
                "zero success must not change completion count") &&
         expect(sample.successful_poll_count == 1,
                "zero success must not establish a successful poll") &&
         expect(sample.empty_poll_count == 1,
                "zero success must be recorded as an empty poll") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "one real observation must remain insufficient");
}

}  // namespace

int main() {
  if (!test_cumulative_slope_excludes_anchor_burst()) {
    return 1;
  }
  if (!test_cpu_migration_invalidates_interval()) {
    return 1;
  }
  if (!test_non_monotonic_tsc_invalidates_interval()) {
    return 1;
  }
  if (!test_completion_error_invalidates_interval()) {
    return 1;
  }
  if (!test_reset_starts_an_independent_window()) {
    return 1;
  }
  if (!test_zero_success_observation_is_empty()) {
    return 1;
  }
  std::puts("Axio RX completion window test passed");
  return 0;
}
