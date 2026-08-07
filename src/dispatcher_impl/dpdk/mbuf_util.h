#pragma once
#include "dispatcher_impl/arphdr.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "util/logger.h"
#include "ws_impl/workspace_header.h"

#include <netinet/udp.h>
#include <rte_mbuf.h>

namespace axio {
#define AXIO_MBUF_TOTAL_HEADER_LENGTH                                  \
  (sizeof(EthernetHeader) + sizeof(struct iphdr) + sizeof(struct udphdr) + \
   sizeof(WorkspaceHeader))

#define AXIO_MBUF_ETH_HEADER(m) rte_pktmbuf_mtod(m, EthernetHeader*)
#define AXIO_MBUF_IP_HEADER(m) \
  rte_pktmbuf_mtod_offset(m, struct iphdr*, sizeof(EthernetHeader))
#define AXIO_MBUF_TCP_HEADER(m)                                         \
  ({                                                                   \
    struct tcphdr* tcp_header = nullptr;                                \
    EthernetHeader* ethernet_header = AXIO_MBUF_ETH_HEADER(m);          \
    if (ethernet_header->ether_type_ == htons(ETHERTYPE_IP)) {          \
      tcp_header = rte_pktmbuf_mtod_offset(                             \
          m, struct tcphdr*, sizeof(EthernetHeader) + sizeof(iphdr));   \
    } else {                                                            \
      tcp_header = rte_pktmbuf_mtod_offset(                             \
          m, struct tcphdr*, sizeof(EthernetHeader) + sizeof(ip6_hdr)); \
    }                                                                  \
    tcp_header;                                                         \
  })
#define AXIO_MBUF_UDP_HEADER(m)                                         \
  rte_pktmbuf_mtod_offset(                                              \
      m, struct udphdr*, sizeof(EthernetHeader) + sizeof(struct iphdr))
#define AXIO_MBUF_WORKSPACE_HEADER(m)                                  \
  rte_pktmbuf_mtod_offset(                                             \
      m, WorkspaceHeader*, sizeof(EthernetHeader) + sizeof(iphdr) +    \
                               sizeof(udphdr))
#define AXIO_MBUF_WORKSPACE_PAYLOAD(m)                                 \
  rte_pktmbuf_mtod_offset(                                             \
      m, char*, sizeof(EthernetHeader) + sizeof(iphdr) + sizeof(udphdr) + \
                    sizeof(WorkspaceHeader))

#define AXIO_MBUF_IPV6_HEADER(m) \
  rte_pktmbuf_mtod_offset(m, struct ip6_hdr*, sizeof(EthernetHeader))
#define AXIO_MBUF_ICMPV6_HEADER(m)                                    \
  rte_pktmbuf_mtod_offset(                                            \
      m, struct icmp6_hdr*, sizeof(EthernetHeader) + sizeof(ip6_hdr))

#define AXIO_MBUF_APPEND_HEADER(m, type) \
  reinterpret_cast<type*>(rte_pktmbuf_append(m, sizeof(type)))
#define AXIO_MBUF_APPEND_ETH_HEADER(m) \
  AXIO_MBUF_APPEND_HEADER(m, EthernetHeader)
#define AXIO_MBUF_APPEND_ARP_HEADER(m) \
  AXIO_MBUF_APPEND_HEADER(m, ArpHeader)
#define AXIO_MBUF_APPEND_IP_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct iphdr)
#define AXIO_MBUF_APPEND_IPV6_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct ip6_hdr)
#define AXIO_MBUF_APPEND_TCP_HEADER(m) AXIO_MBUF_APPEND_HEADER(m, struct tcphdr)
#define AXIO_MBUF_APPEND_DATA(m, size) \
  reinterpret_cast<uint8_t*>(rte_pktmbuf_append(m, (size)))

[[maybe_unused]] static inline void print_mbuf(rte_mbuf* buffer) {
  EthernetHeader* ethernet_header = AXIO_MBUF_ETH_HEADER(buffer);
  char source_mac[64];
  char destination_mac[64];
  format_ethernet_address(&ethernet_header->source_, source_mac);
  format_ethernet_address(&ethernet_header->destination_, destination_mac);

  char log[2048] = {0};
  if (ethernet_header->ether_type_ == htons(ETHERTYPE_IP)) {
    iphdr* ip_header = AXIO_MBUF_IP_HEADER(buffer);
    udphdr* udp_header = AXIO_MBUF_UDP_HEADER(buffer);
    WorkspaceHeader* workspace_header = AXIO_MBUF_WORKSPACE_HEADER(buffer);
    snprintf(
        log, sizeof(log),
        "mbuf: %s -> %s " AXIO_IPV4_FORMAT ":%u ->" AXIO_IPV4_FORMAT
        ":%u proto %u ws_type: %u ws_seg: %lu payload_size: %lu\n",
        source_mac, destination_mac, AXIO_IPV4_BYTES(ip_header->saddr),
        ntohs(udp_header->source), AXIO_IPV4_BYTES(ip_header->daddr),
        ntohs(udp_header->dest), ip_header->protocol,
        workspace_header->workload_type_, workspace_header->segment_num_,
        strlen(reinterpret_cast<char*>(workspace_header) + sizeof(WorkspaceHeader)));
  } else if (ethernet_header->ether_type_ == htons(ETHERTYPE_IPV6)) {
    ip6_hdr* ipv6_header = AXIO_MBUF_IPV6_HEADER(buffer);
    snprintf(log, sizeof(log),
             "mbuf: %s -> %s " AXIO_IPV6_FORMAT " ->" AXIO_IPV6_FORMAT " proto %u\n",
             source_mac, destination_mac, AXIO_IPV6_WORDS(ipv6_header->ip6_src),
             AXIO_IPV6_WORDS(ipv6_header->ip6_dst), ipv6_header->ip6_nxt);
  } else if (ethernet_header->ether_type_ == htons(ETHERTYPE_ARP)) {
    snprintf(log, sizeof(log), "mbuf: %s -> %s arp\n", source_mac,
             destination_mac);
  } else {
    snprintf(log, sizeof(log), "mbuf: %s -> %s type %x\n", source_mac,
             destination_mac, ntohs(ethernet_header->ether_type_));
  }
  AXIO_INFO("%s", log);
}

}  // namespace axio
