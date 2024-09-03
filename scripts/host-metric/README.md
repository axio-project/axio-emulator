record-host-metrics.sh can be used to record cpu utilization, pcie throughput, memory bandwidth, llc usage, etc., during the runtime of other applications.

## Quickstart
* **prerequisite:**
  * installed `pcm, perf, sar` tool
*  **run script:** `sudo bash record-host-metrics.sh --dur=5 --cpu_util=1 --cores=8,9 --pcie=1 --membw=1 --llc=1`
   *  `--dur=5` sets the test time(i.e.,5s) for each metric
   *  `--cpu_util=1` enables cpu utilization test
   *  `--cores=8,9` specifies cpu cores for cpu utilization test
   *  `--pcie=1` enables pcie throughput test
   *  `--membw=1` enables memory bandwidth test
   *  `--llc=1` enable last level cache performance test
*  **get result:**
   *  logs for each test are put in `logs` directory
   *  test results for all test is `reports/report.rpt`
*  **note:**
   *  if machine changes, remember to change device address in function `parse_pciebw`, default is the CX7 addr in Desktop.