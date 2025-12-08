# RMT Reflect - DPDK Packet Reflector

High-performance DPDK-based packet reflector supporting both software and hardware reflection modes.

## Features

- 🚀 **Software Reflection Mode**: CPU-based processing with MAC/IP/UDP port swapping
- ⚡ **Hardware Flow Reflection Mode**: Offload processing to NIC using rte_flow (zero CPU)
- 🎯 PCIe device address specification support
- 📊 Real-time statistics display

## Build

```bash
# Configure build
meson setup build

# Compile
ninja -C build
```

## Usage

### Basic Syntax

```bash
sudo ./build/rmt_reflect -- [options]
```

### Options

- `-a PCIE_ADDR` : Specify PCIe address of the device (e.g., `0000:ca:00.0`)
- `-f, --flow`   : Enable hardware flow offload mode
- `-h`           : Display help message

### Examples

#### 1. Software Reflection Mode (CPU Processing)

```bash
sudo ./build/rmt_reflect -- -a 0000:ca:00.0
```

Suitable for debugging and functional verification.

#### 2. Hardware Flow Reflection Mode (NIC Processing)

```bash
sudo ./build/rmt_reflect -- -a 0000:ca:00.0 -f
```

All packet processing is offloaded to NIC hardware with near-zero CPU usage.

## Troubleshooting

### 1. "Error: no available ports"

**Causes**:
- Incorrect PCIe address format (correct: `0000:ca:00.0`, not `0000:00:ca.0`)
- Device not recognized by DPDK or not bound to appropriate driver

**Solutions**:

Check available devices:
```bash
lspci | grep -i ethernet
```

For Mellanox NICs (ConnectX series), no driver binding needed - use directly.

For Intel and other NICs, bind to DPDK driver first:
```bash
# Bind to vfio-pci
sudo modprobe vfio-pci
sudo dpdk-devbind.py -b vfio-pci 0000:ca:00.0
```

### 2. Permission Denied

The program requires root privileges to access hardware devices. Use `sudo`.

### 3. Hardware Flow Mode Failure

If the NIC doesn't support rte_flow hardware offload, the program automatically falls back to software reflection mode.

## Performance Comparison

| Mode | CPU Usage | Latency | Throughput |
|------|-----------|---------|------------|
| Software Reflection | ~100% (single core) | Microseconds | CPU-limited |
| Hardware Flow | ~0% | Nanoseconds | Line rate (10/25/100 Gbps) |

## Reflection Behavior

The program performs the following operations on each received packet:

1. **Swap Ethernet addresses**: Source MAC ↔ Destination MAC
2. **Swap IP addresses**: Source IP ↔ Destination IP (IPv4)
3. **Swap ports**: Source port ↔ Destination port (UDP/TCP)
4. **Update checksums**: Automatically recalculate IPv4 checksum

The modified packet is sent back to the source port.

## Technical Details

- **DPDK Version**: 22.11.3
- **Supported NICs**: All DPDK-compatible network cards
- **Hardware Flow Support**: Requires NIC with rte_flow API support (e.g., Mellanox ConnectX-5+)
- **Build Standard**: C++17

## Project Structure

```
rmt_reflect/
├── src/
│   ├── main.cpp              # Main program and DPDK initialization
│   ├── reflector.h           # Software reflection interface
│   ├── reflector.cpp         # Software reflection implementation
│   ├── flow_reflector.h      # Hardware flow reflection interface
│   └── flow_reflector.cpp    # Hardware flow reflection implementation
├── meson.build               # Build configuration
├── VERSION                   # Version number
└── README.md                 # This file
```

## License

Part of the AXIO Emulator project.

