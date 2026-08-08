#include <sys/types.h>

#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"
#include "axio/config/runtime_options.h"
#include "axio/config/topology.h"
#include "config.h"
#include "datapath_pipeline.h"
#include "metrics/metrics_writer.h"
#include "util/barrier.h"
#include "workspace.h"

namespace {

static_assert(axio::config::kRuntimeWorkspaceLimit == axio::kWorkspaceMaxNum);
static_assert(axio::config::kRuntimeWorkloadIdLimit == axio::kMaxWorkloadNum);

void ws_main(axio::WsContext* context, uint8_t ws_id, uint8_t ws_type,
             std::vector<axio::WorkspacePhase>* workspace_loop,
             axio::UserConfig* user_config, size_t global_core) {
  axio::bind_current_thread_to_core(global_core);
  if (ws_type == 0) {
    return;
  }
  axio::Workspace<axio::AXIO_DISPATCHER_TYPE> ws(
      context, ws_id, ws_type, user_config->numa_node(),
      user_config->physical_port(), workspace_loop, user_config);
  AXIO_INFO("-------------Workspace %u is running-------------\n", ws_id);
  ws.run_event_loop_timeout_st(user_config->iteration_count(),
                               user_config->duration_seconds());
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<axio::config::RuntimeOptions> options =
      axio::config::parse_runtime_options(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: axio --config LOCAL [--peer-config PEER]"
              << std::endl;
    return 2;
  }

  axio::config::AxioConfig typed_config;
  std::unique_ptr<axio::UserConfig> user_config;
  std::string build_fingerprint;
  try {
    typed_config = axio::config::load_config(options->config_path);
    axio::config::ValidationResult validation;
    if (options->peer_config_path.has_value()) {
      const axio::config::AxioConfig peer_config =
          axio::config::load_config(*options->peer_config_path);
      validation =
          axio::config::validate_config_pair(typed_config, peer_config);
    } else {
      validation = axio::config::validate_config(typed_config);
    }
    if (!validation.ok()) {
      std::cerr << validation.format() << std::endl;
      return 2;
    }
    const axio::BuildFingerprintComparison fingerprint =
        axio::compare_build_fingerprint(typed_config);
    if (!fingerprint.matches()) {
      std::cerr << "Axio build fingerprint mismatch: binary="
                << fingerprint.embedded_fingerprint << ", config="
                << fingerprint.config_fingerprint << std::endl;
      return 2;
    }
    build_fingerprint = fingerprint.embedded_fingerprint;
    user_config = std::make_unique<axio::UserConfig>(typed_config);
  } catch (const std::exception& error) {
    std::cerr << "Axio configuration error: " << error.what() << std::endl;
    return 2;
  }

  std::unique_ptr<axio::metrics::MetricsWriter> metrics_writer;
  std::unique_ptr<axio::metrics::MetricsPublisher> metrics_publisher;
  try {
    metrics_writer = std::make_unique<axio::metrics::MetricsWriter>(
        user_config->metrics_jsonl_path(), user_config->metrics_enabled());
    metrics_publisher = std::make_unique<axio::metrics::MetricsPublisher>(
        metrics_writer.get(), user_config->human_output_enabled(), &std::cout);
  } catch (const std::exception& error) {
    std::cerr << "Axio metrics initialization error: " << error.what()
              << std::endl;
    return 2;
  }

  const uint64_t monotonic_start_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  axio::metrics::MetricsRunMetadata metrics_metadata;
  metrics_metadata.run_id = axio::metrics::make_run_id(
      static_cast<uint64_t>(getpid()), monotonic_start_ns);
  metrics_metadata.role =
      std::string(axio::config::to_string(typed_config.deployment.role));
  metrics_metadata.backend =
      std::string(axio::config::to_string(typed_config.network.backend));
  metrics_metadata.build_fingerprint = build_fingerprint;
  metrics_metadata.config_fingerprint = user_config->config_fingerprint();

  user_config->print();

  /// Init datapath pipeline
  axio::DatapathPipeline pipeline(user_config->topology());
  pipeline.print();

  const std::vector<axio::config::WorkspaceId>& active_workspaces =
      user_config->topology().active_workspace_ids();
  const uint8_t total_thread_num =
      static_cast<uint8_t>(active_workspaces.size());
  printf("Total launched %u threads!\n", total_thread_num);

  const auto start_sync_workspace = std::find_if(
      active_workspaces.begin(), active_workspaces.end(),
      [&user_config](axio::config::WorkspaceId workspace_id) {
        return axio::config::has_role(
            user_config->topology().roles(workspace_id),
            axio::config::WorkspaceRole::kDispatcher);
      });
  if (start_sync_workspace == active_workspaces.end()) {
    std::cerr << "Axio topology error: no dispatcher workspace is active"
              << std::endl;
    return 2;
  }

  /// Init workspace context based on datapath pipeline
  axio::ThreadBarrier barrier(total_thread_num);
  axio::WsContext context(&barrier, metrics_publisher.get(),
                          std::move(metrics_metadata),
                          user_config->metrics_enabled(),
                          static_cast<uint8_t>(start_sync_workspace->value()));

  const std::vector<size_t> numa_cores =
      axio::get_lcores_for_numa_node(user_config->numa_node());
  user_config->topology().validate_cpu_core_capacity(numa_cores.size());

  /// Init and launch workspaces
  axio::clear_affinity_for_process();
  std::vector<std::vector<axio::WorkspacePhase>> workspace_loops(
      active_workspaces.size());
  std::vector<std::thread> workspaces;
  workspaces.reserve(active_workspaces.size());
  for (size_t index = 0; index < active_workspaces.size(); ++index) {
    const axio::config::WorkspaceId workspace_id = active_workspaces[index];
    const uint8_t runtime_workspace_id =
        static_cast<uint8_t>(workspace_id.value());
    /// Get workspace type and pipeline loop for a given workspace
    const uint8_t ws_type = pipeline.generate_workspace_loop(
        runtime_workspace_id, &workspace_loops[index]);

    // Launch workspace
    const size_t numa_local_core = user_config->topology()
                                       .workspace(workspace_id)
                                       .cpu_core.value();
    const size_t global_core = numa_cores.at(numa_local_core);
    context.set_cpu_core(runtime_workspace_id, global_core);
    workspaces.emplace_back(ws_main, &context, runtime_workspace_id, ws_type,
                            &workspace_loops[index], user_config.get(),
                            global_core);
  }
  for (std::thread& workspace : workspaces) workspace.join();
  return 0;
}
