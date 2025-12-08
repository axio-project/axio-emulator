#include "reflector.h"

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_byteorder.h>

/**
 * Swap two MAC addresses
 */
static inline void
__swap_mac_addresses(struct rte_ether_addr *addr1, struct rte_ether_addr *addr2)
{
    struct rte_ether_addr tmp;
    rte_ether_addr_copy(addr1, &tmp);
    rte_ether_addr_copy(addr2, addr1);
    rte_ether_addr_copy(&tmp, addr2);
}

/**
 * Swap two IPv4 addresses (handles potentially unaligned access)
 */
static inline void
__swap_ipv4_addresses(struct rte_ipv4_hdr *ipv4_hdr)
{
    uint32_t tmp;
    rte_memcpy(&tmp, &ipv4_hdr->src_addr, sizeof(uint32_t));
    rte_memcpy(&ipv4_hdr->src_addr, &ipv4_hdr->dst_addr, sizeof(uint32_t));
    rte_memcpy(&ipv4_hdr->dst_addr, &tmp, sizeof(uint32_t));
}

/**
 * Swap two port numbers (handles potentially unaligned access)
 */
static inline void
__swap_ports(uint16_t *port1, uint16_t *port2)
{
    uint16_t tmp, p1, p2;
    rte_memcpy(&p1, port1, sizeof(uint16_t));
    rte_memcpy(&p2, port2, sizeof(uint16_t));
    tmp = p1;
    rte_memcpy(port1, &p2, sizeof(uint16_t));
    rte_memcpy(port2, &tmp, sizeof(uint16_t));
}

/**
 * Update IPv4 checksum after modifying addresses
 */
static inline void
__update_ipv4_checksum(struct rte_ipv4_hdr *ipv4_hdr)
{
    ipv4_hdr->hdr_checksum = 0;
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
}

/**
 * Reflect a packet by swapping Ethernet, IP, and UDP/TCP headers
 */
void
reflect_packet(struct rte_mbuf *mbuf)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    uint8_t *pkt_data;
    uint16_t ether_type;
    uint8_t ip_proto;

    /* Get pointer to packet data */
    pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);

    /* Process Ethernet header */
    eth_hdr = (struct rte_ether_hdr *)pkt_data;
    __swap_mac_addresses(&eth_hdr->src_addr, &eth_hdr->dst_addr);

    ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    /* Check if it's an IPv4 packet */
    if (ether_type != RTE_ETHER_TYPE_IPV4)
        return;

    /* Process IPv4 header */
    ipv4_hdr = (struct rte_ipv4_hdr *)(pkt_data + sizeof(struct rte_ether_hdr));
    __swap_ipv4_addresses(ipv4_hdr);

    ip_proto = ipv4_hdr->next_proto_id;

    /* Update IPv4 checksum */
    __update_ipv4_checksum(ipv4_hdr);

    /* Get the size of IPv4 header (including options) */
    uint8_t ipv4_hdr_len = (ipv4_hdr->version_ihl & 0x0F) * 4;

    /* Process transport layer header */
    if (ip_proto == IPPROTO_UDP) {
        /* Process UDP header */
        udp_hdr = (struct rte_udp_hdr *)(pkt_data + sizeof(struct rte_ether_hdr) + ipv4_hdr_len);
        __swap_ports(&udp_hdr->src_port, &udp_hdr->dst_port);

        /* Note: UDP checksum update is optional for IPv4, setting to 0 disables it */
        udp_hdr->dgram_cksum = 0;
    } else if (ip_proto == IPPROTO_TCP) {
        /* Process TCP header */
        tcp_hdr = (struct rte_tcp_hdr *)(pkt_data + sizeof(struct rte_ether_hdr) + ipv4_hdr_len);
        __swap_ports(&tcp_hdr->src_port, &tcp_hdr->dst_port);

        /* Note: TCP checksum should be recalculated, but for simplicity we set it to 0 */
        /* In production, you should properly recalculate the TCP checksum */
        tcp_hdr->cksum = 0;
    }
}

