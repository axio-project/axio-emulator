#!/bin/bash
CUR_DIR=$(cd `dirname $0`; pwd)
cd $CUR_DIR
# if the build folder exists, remove it
if [ -d "build" ]; then
    ninja -C build uninstall
    rm -rf build
fi
cd dpdk-stable-22.11.3
meson setup -Dexamples=all -Denable_kmods=false -Dtests=false -Ddisable_drivers='raw/*,crypto/*,baseband/*,dma/*' build
meson configure -Dprefix=${CUR_DIR}/dpdk-stable-22.11.3/build/install build
ninja -C build install