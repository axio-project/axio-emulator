#pragma once
#include "util/logger.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/ws_hdr.h"
#include <netinet/udp.h>

namespace axio {
#define AXIO_MBUF_TOTAL_HEADER_LENGTH                                      \
  (sizeof(struct eth_hdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + \
   sizeof(struct ws_hdr))

#define AXIO_MBUF_ETH_HEADER(m) rte_pktmbuf_mtod(m, struct eth_hdr *)
#define AXIO_MBUF_IP_HEADER(m) \
  rte_pktmbuf_mtod_offset(m, struct iphdr*, sizeof(struct eth_hdr))
#define AXIO_MBUF_TCP_HEADER(m)                                      \
  ({                                                                \
    struct tcphdr* tcp_header = nullptr;                             \
    struct eth_hdr* ethernet_header = AXIO_MBUF_ETH_HEADER(m);       \
    if (ethernet_header->type == htons(ETHERTYPE_IP)) {              \
      tcp_header = rte_pktmbuf_mtod_offset(                          \
          m, struct tcphdr*, sizeof(struct eth_hdr) + sizeof(struct iphdr)); \
    } else {                                                         \
      tcp_header = rte_pktmbuf_mtod_offset(                          \
          m, struct tcphdr*,                                        \
          sizeof(struct eth_hdr) + sizeof(struct ip6_hdr));          \
    }                                                               \
    tcp_header;                                                      \
  })
#define AXIO_MBUF_UDP_HEADER(m)                                      \
  rte_pktmbuf_mtod_offset(                                           \
      m, struct udphdr*, sizeof(struct eth_hdr) + sizeof(struct iphdr))
#define AXIO_MBUF_WORKSPACE_HEADER(m)                                \
  rte_pktmbuf_mtod_offset(                                           \
      m, struct ws_hdr*, sizeof(struct eth_hdr) + sizeof(struct iphdr) + \
                            sizeof(struct udphdr))
#define AXIO_MBUF_WORKSPACE_PAYLOAD(m)                               \
  rte_pktmbuf_mtod_offset(                                           \
      m, char*, sizeof(struct eth_hdr) + sizeof(struct iphdr) +      \
                    sizeof(struct udphdr) + sizeof(struct ws_hdr))

#define AXIO_MBUF_IPV6_HEADER(m) \
  rte_pktmbuf_mtod_offset(m, struct ip6_hdr*, sizeof(struct eth_hdr))
#define AXIO_MBUF_ICMPV6_HEADER(m)                                   \
  rte_pktmbuf_mtod_offset(                                           \
      m, struct icmp6_hdr*, sizeof(struct eth_hdr) + sizeof(struct ip6_hdr))

#define AXIO_MBUF_APPEND_HEADER(m, type) \
  (type*)rte_pktmbuf_append(m, sizeof(type))
#define AXIO_MBUF_APPEND_ETH_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct eth_hdr)
#define AXIO_MBUF_APPEND_ARP_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct arphdr)
#define AXIO_MBUF_APPEND_IP_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct iphdr)
#define AXIO_MBUF_APPEND_IPV6_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct ip6_hdr)
#define AXIO_MBUF_APPEND_TCP_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct tcphdr)
#define AXIO_MBUF_APPEND_DATA(m, size) \
  (uint8_t*)rte_pktmbuf_append(m, (size))

[[maybe_unused]] static inline void print_mbuf(rte_mbuf* buffer) {
  eth_hdr* ethernet_header = AXIO_MBUF_ETH_HEADER(buffer);
  char source_mac[64];
  char destination_mac[64];
  eth_addr_to_str(&ethernet_header->s_addr, source_mac);
  eth_addr_to_str(&ethernet_header->d_addr, destination_mac);

  char log[2048] = {0};
  if (ethernet_header->type == htons(ETHERTYPE_IP)) {
    iphdr* ip_header = AXIO_MBUF_IP_HEADER(buffer);
    udphdr* udp_header = AXIO_MBUF_UDP_HEADER(buffer);
    ws_hdr* workspace_header = AXIO_MBUF_WORKSPACE_HEADER(buffer);
    snprintf(
        log, sizeof(log),
        "mbuf: %s -> %s " IPV4_FMT ":%u ->" IPV4_FMT
        ":%u proto %u ws_type: %u ws_seg: %lu payload_size: %lu\n",
        source_mac, destination_mac, IPV4_STR(ip_header->saddr),
        ntohs(udp_header->source), IPV4_STR(ip_header->daddr),
        ntohs(udp_header->dest), ip_header->protocol,
        workspace_header->workload_type_, workspace_header->segment_num_,
        strlen(reinterpret_cast<char*>(workspace_header) + sizeof(ws_hdr)));
  } else if (ethernet_header->type == htons(ETHERTYPE_IPV6)) {
    ip6_hdr* ipv6_header = AXIO_MBUF_IPV6_HEADER(buffer);
    snprintf(log, sizeof(log),
             "mbuf: %s -> %s " IPV6_FMT " ->" IPV6_FMT " proto %u\n",
             source_mac, destination_mac, IPV6_STR(ipv6_header->ip6_src),
             IPV6_STR(ipv6_header->ip6_dst), ipv6_header->ip6_nxt);
  } else if (ethernet_header->type == htons(ETHERTYPE_ARP)) {
    snprintf(log, sizeof(log), "mbuf: %s -> %s arp\n", source_mac,
             destination_mac);
  } else {
    snprintf(log, sizeof(log), "mbuf: %s -> %s type %x\n", source_mac,
             destination_mac, ntohs(ethernet_header->type));
  }
  AXIO_INFO("%s", log);
}

}  // namespace axio
