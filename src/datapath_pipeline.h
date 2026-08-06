/**
 * @file datapath_pipeline.h
 * @brief Define the workspace execution pipeline.
 */
#pragma once

#include "common.h"
#include "config.h"
#include "util/logger.h"
#include "workspace.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace axio {

class DatapathPipeline {
 private:
  struct PipePhase {
    uint8_t phase_type_ = 0;
    std::vector<uint8_t> workspace_ids_;
    std::vector<phase_t> loop_;
    std::vector<std::string> loop_names_;

    bool contains_workspace(uint8_t workspace_id) const {
      return std::find(this->workspace_ids_.begin(), this->workspace_ids_.end(),
                       workspace_id) != this->workspace_ids_.end();
    }
  };

  struct WorkloadPipeline {
    uint8_t workload_type_ = kInvalidWorkloadType;
    std::vector<PipePhase> phases_;
  };

 public:
  explicit DatapathPipeline(const UserConfig::WorkloadsConfig& config) {
    for (const auto& workload : config.pipeline_phases_) {
      const uint8_t workload_type = workload.first;
      this->_add_workload(workload_type);

      const auto& application_workspaces =
          config.application_workspaces_.at(workload_type);
      const auto& dispatchers = config.dispatchers_.at(workload_type);
      for (const auto& phase_name : workload.second) {
        const uint8_t phase_type = this->phase_types_[phase_name];
        if (phase_type == kTxApplicationType || phase_type == kRxApplicationType) {
          this->_add_phase(workload_type, phase_type, application_workspaces);
        } else if (phase_type == kTxDispatcherType ||
                   phase_type == kRxDispatcherType) {
          this->_add_phase(workload_type, phase_type, dispatchers);
        } else if (phase_type == kTxNICType || phase_type == kRxNICType) {
          this->_add_phase(workload_type, phase_type);
        } else {
          AXIO_ERROR("Invalid pipeline phase type %u\n", phase_type);
          return;
        }
      }
    }
  }

  void print() const {
    std::cout << "----------------------" << YELLOW << "Pipeline Configuration"
              << RESET << "----------------------" << std::endl;
    for (const auto& workload : this->workload_pipelines_) {
      printf("Workload type %u:\n", workload.first);
      for (const auto& phase : workload.second.phases_) {
        printf("  Phase type %s (launched at workspace",
               this->phase_type_names_.at(phase.phase_type_).c_str());
        for (const uint8_t workspace_id : phase.workspace_ids_) {
          printf(" %u", workspace_id);
        }
        printf("):\n");
        for (const auto& function_name : phase.loop_names_) {
          printf("    Func executed: ");
          std::cout << BLUE << function_name << RESET << std::endl;
        }
      }
    }
    std::cout << "----------------------" << YELLOW
              << "Pipeline Configuration END" << RESET
              << "----------------------" << std::endl;
  }

  uint8_t generate_workspace_loop(uint8_t workspace_id,
                                  std::vector<phase_t>* workspace_loop) const {
    uint8_t workspace_type = 0;
    for (const auto& workload : this->workload_pipelines_) {
      for (const auto& phase : workload.second.phases_) {
        if (!phase.contains_workspace(workspace_id)) {
          continue;
        }

        if (phase.phase_type_ == kTxApplicationType ||
            phase.phase_type_ == kRxApplicationType) {
          workspace_type |= WORKER;
        } else if (phase.phase_type_ == kTxDispatcherType ||
                   phase.phase_type_ == kRxDispatcherType) {
          workspace_type |= DISPATCHER;
        } else if (phase.phase_type_ == kTxNICType ||
                   phase.phase_type_ == kRxNICType) {
          workspace_type |= NIC_OFFLOAD;
        } else {
          AXIO_ERROR("Invalid pipeline phase type %u\n", phase.phase_type_);
        }

#ifdef OneStage
        if (phase.phase_type_ == OneStage ||
            (phase.phase_type_ == kTxDispatcherType && OneStage == kTxNICType) ||
            (phase.phase_type_ == kRxDispatcherType && OneStage == kRxNICType)) {
#endif
          for (const auto function : phase.loop_) {
            if (std::find(workspace_loop->begin(), workspace_loop->end(), function) ==
                workspace_loop->end()) {
              workspace_loop->push_back(function);
            }
          }
#ifdef OneStage
        }
        if ((phase.phase_type_ == kTxDispatcherType &&
             OneStage == kTxDispatcherType) ||
            (phase.phase_type_ == kRxDispatcherType && OneStage == kRxNICType)) {
          workspace_loop->pop_back();
        } else if ((phase.phase_type_ == kTxDispatcherType &&
                    OneStage == kTxNICType) ||
                   (phase.phase_type_ == kRxDispatcherType &&
                    OneStage == kRxDispatcherType)) {
          const auto function = workspace_loop->back();
          workspace_loop->pop_back();
          workspace_loop->pop_back();
          workspace_loop->push_back(function);
        }
#endif
      }
    }
    return workspace_type;
  }

  uint8_t workload_type(uint8_t workspace_id) const {
    for (const auto& workload : this->workload_pipelines_) {
      for (const auto& phase : workload.second.phases_) {
        if (phase.contains_workspace(workspace_id)) {
          return workload.first;
        }
      }
    }
    return kInvalidWorkloadType;
  }

 private:
  static constexpr uint8_t kInvalidPhaseType = 6;

  void _add_workload(uint8_t workload_type) {
    if (this->workload_pipelines_.count(workload_type) > 0) {
      AXIO_ERROR("Workload type %u already exists\n", workload_type);
      return;
    }
    this->workload_pipelines_.emplace(
        workload_type, WorkloadPipeline{workload_type, {}});
  }

  PipePhase _make_phase(uint8_t phase_type) {
    rt_assert(phase_type < kInvalidPhaseType, "Invalid pipeline phase type\n");
    PipePhase phase;
    phase.phase_type_ = phase_type;
    phase.loop_ = this->phase_loops_[phase_type];
    phase.loop_names_ = this->phase_loop_names_[phase_type];
    return phase;
  }

  void _add_phase(uint8_t workload_type, uint8_t phase_type) {
    if (this->workload_pipelines_.count(workload_type) == 0) {
      AXIO_ERROR("Workload type %u does not exist in the pipeline\n", workload_type);
      return;
    }
    this->workload_pipelines_.at(workload_type).phases_.push_back(
        this->_make_phase(phase_type));
  }

  void _add_phase(uint8_t workload_type, uint8_t phase_type,
                  const std::vector<uint8_t>& workspace_ids) {
    if (this->workload_pipelines_.count(workload_type) == 0) {
      AXIO_ERROR("Workload type %u does not exist in the pipeline\n", workload_type);
      return;
    }
    PipePhase phase = this->_make_phase(phase_type);
    phase.workspace_ids_ = workspace_ids;
    this->workload_pipelines_.at(workload_type).phases_.push_back(std::move(phase));
  }

  void _add_phase(uint8_t workload_type, uint8_t phase_type,
                  const std::vector<std::vector<uint8_t>>& workspace_groups) {
    if (this->workload_pipelines_.count(workload_type) == 0) {
      AXIO_ERROR("Workload type %u does not exist in the pipeline\n", workload_type);
      return;
    }
    PipePhase phase = this->_make_phase(phase_type);
    for (const auto& workspace_group : workspace_groups) {
      phase.workspace_ids_.insert(phase.workspace_ids_.end(),
                                  workspace_group.begin(), workspace_group.end());
    }
    this->workload_pipelines_.at(workload_type).phases_.push_back(std::move(phase));
  }

  std::map<uint8_t, WorkloadPipeline> workload_pipelines_;
  std::map<std::string, uint8_t> phase_types_ = {
      {"TxApplication", kTxApplicationType},
      {"TxDispatcher", kTxDispatcherType},
      {"TxNIC", kTxNICType},
      {"RXNIC", kRxNICType},
      {"RXDispatcher", kRxDispatcherType},
      {"RxApplication", kRxApplicationType},
  };
  std::map<uint8_t, std::string> phase_type_names_ = {
      {kTxApplicationType, "TxApplication"},
      {kTxDispatcherType, "TxDispatcher"},
      {kTxNICType, "TxNIC"},
      {kRxNICType, "RXNIC"},
      {kRxDispatcherType, "RXDispatcher"},
      {kRxApplicationType, "RxApplication"},
  };
  std::map<uint8_t, std::vector<phase_t>> phase_loops_ = {
      {kTxApplicationType,
       {&Workspace<DISPATCHER_TYPE>::apply_mbufs,
        &Workspace<DISPATCHER_TYPE>::generate_pkts}},
      {kTxDispatcherType,
       {&Workspace<DISPATCHER_TYPE>::bursted_tx,
        &Workspace<DISPATCHER_TYPE>::nic_tx}},
      {kTxNICType, {}},
      {kRxNICType, {}},
      {kRxApplicationType, {&Workspace<DISPATCHER_TYPE>::app_handler}},
      {kRxDispatcherType,
       {&Workspace<DISPATCHER_TYPE>::nic_rx,
        &Workspace<DISPATCHER_TYPE>::bursted_rx}},
  };
  std::map<uint8_t, std::vector<std::string>> phase_loop_names_ = {
      {kTxApplicationType, {"apply_mbufs", "generate_pkts"}},
      {kTxDispatcherType, {"bursted_tx", "nic_tx"}},
      {kTxNICType, {}},
      {kRxNICType, {}},
      {kRxApplicationType, {"app_handler"}},
      {kRxDispatcherType, {"nic_rx", "bursted_rx"}},
  };
};

}  // namespace axio
