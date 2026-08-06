/**
 * @file dpdk_dispatcher_dataplane.cc
 * @brief Define Transmit / Receive functions of DPDK
 */
#include "dpdk_dispatcher.h"
namespace axio {

/// Generate a IP+UDP packet
void DpdkDispatcher::_set_packet_headers(rte_mbuf* buffer) {
  eth_hdr* ethernet_header = AXIO_MBUF_ETH_HEADER(buffer);
  iphdr* ip_header = AXIO_MBUF_IP_HEADER(buffer);
  udphdr* udp_header = AXIO_MBUF_UDP_HEADER(buffer);

  /// set eth header
  ethernet_header->type = htons(ETHERTYPE_IP);
  rte_memcpy(ethernet_header->s_addr.bytes, this->resolve_.mac_addr_.bytes,
             ETH_ADDR_LEN);
  rte_memcpy(ethernet_header->d_addr.bytes, this->destination_mac_->bytes,
             ETH_ADDR_LEN);

  /// set ip header
  ip_header->saddr = this->resolve_.ipv4_addr_.ip;
  ip_header->daddr = this->destination_ip_->ip;
  ip_header->ihl = 5;
  ip_header->version = 4;
  ip_header->tos = 0;
  ip_header->tot_len =
      rte_cpu_to_be_16(buffer->pkt_len - sizeof(eth_hdr));
  ip_header->ttl = 64;
  ip_header->frag_off = IP_FLAG_DF;
  ip_header->protocol = IPPROTO_UDP;
  buffer->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
  buffer->l2_len = sizeof(eth_hdr);
  buffer->l3_len = sizeof(iphdr);

  /// set udp header completely
  udp_header->source += kDefaultUdpPort;
  udp_header->source = rte_cpu_to_be_16(udp_header->source);
  udp_header->dest += kDefaultUdpPort;
  udp_header->dest = rte_cpu_to_be_16(udp_header->dest);
  udp_header->len = rte_cpu_to_be_16(
      buffer->pkt_len - sizeof(eth_hdr) - sizeof(iphdr));
}

uint8_t DpdkDispatcher::_resolve_packet_header(rte_mbuf* buffer) {
  ws_hdr* workspace_header = AXIO_MBUF_WORKSPACE_HEADER(buffer);
  return workspace_header->workload_type_;
}

/// Collect mbufs from all workers' tx queues with Round-Robin mode
size_t DpdkDispatcher::collect_tx_packets() {
  size_t remain_ring_size = kNumTxRingEntries - this->tx_queue_index_;
  uint8_t nb_collect_queue = 0;
  size_t nb_collect_num = 0;
  while (remain_ring_size && nb_collect_queue < this->workspace_tx_queues_.size()) {
    /// select a workspace tx queue
    LockFreeQueue *worker_queue = this->workspace_tx_queues_[this->workspace_queue_index_];
    size_t tx_size = worker_queue->size();
    if (tx_size < this->tx_batch_size()) {
      this->workspace_queue_index_ = (this->workspace_queue_index_ + 1) % this->workspace_tx_queues_.size();
      nb_collect_queue++;
      continue;
    }
    tx_size = (tx_size > remain_ring_size) ? remain_ring_size : tx_size;
    // printf("%lu, %lu\n", worker_queue->head_, worker_queue->tail_);
    for (size_t i = 0; i < tx_size; i++) {
      this->tx_queue_[this->tx_queue_index_] = (rte_mbuf*)worker_queue->dequeue();
      this->_set_packet_headers(this->tx_queue_[this->tx_queue_index_]);
      this->tx_queue_index_++;
    }
    this->workspace_queue_index_ = (this->workspace_queue_index_ + 1) % this->workspace_tx_queues_.size();
    nb_collect_queue++;
    remain_ring_size -= tx_size;
    nb_collect_num += tx_size;
  }
  return nb_collect_num;
}

extern void dpdk_set_buffer_payload(rte_mbuf* buffer, char* udp_header,
                                    char* workspace_header,
                                    size_t payload_size);

void DpdkDispatcher::fill_tx_packets(size_t flow_size, size_t frame_size) {
  rte_mempool* mempool = this->_mempool();
  while(AXIO_UNLIKELY(rte_pktmbuf_alloc_bulk((rte_mempool*)(mempool), this->tx_queue_, flow_size) != 0));
  for (size_t i = 0; i < flow_size; i++) {
    AXIO_MBUF_APPEND_DATA(this->tx_queue_[this->tx_queue_index_], frame_size);
    struct eth_hdr *eth = nullptr;
    eth = AXIO_MBUF_ETH_HEADER(this->tx_queue_[this->tx_queue_index_]);
    /// set eth header
    eth->type = htons(ETHERTYPE_IP);
    rte_memcpy(eth->s_addr.bytes, this->resolve_.mac_addr_.bytes, ETH_ADDR_LEN);
    rte_memcpy(eth->d_addr.bytes, this->destination_mac_->bytes, ETH_ADDR_LEN);
    rt_assert(this->tx_queue_index_ < kNumTxRingEntries, "this->tx_queue_index_ >= kNumTxRingEntries");
    this->tx_queue_index_++;
  }
}

void DpdkDispatcher::set_tx_queue_index(size_t index) {
  this->tx_queue_index_ = index;
}

size_t DpdkDispatcher::rx_queue_index() {
  return this->rx_queue_index_;
}

void DpdkDispatcher::set_rx_queue_index(size_t index) {
  this->rx_queue_index_ = index;
}

void DpdkDispatcher::fill_rx_packets(size_t flow_size) {
  rte_mempool* mempool = this->_mempool();
  for (size_t i = 0; i < flow_size; i++) {
    rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    while (AXIO_UNLIKELY(mbuf == nullptr)) {
      mbuf = rte_pktmbuf_alloc(mempool);
    }
    udphdr uh;
    uh.source = 0;
    uh.dest = 0;
    ws_hdr hdr;
    hdr.workload_type_ = 0;
    hdr.segment_num_ = 1;
    dpdk_set_buffer_payload(mbuf, (char*)&uh, (char*)&hdr, 100);
    this->_set_packet_headers(mbuf);
    rt_assert(this->rx_queue_index_ < kNumRxRingEntries, "this->rx_queue_index_ >= kNumRxRingEntries");
    this->rx_queue_[this->rx_queue_index_] = mbuf;
    this->rx_queue_index_++;
  }
}

bool DpdkDispatcher::_is_arp_packet(rte_mbuf* buffer) {
  eth_hdr* ethernet_header = AXIO_MBUF_ETH_HEADER(buffer);
  return ntohs(ethernet_header->type) == ETH_P_ARP;
}

void DpdkDispatcher::_handle_arp_packet(rte_mbuf* buffer) {
  auto* arp_header = reinterpret_cast<arp_hdr_t*>(AXIO_MBUF_IP_HEADER(buffer));
  if (ntohs(arp_header->arp_op) == ARPOP_REQUEST) {
    if (ntohl(arp_header->arp_tpa) == ipv4_from_str(this->local_ip())) {
      this->_send_arp_reply(arp_header);
    }
  } else {
    printf("Received a non-request ARP packet\n");
  }
}

size_t DpdkDispatcher::dispatch_rx_packets() {
  /// dispatch receive_burst packets to worker rx queue; flush the rx queue
  size_t dispatch_total = 0;
  LockFreeQueue* worker_queue = nullptr;
  uint8_t workload_type = 0;
  // for (size_t i = 0; i < this->rx_queue_index_; i++) {
  //   rte_prefetch0(this->rx_queue_[i]);
  // }
  for (size_t i = 0; i < this->rx_queue_index_; i++) {
    /// resolve pkt header to get workload_type
    // rte_prefetch0(this->rx_queue_[i+1]);

    /// arp packet handler
    if (this->_is_arp_packet(this->rx_queue_[i])) {
      this->_handle_arp_packet(this->rx_queue_[i]);
      continue;
    }
    workload_type = this->_resolve_packet_header(this->rx_queue_[i]);
    /// get corresponding workspace id
    uint8_t ws_id = this->rx_rule_table_->select_next(workload_type);
    /// get workspace rx queue
    worker_queue = this->workspace_rx_queues_[ws_id];
    /// dispatch to worker rx queue
    if (AXIO_UNLIKELY(
            !worker_queue->enqueue(
                reinterpret_cast<uint8_t*>(this->rx_queue_[i])))) {
      /// drop the packet if the ws queue is full
      break;
    }
    // while(!worker_queue->enqueue((uint8_t*)this->rx_queue_[i]));
    dispatch_total++;
  }
  /// free rx-failed mbufs and reset the rx queue index
  for (size_t i = dispatch_total; i < this->rx_queue_index_; i++) {
    rte_pktmbuf_free(this->rx_queue_[i]);
  }

  this->rx_queue_index_ = 0;
  return dispatch_total;
}

void DpdkDispatcher::_send_arp_reply(arp_hdr_t* arp_header) {
  const uint8_t packet_size = sizeof(eth_hdr) + sizeof(arp_hdr_t);
  const uint32_t host_ip = ipv4_from_str(this->local_ip());

  rte_mbuf* tx_buffers[1];
  rte_mempool* mempool = this->_mempool();
  tx_buffers[0] = rte_pktmbuf_alloc(mempool);
  assert(tx_buffers[0] != nullptr);

  rte_mbuf* tx_buffer = tx_buffers[0];
  uint8_t* packet = rte_pktmbuf_mtod(tx_buffer, uint8_t*);

  eth_hdr* ethernet_header = reinterpret_cast<eth_hdr*>(packet);
  arp_hdr_t* response_header =
      reinterpret_cast<arp_hdr_t*>(packet + sizeof(eth_hdr));

  memcpy(ethernet_header->d_addr.bytes, arp_header->arp_sha, ETH_ADDR_LEN);
  memcpy(ethernet_header->s_addr.bytes, this->local_mac().bytes,
         ETH_ADDR_LEN);
  ethernet_header->type = htons(ETH_P_ARP);

  response_header->arp_hrd = htons(ARPHRD_ETHER);
  response_header->arp_pro = htons(ETH_P_IP);
  response_header->arp_hln = 6;
  response_header->arp_pln = 4;
  response_header->arp_op = htons(ARPOP_REPLY);
  memcpy(response_header->arp_sha, this->local_mac().bytes, ETH_ADDR_LEN);
  response_header->arp_spa = htonl(host_ip);
  memcpy(response_header->arp_tha, arp_header->arp_sha, ETH_ADDR_LEN);
  response_header->arp_tpa = arp_header->arp_spa;

  tx_buffer->nb_segs = 1;
  tx_buffer->pkt_len = packet_size;
  tx_buffer->data_len = packet_size;

  size_t transmitted = rte_eth_tx_burst(
      this->physical_port(), this->queue_pair_id_, tx_buffers, 1);
  if (transmitted != 1) {
    printf("Failed to send ARP response\n");
    transmitted = rte_eth_tx_burst(
        this->physical_port(), this->queue_pair_id_, tx_buffers, 1);
  }
  printf("Sent an ARP reply\n");
}

size_t DpdkDispatcher::flush_tx() {
  /// flush the tx queue
  size_t nb_tx = 0, tx_total = 0;
  rte_mbuf** tx = &this->tx_queue_[0];
  while (tx_total < this->tx_queue_index_) {
    nb_tx = rte_eth_tx_burst(this->physical_port(), this->queue_pair_id_, tx,
                             this->tx_queue_index_ - tx_total);
    tx += nb_tx;
    tx_total += nb_tx;
  }
  // rt_assert(tx_total == this->tx_queue_index_, "Failed to transmit all packets\n");
  /// free tx-failed mbufs and reset the tx queue index
  // for (size_t i = tx_total; i < this->tx_queue_index_; i++)
  //   rte_pktmbuf_free(this->tx_queue_[i]);
  this->tx_queue_index_ = 0;
  return tx_total;
}

size_t DpdkDispatcher::receive_burst() {
  size_t nb_rx = 0;
  rte_mbuf** rx = &this->rx_queue_[this->rx_queue_index_];
  // insert rx pkts to rx queue
  // nb_rx = rte_eth_rx_burst(this->physical_port(), this->queue_pair_id_, rx, kNumRxRingEntries - this->rx_queue_index_);
  nb_rx = rte_eth_rx_burst(this->physical_port(), this->queue_pair_id_, rx, this->rx_batch_size());
  this->rx_queue_index_ += nb_rx;
  return nb_rx;
}

void DpdkDispatcher::_drain_rx_queue() {
  rte_mbuf* rx_packets[this->nic_rx_post_size()];
  while (true) {
    size_t nb_rx_new =
        rte_eth_rx_burst(this->physical_port(), this->queue_pair_id_,
                         rx_packets, this->nic_rx_post_size());
    if (nb_rx_new == 0) return;
    for (size_t i = 0; i < nb_rx_new; i++) {
      rte_pktmbuf_free(rx_packets[i]);
    }
  }
}

void DpdkDispatcher::free_rx_queue() {
  for (size_t i = 0; i < this->rx_queue_index_; i++) {
    rte_pktmbuf_free(this->rx_queue_[i]);
  }
  this->rx_queue_index_ = 0;
}

}  // namespace axio
