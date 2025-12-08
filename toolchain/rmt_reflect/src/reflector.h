#ifndef REFLECTOR_H
#define REFLECTOR_H

#include <rte_mbuf.h>

/**
 * Reflect a packet by swapping Ethernet addresses, IP addresses, and UDP ports
 * 
 * @param mbuf
 *   The packet mbuf to reflect
 */
void reflect_packet(struct rte_mbuf *mbuf);

#endif /* REFLECTOR_H */

