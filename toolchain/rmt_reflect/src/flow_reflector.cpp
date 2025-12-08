#include "flow_reflector.h"

#include <stdio.h>
#include <string.h>
#include <rte_flow.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>

/* Maximum number of flow rules */
#define MAX_FLOW_RULES 16

/* Global array to store flow handles */
static struct rte_flow *flow_rules[RTE_MAX_ETHPORTS][MAX_FLOW_RULES];
static int flow_rule_count[RTE_MAX_ETHPORTS] = {0};

/**
 * Print flow error
 */
static void
__print_flow_error(struct rte_flow_error *error)
{
    if (error && error->message) {
        printf("Flow error: %s\n", error->message);
    }
}

/**
 * Add a flow rule to the global tracking array
 */
static int
__add_flow_rule(uint16_t port_id, struct rte_flow *flow)
{
    if (flow_rule_count[port_id] >= MAX_FLOW_RULES) {
        printf("Maximum number of flow rules reached for port %u\n", port_id);
        return -1;
    }
    
    flow_rules[port_id][flow_rule_count[port_id]++] = flow;
    return 0;
}

/**
 * Setup hairpin queues for zero-copy packet reflection
 */
int
setup_hairpin_queues(uint16_t port_id, uint16_t rx_queue_id, uint16_t tx_queue_id)
{
    struct rte_eth_hairpin_conf hairpin_conf;
    int ret;
    
    memset(&hairpin_conf, 0, sizeof(hairpin_conf));
    hairpin_conf.peer_count = 1;
    hairpin_conf.peers[0].port = port_id;
    hairpin_conf.peers[0].queue = tx_queue_id;

    printf("Setting up hairpin: RX queue %u -> TX queue %u on port %u\n",
           rx_queue_id, tx_queue_id, port_id);

    /* Setup RX hairpin queue */
    ret = rte_eth_rx_hairpin_queue_setup(port_id, rx_queue_id, 1024, &hairpin_conf);
    if (ret != 0) {
        printf("Failed to setup RX hairpin queue %u on port %u: %s\n",
               rx_queue_id, port_id, rte_strerror(-ret));
        return ret;
    }

    /* Setup TX hairpin queue - peer back to RX */
    hairpin_conf.peers[0].queue = rx_queue_id;
    ret = rte_eth_tx_hairpin_queue_setup(port_id, tx_queue_id, 1024, &hairpin_conf);
    if (ret != 0) {
        printf("Failed to setup TX hairpin queue %u on port %u: %s\n",
               tx_queue_id, port_id, rte_strerror(-ret));
        return ret;
    }

    printf("Hairpin queues setup successfully\n");
    return 0;
}

/**
 * Create flow rule to swap MAC addresses and redirect to hairpin TX queue
 */
static int
__create_mac_swap_flow(uint16_t port_id, uint16_t hairpin_tx_queue)
{
    struct rte_flow_attr attr = {
        .ingress = 1,
    };
    struct rte_flow_item pattern[2];
    struct rte_flow_action actions[4];
    struct rte_flow_error error;
    struct rte_flow *flow;
    int action_idx = 0;

    memset(pattern, 0, sizeof(pattern));
    memset(actions, 0, sizeof(actions));

    /* Match all packets */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = NULL;
    pattern[0].mask = NULL;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    /* Action: Swap MAC addresses if supported */
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_MAC_SWAP;
    action_idx++;

    /* Action: Send to hairpin TX queue */
    struct rte_flow_action_queue queue_action = {
        .index = hairpin_tx_queue,
    };
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[action_idx].conf = &queue_action;
    action_idx++;

    /* Action: End */
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_END;

    /* Validate the flow rule */
    if (rte_flow_validate(port_id, &attr, pattern, actions, &error) != 0) {
        printf("Flow validation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    /* Create the flow rule */
    flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        printf("Flow creation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    printf("Created MAC swap flow rule\n");
    return __add_flow_rule(port_id, flow);
}

/**
 * Create flow rule with packet modifications for reflection
 * This is a more complex approach that modifies packet fields
 */
static int
__create_packet_modify_flow(uint16_t port_id, uint16_t tx_queue)
{
    struct rte_flow_attr attr = {
        .ingress = 1,
    };
    struct rte_flow_item pattern[4];
    struct rte_flow_action actions[8];
    struct rte_flow_error error;
    struct rte_flow *flow;
    int action_idx = 0;

    memset(pattern, 0, sizeof(pattern));
    memset(actions, 0, sizeof(actions));

    /* Match UDP packets */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    /* Action 1: Swap MAC addresses */
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_MAC_SWAP;
    action_idx++;

    /* Action 2: Send to TX queue */
    struct rte_flow_action_queue queue_action = {
        .index = tx_queue,
    };
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[action_idx].conf = &queue_action;
    action_idx++;

    /* Action 3: End */
    actions[action_idx].type = RTE_FLOW_ACTION_TYPE_END;

    /* Validate the flow rule */
    if (rte_flow_validate(port_id, &attr, pattern, actions, &error) != 0) {
        printf("UDP flow validation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    /* Create the flow rule */
    flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        printf("UDP flow creation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    printf("Created UDP packet modify flow rule\n");
    return __add_flow_rule(port_id, flow);
}

/**
 * Create a simple loopback flow rule
 */
static int
__create_simple_loopback_flow(uint16_t port_id, uint16_t tx_queue)
{
    struct rte_flow_attr attr = {
        .ingress = 1,
    };
    struct rte_flow_item pattern[2];
    struct rte_flow_action actions[3];
    struct rte_flow_error error;
    struct rte_flow *flow;

    memset(pattern, 0, sizeof(pattern));
    memset(actions, 0, sizeof(actions));

    /* Match all Ethernet packets */
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    /* Action: Queue to TX */
    struct rte_flow_action_queue queue_action = {
        .index = tx_queue,
    };
    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue_action;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;

    /* Validate and create flow */
    if (rte_flow_validate(port_id, &attr, pattern, actions, &error) != 0) {
        printf("Simple loopback flow validation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        printf("Simple loopback flow creation failed: ");
        __print_flow_error(&error);
        return -1;
    }

    printf("Created simple loopback flow rule\n");
    return __add_flow_rule(port_id, flow);
}

/**
 * Setup hardware-based packet reflection using rte_flow
 */
int
setup_flow_reflection(uint16_t port_id)
{
    int ret;

    printf("\n=== Setting up hardware-based packet reflection ===\n");
    printf("Port: %u\n", port_id);

    /* Initialize flow rule tracking */
    flow_rule_count[port_id] = 0;

    /* Try to create MAC swap flow with hairpin queue */
    printf("\nAttempting to create MAC swap flow rule...\n");
    ret = __create_mac_swap_flow(port_id, 0);
    if (ret == 0) {
        printf("✓ Hardware MAC swap reflection configured successfully\n");
        return 0;
    }

    /* If MAC swap failed, try packet modify flow */
    printf("\nMAC swap not supported, trying packet modify flow...\n");
    ret = __create_packet_modify_flow(port_id, 0);
    if (ret == 0) {
        printf("✓ Hardware packet modify reflection configured successfully\n");
        return 0;
    }

    /* If both failed, try simple loopback */
    printf("\nPacket modify not supported, trying simple loopback...\n");
    ret = __create_simple_loopback_flow(port_id, 0);
    if (ret == 0) {
        printf("⚠ Simple loopback configured (no packet modification)\n");
        printf("  Note: Packets will be looped back without MAC/IP swap\n");
        return 0;
    }

    printf("\n✗ Failed to setup any hardware reflection flows\n");
    printf("  Your NIC may not support rte_flow hardware offload\n");
    return -1;
}

/**
 * Teardown hardware-based packet reflection
 */
void
teardown_flow_reflection(uint16_t port_id)
{
    struct rte_flow_error error;
    int i;

    printf("Tearing down flow rules for port %u\n", port_id);

    for (i = 0; i < flow_rule_count[port_id]; i++) {
        if (flow_rules[port_id][i]) {
            if (rte_flow_destroy(port_id, flow_rules[port_id][i], &error) != 0) {
                printf("Failed to destroy flow rule %d: ", i);
                __print_flow_error(&error);
            }
            flow_rules[port_id][i] = NULL;
        }
    }

    flow_rule_count[port_id] = 0;

    /* Flush all flows to be safe */
    rte_flow_flush(port_id, &error);
    
    printf("Flow rules teardown complete\n");
}

