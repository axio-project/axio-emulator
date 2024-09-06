
APP=dperf
SRCS-y :=   src/main.cc src/config.cc\
            src/ws_impl/workspace.cc \
            src/dispatcher_impl/dispatcher.cc src/dispatcher_impl/iphdr.cc src/dispatcher_impl/ethhdr.cc \
			src/dispatcher_impl/dpdk/dpdk_dispatcher.cc src/dispatcher_impl/dpdk/dpdk_externs.cc src/dispatcher_impl/dpdk/dpdk_init.cc src/dispatcher_impl/dpdk/dpdk_dispatcher_dataplane.cc \
			src/dispatcher_impl/roce/roce_dispatcher.cc src/dispatcher_impl/roce/roce_dispatcher_dataplane.cc \
            src/util/numautils.cc src/util/huge_alloc.cc

PKGCONF = pkg-config
PKG_CONFIG_PATH=/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
LDFLAGS_STATIC = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --static --libs libdpdk)

ifneq ($(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

CFLAGS += -O3 -g -std=c++17
CFLAGS += -I ./src -I ./src/util -I ./src/dispatcher_impl/dpdk -I ./src/dispatcher_impl/roce
CFLAGS += -I /opt/mellanox/dpdk/include/dpdk -I /opt/mellanox/dpdk/include/aarch64-linux-gnu/dpdk
CFLAGS += -Wno-deprecated-declarations -march=native
LDFLAGS += -lpthread -lrte_bus_pci -lrte_bus_vdev -ldl -lnuma 

build/$(APP): $(SRCS-y)
	mkdir -p build
	g++ $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) -Wl,--no-whole-archive $(LDFLAGS_STATIC)

clean:
	rm -rf build/
