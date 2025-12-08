#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_flow.h>

#include "reflector.h"
#include "flow_reflector.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static volatile bool force_quit = false;

/* Application arguments */
struct app_args {
    char pcie_addr[64];
    bool use_pcie_addr;
    bool use_hardware_flow;  // Use rte_flow hardware offload
};

/* Port configuration */
static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mtu = RTE_ETHER_MAX_LEN,
    },
};

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

static void
print_usage(const char *prgname)
{
    printf("Usage: %s [EAL options] -- [application options]\n"
           "Application options:\n"
           "  -a PCIE_ADDR : Specify PCIe address of the device (e.g., 0000:00:04.0)\n"
           "  -f, --flow   : Use hardware flow offload (rte_flow) for packet reflection\n"
           "  -h           : Display this help message\n\n"
           "Modes:\n"
           "  Without -f: Software reflection using CPU cores\n"
           "  With -f:    Hardware reflection using NIC flow rules (zero CPU)\n",
           prgname);
}

static int
parse_app_args(int argc, char **argv, struct app_args *args)
{
    int opt;
    static struct option long_options[] = {
        {"flow", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    /* Initialize default values */
    args->use_pcie_addr = false;
    args->use_hardware_flow = false;
    memset(args->pcie_addr, 0, sizeof(args->pcie_addr));
    
    while ((opt = getopt_long(argc, argv, "a:fh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'a':
            if (strlen(optarg) >= sizeof(args->pcie_addr)) {
                printf("Error: PCIe address too long\n");
                return -1;
            }
            strncpy(args->pcie_addr, optarg, sizeof(args->pcie_addr) - 1);
            args->use_pcie_addr = true;
            printf("Using PCIe device: %s\n", args->pcie_addr);
            break;
        case 'f':
            args->use_hardware_flow = true;
            printf("Hardware flow offload mode enabled\n");
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }
    
    return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf_local = port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error during getting device (port %u) info: %s\n",
                port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf_local.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf_local);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf_local.txmode.offloads;
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    /* Display the port MAC address. */
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 ":%02" PRIx8 ":%02" PRIx8
               ":%02" PRIx8 ":%02" PRIx8 ":%02" PRIx8 "\n",
            port,
            addr.addr_bytes[0], addr.addr_bytes[1],
            addr.addr_bytes[2], addr.addr_bytes[3],
            addr.addr_bytes[4], addr.addr_bytes[5]);

    /* Enable RX in promiscuous mode for the Ethernet device. */
    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

/*
 * The main function for software packet reflection.
 */
static void
lcore_main_software(uint16_t port)
{
    printf("\nCore %u doing SOFTWARE packet reflection on port %u. [Ctrl+C to quit]\n",
            rte_lcore_id(), port);

    /* Run until the application is quit or killed. */
    while (!force_quit) {
        struct rte_mbuf *bufs[BURST_SIZE];
        const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

        if (unlikely(nb_rx == 0))
            continue;

        /* Process and reflect packets */
        for (uint16_t i = 0; i < nb_rx; i++) {
            reflect_packet(bufs[i]);
        }

        /* Send burst of TX packets */
        const uint16_t nb_tx = rte_eth_tx_burst(port, 0, bufs, nb_rx);

        /* Free any unsent packets. */
        if (unlikely(nb_tx < nb_rx)) {
            for (uint16_t i = nb_tx; i < nb_rx; i++)
                rte_pktmbuf_free(bufs[i]);
        }
    }
}

/*
 * The main function for hardware flow reflection mode.
 * In this mode, the NIC handles all packet reflection in hardware.
 */
static void
lcore_main_hardware_flow(uint16_t port)
{
    uint64_t prev_cycles = rte_get_timer_cycles();
    uint64_t stats_interval = rte_get_timer_hz(); // 1 second
    struct rte_eth_stats stats, prev_stats;
    
    printf("\nHARDWARE FLOW reflection active on port %u. [Ctrl+C to quit]\n", port);
    printf("All packet processing is offloaded to NIC - zero CPU usage!\n\n");
    
    memset(&prev_stats, 0, sizeof(prev_stats));
    rte_eth_stats_get(port, &prev_stats);

    /* Just monitor statistics while hardware does the work */
    while (!force_quit) {
        uint64_t cur_cycles = rte_get_timer_cycles();
        
        /* Sleep to reduce CPU usage - hardware is doing all the work */
        rte_delay_ms(100);
        
        /* Print statistics every second */
        if (cur_cycles - prev_cycles >= stats_interval) {
            rte_eth_stats_get(port, &stats);
            
            printf("\r[Hardware Flow Stats] RX: %lu pkts (%lu MB) | TX: %lu pkts (%lu MB) | "
                   "Errors: %lu | Missed: %lu",
                   stats.ipackets,
                   stats.ibytes / (1024*1024),
                   stats.opackets,
                   stats.obytes / (1024*1024),
                   stats.ierrors + stats.oerrors,
                   stats.imissed);
            fflush(stdout);
            
            prev_cycles = cur_cycles;
        }
    }
    
    printf("\n");
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
    struct rte_mempool *mbuf_pool;
    unsigned nb_ports;
    uint16_t portid = 0;
    struct app_args app_args;
    int eal_argc;
    char *eal_argv[32];
    int arg_idx = 0;

    /* Build EAL arguments */
    eal_argv[arg_idx++] = argv[0];
    eal_argv[arg_idx++] = (char *)"-c";
    eal_argv[arg_idx++] = (char *)"0x1";  // Use core 0
    eal_argv[arg_idx++] = (char *)"-n";
    eal_argv[arg_idx++] = (char *)"4";    // Memory channels
    
    /* Parse application arguments to check for PCIe address */
    int app_arg_start = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            app_arg_start = i;
            break;
        }
    }
    
    if (app_arg_start > 0) {
        /* Parse application arguments first to get PCIe address */
        int app_argc = argc - app_arg_start;
        char **app_argv = &argv[app_arg_start];
        int parse_ret = parse_app_args(app_argc, app_argv, &app_args);
        if (parse_ret != 0) {
            if (parse_ret > 0) {
                return 0;  // Help was displayed
            }
            return -1;     // Error occurred
        }
        
        /* Add PCIe address to EAL arguments if specified */
        if (app_args.use_pcie_addr) {
            eal_argv[arg_idx++] = (char *)"-a";
            eal_argv[arg_idx++] = app_args.pcie_addr;
        }
    } else {
        app_args.use_pcie_addr = false;
    }
    
    eal_argv[arg_idx] = NULL;
    eal_argc = arg_idx;

    /* Initialize the Environment Abstraction Layer (EAL). */
    printf("Initializing DPDK EAL with %d arguments...\n", eal_argc);
    int ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    /* Register signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 1)
        rte_exit(EXIT_FAILURE, "Error: no available ports\n");

    printf("Number of available ports: %u\n", nb_ports);

    /* Use the first available port */
    portid = 0;

    /* Creates a new mempool in memory to hold the mbufs. */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    /* Initialize port. */
    if (port_init(portid, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);

    printf("Initialized port %u\n", portid);
    
    /* Setup hardware flow reflection if requested */
    if (app_args.use_hardware_flow) {
        printf("\n=== Hardware Flow Reflection Mode ===\n");
        if (setup_flow_reflection(portid) != 0) {
            printf("Failed to setup hardware flow reflection\n");
            printf("Falling back to software reflection mode\n");
            app_args.use_hardware_flow = false;
        }
    }
    
    if (app_args.use_hardware_flow) {
        printf("\nStarting hardware flow packet reflector...\n");
        /* Call hardware flow monitoring on the main core */
        lcore_main_hardware_flow(portid);
        
        /* Cleanup flow rules */
        teardown_flow_reflection(portid);
    } else {
        printf("\n=== Software Reflection Mode ===\n");
        printf("Starting software packet reflector...\n");
        /* Call software reflection on the main core */
        lcore_main_software(portid);
    }

    /* Clean up */
    printf("\nCleaning up and shutting down...\n");
    ret = rte_eth_dev_stop(portid);
    if (ret != 0)
        printf("rte_eth_dev_stop: err=%d, port=%u\n", ret, portid);

    rte_eth_dev_close(portid);
    rte_eal_cleanup();

    printf("Bye...\n");
    return 0;
}

