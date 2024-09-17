# PipeTune
PipeTune is an efficient performance tuning framework for host datapaths. It correlates three crucial datapath configurations (i.e., core number, queue number and batch size) with memory efficiency, further translating to datapath performance to derive effective tuning strategies. Building upon them, we implemented PipeTune to automatically search for the optimal configuration values to achieve consistently high performance.

The detail of PipeTune is described in our paper: [Tuning Host Datapath Performance with PipeTune](https://github.com/Huangxy-Minel/Paper-DPerf).

## Features
- **Emulation**: PipeTune provides two types of emulation hooks, i.e., message-based handler and packet-based handler, to emulate real-world applications.
- **Parameters Tuning**: PipeTune can automatically search for the optimal configuration values of core number, queue number and batch size.

The following figure shows the architecture of PipeTune.
![PipeTune Architecture](figs/pipetune_overview.pdf)

## Quick Start
The following instructions will help you to quickly set up PipeTune on your machine. 
### Test Environment
- Ubuntu 22.04
- Linux kernel 5.15.x
- DPDK 22.11
- Intel(R) Xeon(R) Silver 4309Y CPU @ 2.80GHz
- two-port 200G Ethernet Mellanox Connect-X 7
- PCIe 4.0 x 16
- 512GB DDR5 3200MT/s

### Install Prerequisites
Install with package manager (e.g., apt):
```bash
bash init_dependencies.sh
```
Install DPDK, if you have not installed it:
```bash
tar -xvf third_party/dpdk-22.11.3.tar.xz
bash third_party/build_dpdk.sh
```
Install Mellanox OFED, if you have not installed it. Please refer to the [official website](https://www.mellanox.com/products/infiniband-drivers/linux/mlnx_ofed) for installation.

### Build PipeTune
```bash
meson build
ninja
```
