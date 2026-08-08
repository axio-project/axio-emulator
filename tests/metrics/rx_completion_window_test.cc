#include "axio/receive_burst_result.h"
#include "metrics/rx_completion_window.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

bool test_queue_aggregation_preserves_capacity_and_slowest_queue() {
  const std::vector<axio::metrics::QueueCompletionInterval> queues = {
      {20.0, 3},
      {40.0, 1},
  };
  const axio::metrics::CompletionIntervalAggregate aggregate =
      axio::metrics::aggregate_completion_intervals(queues);

  return expect(aggregate.count_weighted_interval_cycles.has_value(),
                "valid queues must produce a weighted interval") &&
         expect(near(*aggregate.count_weighted_interval_cycles, 25.0),
                "weighted interval must use completion counts") &&
         expect(aggregate.slowest_interval_cycles.has_value(),
                "valid queues must preserve the slowest interval") &&
         expect(near(*aggregate.slowest_interval_cycles, 40.0),
                "slowest interval must not be hidden") &&
         expect(aggregate.aggregate_rate_per_cycle.has_value(),
                "parallel queues must produce an aggregate rate") &&
         expect(near(*aggregate.aggregate_rate_per_cycle, 0.075),
                "parallel queue rates must be summed") &&
         expect(aggregate.aggregate_capacity_interval_cycles.has_value(),
                "aggregate rate must produce a capacity interval") &&
         expect(near(*aggregate.aggregate_capacity_interval_cycles,
                     1.0 / 0.075),
                "capacity interval must invert the aggregate rate") &&
         expect(aggregate.timed_completion_count == 4,
                "aggregate must retain the total timed completions");
}

bool test_invalid_queue_interval_suppresses_aggregate() {
  const axio::metrics::CompletionIntervalAggregate aggregate =
      axio::metrics::aggregate_completion_intervals({{0.0, 1}});
  return expect(!aggregate.count_weighted_interval_cycles.has_value(),
                "a zero queue interval must suppress the weighted value") &&
         expect(!aggregate.slowest_interval_cycles.has_value(),
                "a zero queue interval must suppress the slowest value") &&
         expect(!aggregate.aggregate_rate_per_cycle.has_value(),
                "a zero queue interval must suppress aggregate rate") &&
         expect(!aggregate.aggregate_capacity_interval_cycles.has_value(),
                "a zero queue interval must suppress capacity interval");
}

bool test_receive_burst_bridge_distinguishes_success_empty_and_error() {
  axio::metrics::RxCompletionWindow window;
  axio::metrics::observe_receive_burst(&window, {4, 0, 100, 7});
  axio::metrics::observe_receive_burst(&window, {});
  axio::metrics::observe_receive_burst(&window, {2, 0, 180, 7});
  axio::metrics::observe_receive_burst(&window, {0, 1, 200, 7});

  const axio::metrics::RxCompletionSnapshot sample = window.snapshot();
  return expect(sample.total_completion_count == 6,
                "bridge must retain successful backend completions") &&
         expect(sample.successful_poll_count == 2,
                "bridge must record only successful backend polls") &&
         expect(sample.empty_poll_count == 1,
                "bridge must distinguish an empty poll from an error") &&
         expect(sample.completion_error_count == 1,
                "bridge must retain backend completion errors") &&
         expect(!sample.mean_interval_cycles.has_value(),
                "backend errors must invalidate an otherwise timed window");
}

bool test_disabled_collection_does_not_touch_the_window() {
  axio::metrics::RxCompletionWindow window;
  const axio::ReceiveBurstResult result = {4, 0, 100, 7};
  axio::metrics::observe_receive_burst_if_enabled(&window, result, false);
  const axio::metrics::RxCompletionSnapshot disabled = window.snapshot();
  if (!expect(disabled.total_completion_count == 0 &&
                  disabled.empty_poll_count == 0,
              "disabled metrics must leave the fast-path window untouched")) {
    return false;
  }
  axio::metrics::observe_receive_burst_if_enabled(&window, result, true);
  return expect(window.snapshot().total_completion_count == 4,
                "enabled metrics must observe successful completions");
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
  if (!test_queue_aggregation_preserves_capacity_and_slowest_queue()) {
    return 1;
  }
  if (!test_invalid_queue_interval_suppresses_aggregate()) {
    return 1;
  }
  if (!test_receive_burst_bridge_distinguishes_success_empty_and_error()) {
    return 1;
  }
  if (!test_disabled_collection_does_not_touch_the_window()) {
    return 1;
  }
  std::puts("Axio RX completion window test passed");
  return 0;
}
