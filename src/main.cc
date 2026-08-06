#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <thread>
#include "util/barrier.h"

#include "workspace.h"
#include "config.h"
#include "datapath_pipeline.h"

void ws_main(axio::WsContext* context, uint8_t ws_id, uint8_t ws_type, std::vector<axio::phase_t> *ws_loop, axio::UserConfig *user_config) {
  if (ws_type == 0) {
    return;
  }
  axio::Workspace<axio::AXIO_DISPATCHER_TYPE> ws(context, ws_id, ws_type, user_config->numa_node(), user_config->physical_port(),
                                              ws_loop, user_config);   
  AXIO_INFO("-------------Workspace %u is running-------------\n", ws_id);
  ws.run_event_loop_timeout_st(user_config->iteration_count(), user_config->duration_seconds()); // duration seconds
  // AXIO_INFO("-------------Workspace %u has finished-------------\n", ws_id);
  return;
}

int main(int argc, char **argv) {
  /// Read config file
  #if AXIO_NODE_TYPE == AXIO_SERVER
    #if AXIO_ENABLE_TUNING
      axio::UserConfig *user_config = new axio::UserConfig("./config/recv_config.out");
    #else
      axio::UserConfig *user_config = new axio::UserConfig("./config/recv_config");
    #endif
  #elif AXIO_NODE_TYPE == AXIO_CLIENT
    #if AXIO_ENABLE_TUNING
      axio::UserConfig *user_config = new axio::UserConfig("./config/send_config.out");
    #else
      axio::UserConfig *user_config = new axio::UserConfig("./config/send_config");
    #endif
  #endif
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
    std::vector<axio::phase_t> *ws_loop = new std::vector<axio::phase_t>();
    ws_type = pipeline->generate_workspace_loop(i, ws_loop);

    // Launch workspace
    workspaces[i] = std::thread(ws_main, context, i, ws_type, ws_loop, user_config);
    size_t core = axio::bind_to_core(workspaces[i], user_config->numa_node(), i);
    context->set_cpu_core(i, core);
  }
  for (auto &workspace : workspaces) workspace.join();
  return 0;
}
