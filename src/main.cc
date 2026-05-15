#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <thread>
#include "util/barrier.h"

#include "workspace.h"
#include "config.h"
#include "datapath_pipeline.h"

void ws_main(dperf::WsContext* context, uint8_t ws_id, uint8_t ws_type, std::vector<dperf::phase_t> *ws_loop, dperf::UserConfig *user_config) {
  if (ws_type == 0) {
    return;
  }
  dperf::Workspace<dperf::DISPATCHER_TYPE> ws(context, ws_id, ws_type, user_config->get_numa(), user_config->get_phy_port(), 
                                              ws_loop, user_config);   
  DPERF_INFO("-------------Workspace %u is running-------------\n", ws_id);
  ws.run_event_loop_timeout_st(user_config->get_iteration(), user_config->get_duration()); // duration seconds
  // DPERF_INFO("-------------Workspace %u has finished-------------\n", ws_id);
  return;
}

int main(int argc, char **argv) {
  /// Read config file
  #if NODE_TYPE == SERVER
    #if ENABLE_TUNE
      dperf::UserConfig *user_config = new dperf::UserConfig("./config/recv_config.out");
    #else
      dperf::UserConfig *user_config = new dperf::UserConfig("./config/recv_config");
    #endif
  #elif NODE_TYPE == CLIENT
    #if ENABLE_TUNE
      dperf::UserConfig *user_config = new dperf::UserConfig("./config/send_config.out");
    #else
      dperf::UserConfig *user_config = new dperf::UserConfig("./config/send_config");
    #endif
  #endif
  user_config->print_config();

  /// Init datapath pipeline
  dperf::DatapathPipeline *pipeline = new dperf::DatapathPipeline(user_config->workloads_config_);
  pipeline->print_pipeline();

  /// [Plan-A] How many ws threads share one physical core.
  /// Set via env: AXIO_QP_PER_CORE=8/16/32/64. Default 1 (original behaviour).
  size_t qp_per_core = 1;
  if (const char* env = std::getenv("AXIO_QP_PER_CORE")) {
    qp_per_core = std::max<size_t>(1, std::atoi(env));
  }
  printf("[Plan-A] AXIO_QP_PER_CORE = %zu\n", qp_per_core);

  uint16_t total_thread_num = 0;
  for (uint16_t i = 0; i < dperf::kWorkspaceMaxNum; i++) {
    if (pipeline->get_workload_type(i) != dperf::kInvalidWorkloadType)
      total_thread_num++;
  }
  printf("Total launched %u threads!\n", total_thread_num);

  /// Init workspace context based on datapath pipeline
  dperf::ThreadBarrier *barrier = new dperf::ThreadBarrier(total_thread_num);
  dperf::WsContext *context = new dperf::WsContext(barrier);

  /// Init and launch workspaces
  dperf::clear_affinity_for_process();
  std::vector<std::thread> workspaces(dperf::kWorkspaceMaxNum);
  size_t live_idx = 0;  // dense index of "real" ws threads, used to compute target core
  for (uint16_t i = 0; i < dperf::kWorkspaceMaxNum; i++) {
    /// Get workspace type and pipeline loop for a given workspace
    uint8_t ws_type = dperf::kInvaildWorkspaceType;
    std::vector<dperf::phase_t> *ws_loop = new std::vector<dperf::phase_t>();
    ws_type = pipeline->generate_ws_loop(i, ws_loop);

    // Launch workspace
    workspaces[i] = std::thread(ws_main, context, (uint8_t)i, ws_type, ws_loop, user_config);

    if (ws_type != 0 && ws_type != dperf::kInvaildWorkspaceType) {
      size_t target_core_local_idx = live_idx / qp_per_core;
      size_t core = dperf::bind_to_core(workspaces[i], user_config->get_numa(), target_core_local_idx);
      context->cpu_core[i] = core;
      printf("[Plan-A] ws %u -> numa-local core %zu (global %zu)\n",
             (unsigned)i, target_core_local_idx, core);
      live_idx++;
    }
  }
  for (auto &workspace : workspaces) workspace.join();
  return 0;
}

