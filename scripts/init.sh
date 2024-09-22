#!/bin/bash
CUR_DIR=$(cd `dirname $0`; pwd)
cd $CUR_DIR
# basic dependencies
sudo apt-get install make build-essential gcc g++ libudev-dev libnl-3-dev libnl-route-3-dev ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc
# Pipetune dependencies
sudo apt install libnuma-dev libgflags-dev numactl libsystemd-dev libidn11-dev
# perf dependencies
sudo apt-get install linux-tools-common linux-tools-generic linux-tools-`uname -r`
sudo apt-get install pcm

# mkdir tmp
mkdir -p ../tmp