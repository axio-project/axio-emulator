#include <stdio.h>
#include <thread>
#include <vector>
#include "common.h"
#include "util/logger.h"
#include <numa.h>
#include "util/numautils.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>

namespace dperf {

void wait_duration(size_t wait_sec){
  auto start = std::chrono::high_resolution_clock::now();  // 记录开始时间

  while (true) {
      auto current = std::chrono::high_resolution_clock::now();  // 获取当前时间
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(current - start);  // 计算经过的时间

      if (duration.count() >= wait_sec) {
          break;  // 如果经过的时间达到1秒，则退出循环
      }
  }
}

size_t num_lcores_per_numa_node() {
  return static_cast<size_t>(numa_num_configured_cpus() /
                             numa_num_configured_nodes());
}

std::vector<size_t> get_lcores_for_numa_node(size_t numa_node) {
  rt_assert(numa_node <= static_cast<size_t>(numa_max_node()));

  std::vector<size_t> ret;
  size_t num_lcores = static_cast<size_t>(numa_num_configured_cpus());

  for (size_t i = 0; i < num_lcores; i++) {
    if (numa_node == static_cast<size_t>(numa_node_of_cpu(i))) {
      ret.push_back(i);
    }
  }

  return ret;
}

size_t bind_to_core(std::thread &thread, size_t numa_node,
                  size_t numa_local_index) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  rt_assert(numa_node <= kMaxNumaNodes, "Invalid NUMA node");

  const std::vector<size_t> lcore_vec = get_lcores_for_numa_node(numa_node);
  if (numa_local_index >= lcore_vec.size()) {
    DPERF_ERROR(
        "DPerf: Requested binding to core %zu (zero-indexed) on NUMA node %zu, "
        "which has only %zu cores. Ignoring, but this can cause very low "
        "performance.\n",
        numa_local_index, numa_node, lcore_vec.size());
    return -1;
  }

  const size_t global_index = lcore_vec.at(numa_local_index);

  CPU_SET(global_index, &cpuset);
  int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t),
                                  &cpuset);
  rt_assert(rc == 0, "Error setting thread affinity");

  return global_index;
}

void clear_affinity_for_process() {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  const size_t num_cpus = std::thread::hardware_concurrency();
  for (size_t i = 0; i < num_cpus; i++) CPU_SET(i, &mask);

  int ret = sched_setaffinity(0 /* whole-process */, sizeof(cpu_set_t), &mask);
  rt_assert(ret == 0, "Failed to clear CPU affinity for this process");
}

size_t get_global_index(size_t numa_node, size_t numa_local_index) {
  rt_assert(numa_node <= kMaxNumaNodes, "Invalid NUMA node");

  const std::vector<size_t> lcore_vec = get_lcores_for_numa_node(numa_node);
  if (numa_local_index >= lcore_vec.size()) {
    DPERF_ERROR(
        "DPerf: Requested binding to core %zu (zero-indexed) on NUMA node %zu, "
        "which has only %zu cores. Ignoring, but this can cause very low "
        "performance.\n",
        numa_local_index, numa_node, lcore_vec.size());
    return 0;
  }

  return lcore_vec.at(numa_local_index);
}

double get_cpu_freq_max_ghz(size_t core_idx) {
  std::string cpuPath = "/sys/devices/system/cpu/cpu" + std::to_string(core_idx);
  std::string frequencyPath = cpuPath + "/cpufreq/cpuinfo_max_freq";
  std::ifstream file(frequencyPath);
  double freq = 0.0;
  if (file.is_open()) {
    std::string line;
    std::getline(file, line);
    freq = std::stod(line) / 1000.0 / 1000.0;
    file.close();
  }
  else {
    DPERF_ERROR("Cannot open file %s\n", frequencyPath.c_str());
    return 0;
  }
  return freq;
}

size_t get_cpu_freq_max_hz(size_t core_idx) {
  std::string cpuPath = "/sys/devices/system/cpu/cpu" + std::to_string(core_idx);
  std::string frequencyPath = cpuPath + "/cpufreq/cpuinfo_max_freq";
  std::ifstream file(frequencyPath);
  size_t freq = 0;
  if (file.is_open()) {
    std::string line;
    std::getline(file, line);
    freq = std::stod(line);
    file.close();
  }
  else {
    DPERF_ERROR("Cannot open file %s\n", frequencyPath.c_str());
    return 0;
  }
  return freq;
}

double get_cpu_freq_ghz(size_t core_idx) {
  std::string cpuPath = "/sys/devices/system/cpu/cpu" + std::to_string(core_idx);
  std::string frequencyPath = cpuPath + "/cpufreq/scaling_cur_freq";
  std::ifstream file(frequencyPath);
  double freq = 0.0;
  if (file.is_open()) {
    std::string line;
    std::getline(file, line);
    freq = std::stod(line) / 1000.0 / 1000.0;
    file.close();
  }
  else {
    DPERF_WARN("Cannot open file %s, try to read cpu freq from /proc/cpuinfo\n", frequencyPath.c_str());
    std::string cpuinfoPath = "/proc/cpuinfo";
    std::ifstream file(cpuinfoPath);
    if (file.is_open()) {
      std::string line;
      bool flag = false;
      while (std::getline(file, line)) {
        if (line.find("processor") != std::string::npos) {
          size_t idx = std::stoi(line.substr(line.find(":") + 1));
          if (idx != core_idx) {
            continue;
          }
          else
            flag = true;
        }
        if (line.find("cpu MHz") != std::string::npos && flag) {
          freq = std::stod(line.substr(line.find(":") + 1)) / 1000.0;
          break;
        }
      }
      file.close();
    }
    else {
      DPERF_ERROR("Cannot open file %s\n", cpuinfoPath.c_str());
      return 0;
    }
  }
  return freq;
}

bool is_cpu_freq_max(size_t core_idx) {
  return get_cpu_freq_ghz(core_idx) == get_cpu_freq_max_ghz(core_idx);
}

bool pathExists(const std::string& path) {
  struct stat buffer;
  return (stat(path.c_str(), &buffer) == 0);
}

void set_cpu_freq_max(size_t core_idx) {
  std::string cpuPath = "/sys/devices/system/cpu/cpu0/cpufreq";
  if (pathExists(cpuPath)) {
    std::string cmd = "sudo cpufreq-set -c" + std::to_string(core_idx) + " -g performance";
    int res = system(cmd.c_str());
    wait_duration(2);
    // std::string cmd = "sudo cpufreq-set -c " + std::to_string(core_idx) + " -g userspace";
    // int res = system(cmd.c_str());
    // wait_duration(2);
    // cmd = "sudo cpufreq-set -c " + std::to_string(core_idx) + " -f 2000000";
    // res = system(cmd.c_str());
    // wait_duration(2);
    // int target_freq = get_cpu_freq_max_hz(core_idx);
    // double max_fluc = 0.2; // the max fluctuation is +- 0.2GHz
    // uint8_t retry_time = 0;
    // while (true) {
    //   printf("[Core %lu] Try to set cpu freq to %u\n", core_idx, target_freq);
    //   if (target_freq <= 0) {
    //     DPERF_ERROR("Cannot set CPU frequency to a stable state, please check your bios or grub setting~\n");
    //     std::runtime_error("dPerf: Set CPU frequency error");
    //   }
    //   cmd = "sudo cpufreq-set -c " + std::to_string(core_idx) + " -f " + std::to_string(target_freq);
    //   res = system(cmd.c_str());
    //   wait_duration(2);
    //   double cur_freq = get_cpu_freq_ghz(core_idx);
    //   double tar_g_greq = (double)target_freq / 1000.0 / 1000.0;
    //   if (cur_freq > tar_g_greq || std::abs(cur_freq - tar_g_greq) <= max_fluc) {
    //     break;
    //   }
    //   else {
    //     retry_time++;
    //     if (retry_time == 3) {
    //       target_freq -= 100000;  // -= 0.1GHz
    //       retry_time = 0;
    //     }
    //   }
    // }
  } else {
      DPERF_WARN("Cannot leverage cpufreq-set to set CPU frequency to max, try to warm up.\n");
      const int iterations = 10000000;
      double result = 0.0;
      for (int i = 0; i < iterations; ++i) {
          result += std::sin(i) * std::cos(i);
      }
  }
}

void set_cpu_freq_normal(size_t core_idx) {
  std::string cpuPath = "/sys/devices/system/cpu/cpu0/cpufreq";
  if (pathExists(cpuPath)) {
    std::string cmd = "sudo cpufreq-set -c" + std::to_string(core_idx) + " -g ondemand";
    int res = system(cmd.c_str());
  }
  else {
    DPERF_WARN("Cannot leverage cpufreq-set to set CPU frequency to normal, skip.\n");
  }
}

}  // namespace dperf
