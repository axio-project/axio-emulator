#ifndef FLOW_REFLECTOR_H
#define FLOW_REFLECTOR_H

#include <stdint.h>
#include <rte_flow.h>

/**
 * Setup hardware-based packet reflection using rte_flow
 * 
 * This function creates flow rules to reflect packets at hardware level,
 * swapping MAC addresses, IP addresses, and UDP/TCP ports.
 * 
 * @param port_id
 *   The port ID to setup reflection on
 * @return
 *   0 on success, negative error code on failure
 */
int setup_flow_reflection(uint16_t port_id);

/**
 * Teardown hardware-based packet reflection
 * 
 * @param port_id
 *   The port ID to teardown reflection on
 */
void teardown_flow_reflection(uint16_t port_id);

/**
 * Setup hairpin queues for hardware packet loopback
 * 
 * @param port_id
 *   The port ID to setup hairpin on
 * @param rx_queue_id
 *   RX hairpin queue ID
 * @param tx_queue_id
 *   TX hairpin queue ID
 * @return
 *   0 on success, negative error code on failure
 */
int setup_hairpin_queues(uint16_t port_id, uint16_t rx_queue_id, uint16_t tx_queue_id);

#endif /* FLOW_REFLECTOR_H */

