#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include "util/barrier.h"

#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"
#include "workspace.h"
#include "config.h"
#include "datapath_pipeline.h"

void ws_main(axio::WsContext* context, uint8_t ws_id, uint8_t ws_type,
             std::vector<axio::WorkspacePhase>* workspace_loop,
             axio::UserConfig* user_config) {
  if (ws_type == 0) {
    return;
  }
  axio::Workspace<axio::AXIO_DISPATCHER_TYPE> ws(
      context, ws_id, ws_type, user_config->numa_node(),
      user_config->physical_port(), workspace_loop, user_config);
  AXIO_INFO("-------------Workspace %u is running-------------\n", ws_id);
  ws.run_event_loop_timeout_st(user_config->iteration_count(), user_config->duration_seconds()); // duration seconds
  // AXIO_INFO("-------------Workspace %u has finished-------------\n", ws_id);
  return;
}

int main(int argc, char **argv) {
  if (argc != 3 || std::string(argv[1]) != "--config") {
    std::cerr << "usage: axio --config FILE" << std::endl;
    return 2;
  }

  axio::config::AxioConfig typed_config;
  std::unique_ptr<axio::UserConfig> user_config;
  try {
    typed_config = axio::config::load_config(argv[2]);
    const axio::config::ValidationResult validation =
        axio::config::validate_config(typed_config);
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
    user_config = std::make_unique<axio::UserConfig>(typed_config);
  } catch (const std::exception& error) {
    std::cerr << "Axio configuration error: " << error.what() << std::endl;
    return 2;
  }

  user_config->print();

  /// Init datapath pipeline
  axio::DatapathPipeline *pipeline =
      new axio::DatapathPipeline(user_config->workloads());
  pipeline->print();

  uint8_t total_thread_num = 0;
  for (uint8_t i = 0; i < axio::kWorkspaceMaxNum; i++) {
    if (pipeline->workload_type(i) != axio::kInvalidWorkloadType)
      total_thread_num++;
  }
  printf("Total launched %u threads!\n", total_thread_num);

  /// Init workspace context based on datapath pipeline
  axio::ThreadBarrier *barrier = new axio::ThreadBarrier(total_thread_num);
  axio::WsContext *context = new axio::WsContext(barrier);

  /// Init and launch workspaces
  axio::clear_affinity_for_process();
  std::vector<std::thread> workspaces(axio::kWorkspaceMaxNum);
  for (uint8_t i = 0; i < axio::kWorkspaceMaxNum; i++) {
    /// Get workspace type and pipeline loop for a given workspace
    uint8_t ws_type = axio::kInvalidWorkspaceType;
    auto* workspace_loop = new std::vector<axio::WorkspacePhase>();
    ws_type = pipeline->generate_workspace_loop(i, workspace_loop);

    // Launch workspace
    workspaces[i] =
        std::thread(ws_main, context, i, ws_type, workspace_loop,
                    user_config.get());
    size_t core =
        axio::bind_to_core(workspaces[i], user_config->numa_node(), i);
    context->set_cpu_core(i, core);
  }
  for (auto &workspace : workspaces) workspace.join();
  return 0;
}
