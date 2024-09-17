/**
 * @file dpdk_dispatcher_dataplane.cc
 * @brief Define Transmit / Receive functions of DPDK
 */
#include "dpdk_dispatcher.h"
namespace dperf{

/// Generate a IP+UDP packet
void DpdkDispatcher::set_pkt_hdr(rte_mbuf *m) {
  struct eth_hdr *eth = NULL;
  struct iphdr *iph = NULL;
  struct udphdr *uh = NULL;
  eth = mbuf_eth_hdr(m);
  iph = mbuf_ip_hdr(m);
  uh = mbuf_udp_hdr(m);

  /// set eth header
  eth->type = htons(ETHERTYPE_IP);
  rte_memcpy(eth->s_addr.bytes, resolve_.mac_addr_.bytes, ETH_ADDR_LEN);
  rte_memcpy(eth->d_addr.bytes, dmac_->bytes, ETH_ADDR_LEN);

  /// set ip header
  iph->saddr = resolve_.ipv4_addr_.ip;
  iph->daddr = daddr_->ip;
  iph->ihl = 5;   // header len: 20 bytes
  iph->version = 4;
  iph->tos = 0;
  iph->tot_len = rte_cpu_to_be_16(m->pkt_len - sizeof(struct eth_hdr));
  iph->ttl = 64;
  iph->frag_off = IP_FLAG_DF;   // Don't fragment
  iph->protocol = IPPROTO_UDP;  // UDP
  m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
  m->l2_len = sizeof(struct eth_hdr);
  m->l3_len = sizeof(struct iphdr);

  /// set udp header completely
  uh->source += kDefaultUdpPort;
  uh->source = rte_cpu_to_be_16(uh->source);
  uh->dest += kDefaultUdpPort;
  uh->dest = rte_cpu_to_be_16(uh->dest);
  uh->len = rte_cpu_to_be_16(m->pkt_len - sizeof(struct eth_hdr) - sizeof(struct iphdr));
}

uint8_t DpdkDispatcher::resolve_pkt_hdr(rte_mbuf *m) {
  struct ws_hdr *wh = NULL;
  wh = mbuf_ws_hdr(m);
  return wh->workload_type_;
}

/// Collect mbufs from all workers' tx queues with Round-Robin mode
size_t DpdkDispatcher::collect_tx_pkts() {
  size_t remain_ring_size = kNumTxRingEntries - tx_queue_idx_;
  uint8_t nb_collect_queue = 0;
  size_t nb_collect_num = 0;
  while (remain_ring_size && nb_collect_queue < ws_tx_queues_.size()) {
    /// select a workspace tx queue
    lock_free_queue *worker_queue = ws_tx_queues_[ws_queue_idx_];
    size_t tx_size = worker_queue->get_size();
    if (tx_size < kDispTxBatchSize) {
      ws_queue_idx_ = (ws_queue_idx_ + 1) % ws_tx_queues_.size();
      nb_collect_queue++;
      continue;
    }
    tx_size = (tx_size > remain_ring_size) ? remain_ring_size : tx_size;
    // printf("%lu, %lu\n", worker_queue->head_, worker_queue->tail_);
    for (size_t i = 0; i < tx_size; i++) {
      tx_queue_[tx_queue_idx_] = (rte_mbuf*)worker_queue->dequeue();
      set_pkt_hdr(tx_queue_[tx_queue_idx_]);
      tx_queue_idx_++;
    }
    ws_queue_idx_ = (ws_queue_idx_ + 1) % ws_tx_queues_.size();
    nb_collect_queue++;
    remain_ring_size -= tx_size;
    nb_collect_num += tx_size;
  }
  return nb_collect_num;
}

extern void dpdk_set_mbuf_paylod(rte_mbuf *mbuf, char* uh, char* ws_header, size_t payload_size);

void DpdkDispatcher::fill_tx_pkts(size_t flow_size, size_t frame_size) {
  rte_mempool* mempool = get_mempool();
  while(unlikely(rte_pktmbuf_alloc_bulk((rte_mempool*)(mempool), tx_queue_, flow_size) != 0));
  for (size_t i = 0; i < flow_size; i++) {
    mbuf_push_data(tx_queue_[tx_queue_idx_], frame_size);
    struct eth_hdr *eth = NULL;
    eth = mbuf_eth_hdr(tx_queue_[tx_queue_idx_]);
    /// set eth header
    eth->type = htons(ETHERTYPE_IP);
    rte_memcpy(eth->s_addr.bytes, resolve_.mac_addr_.bytes, ETH_ADDR_LEN);
    rte_memcpy(eth->d_addr.bytes, dmac_->bytes, ETH_ADDR_LEN);
    rt_assert(tx_queue_idx_ < kNumTxRingEntries, "tx_queue_idx_ >= kNumTxRingEntries");
    tx_queue_idx_++;
  }
}

void DpdkDispatcher::set_tx_queue_index(size_t index) {
  tx_queue_idx_ = index;
}

size_t DpdkDispatcher::get_rx_queue_index() {
  return rx_queue_idx_;
}

void DpdkDispatcher::set_rx_queue_index(size_t index) {
  rx_queue_idx_ = index;
}

void DpdkDispatcher::fill_rx_pkts(size_t flow_size) {
  rte_mempool* mempool = get_mempool();
  for (size_t i = 0; i < flow_size; i++) {
    rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    while (unlikely(mbuf == NULL)) {
      mbuf = rte_pktmbuf_alloc(mempool);
    }
    udphdr uh;
    uh.source = 0;
    uh.dest = 0;
    ws_hdr hdr;
    hdr.workload_type_ = 0;
    hdr.segment_num_ = 1;
    dpdk_set_mbuf_paylod(mbuf, (char*)&uh, (char*)&hdr, 100);
    set_pkt_hdr(mbuf);
    rt_assert(rx_queue_idx_ < kNumRxRingEntries, "rx_queue_idx_ >= kNumRxRingEntries");
    rx_queue_[rx_queue_idx_] = mbuf;
    rx_queue_idx_++;
  }
}

bool DpdkDispatcher::is_arp_packet(rte_mbuf *m) {
  struct eth_hdr* ethhdr = mbuf_eth_hdr(m);
  if(ntohs(ethhdr->type) == ETH_P_ARP) return true;
  return false;
}

void DpdkDispatcher::handle_arp_packet(rte_mbuf *m) {
  arp_hdr_t *arph = (arp_hdr_t*)mbuf_ip_hdr(m);
  if (ntohs(arph->arp_op)  == ARPOP_REQUEST) {
    if (ntohl(arph->arp_tpa) == ipv4_from_str(kLocalIpStr)) {
      tx_burst_for_arp(arph);
    }
  }else{
    printf("received an arp packet(non request)\n");
  }
}

size_t DpdkDispatcher::dispatch_rx_pkts() {
  /// dispatch rx_burst packets to worker rx queue; flush the rx queue
  size_t dispatch_total = 0;
  lock_free_queue *worker_queue = nullptr;
  uint8_t worload_type = 0;
  // for (size_t i = 0; i < rx_queue_idx_; i++) {
  //   rte_prefetch0(rx_queue_[i]);
  // }
  for (size_t i = 0; i < rx_queue_idx_; i++) {
    /// resolve pkt header to get workload_type
    // rte_prefetch0(rx_queue_[i+1]);

    /// arp packet handler
    if (is_arp_packet(rx_queue_[i])) {
      handle_arp_packet(rx_queue_[i]);
      continue;
    }
    worload_type = resolve_pkt_hdr(rx_queue_[i]);
    /// get corresponding workspace id
    uint8_t ws_id = rx_rule_table_->rr_select(worload_type);
    /// get workspace rx queue
    worker_queue = ws_rx_queues_[ws_id];
    /// dispatch to worker rx queue
    if (unlikely(!worker_queue->enqueue((uint8_t*)rx_queue_[i]))) {
      /// drop the packet if the ws queue is full
      break;
    }
    // while(!worker_queue->enqueue((uint8_t*)rx_queue_[i]));
    dispatch_total++;
  }
  /// free rx-failed mbufs and reset the rx queue index
  for (size_t i = dispatch_total; i < rx_queue_idx_; i++)
    rte_pktmbuf_free(rx_queue_[i]);
  
  rx_queue_idx_ = 0;
  return dispatch_total;
}

void DpdkDispatcher::tx_burst_for_arp(arp_hdr_t* arp_hdr){
  uint8_t pkt_size = sizeof(eth_hdr)+sizeof(arp_hdr_t);
  uint32_t host_ip = ipv4_from_str(kLocalIpStr);

  rte_mbuf *tx_mbufs[1];
  rte_mempool* mempool = get_mempool();
  tx_mbufs[0] = rte_pktmbuf_alloc(mempool);
  assert(tx_mbufs[0] != nullptr);

  rte_mbuf * tx_mbuf = tx_mbufs[0];
  uint8_t* packet = rte_pktmbuf_mtod(tx_mbuf, uint8_t *);

  eth_hdr *eh = reinterpret_cast<eth_hdr *> (packet);
  arp_hdr_t *arph = reinterpret_cast<arp_hdr_t*> (packet+sizeof(eth_hdr));

  //set eth header
	memcpy(eh->d_addr.bytes, arp_hdr->arp_sha, ETH_ADDR_LEN);
	memcpy(eh->s_addr.bytes, kLocalMac.bytes, ETH_ADDR_LEN);
	eh->type = htons(ETH_P_ARP);

  //set arp header
  arph->arp_hrd = htons(ARPHRD_ETHER);
	arph->arp_pro = htons(ETH_P_IP);
	arph->arp_hln = 6;
	arph->arp_pln = 4;
	arph->arp_op =  htons(ARPOP_REPLY);
	memcpy(arph->arp_sha, kLocalMac.bytes, ETH_ADDR_LEN);
	arph->arp_spa = htonl(host_ip);
	memcpy(arph->arp_tha, arp_hdr->arp_sha, ETH_ADDR_LEN);
	arph->arp_tpa = arp_hdr->arp_spa;

  //set tx_mbuf
  tx_mbufs[0]->nb_segs = 1;
  tx_mbufs[0]->pkt_len = pkt_size;
  tx_mbufs[0]->data_len = pkt_size;

  size_t nb_tx_new = rte_eth_tx_burst(phy_port_, qp_id_, tx_mbufs, 1);
  if (nb_tx_new != 1){
    printf("failed to send arp reponse\n");
    nb_tx_new = rte_eth_tx_burst(phy_port_, qp_id_, tx_mbufs, 1);
  }
  printf("send a arp reply!\n");
}

size_t DpdkDispatcher::tx_flush(){
  /// flush the tx queue
  size_t nb_tx = 0, tx_total = 0;
  rte_mbuf **tx = &tx_queue_[0];
  while(tx_total < tx_queue_idx_) {
    nb_tx = rte_eth_tx_burst(phy_port_, qp_id_, tx, tx_queue_idx_ - tx_total);
    tx += nb_tx;
    tx_total += nb_tx;
  }
  // rt_assert(tx_total == tx_queue_idx_, "Failed to transmit all packets\n");
  /// free tx-failed mbufs and reset the tx queue index
  // for (size_t i = tx_total; i < tx_queue_idx_; i++)
  //   rte_pktmbuf_free(tx_queue_[i]);
  tx_queue_idx_ = 0;
  return tx_total;
}

size_t DpdkDispatcher::rx_burst(){
  size_t nb_rx = 0;
  rte_mbuf **rx = &rx_queue_[rx_queue_idx_];
  // insert rx pkts to rx queue
  // nb_rx = rte_eth_rx_burst(phy_port_, qp_id_, rx, kNumRxRingEntries - rx_queue_idx_);
  nb_rx = rte_eth_rx_burst(phy_port_, qp_id_, rx, kNICRxPostSize);
  rx_queue_idx_ += nb_rx;
  return nb_rx;
}

void DpdkDispatcher::drain_rx_queue(){
  struct rte_mbuf *rx_pkts[kNICRxPostSize];
  while (true) {
    size_t nb_rx_new =
        rte_eth_rx_burst(phy_port_, qp_id_, rx_pkts, kNICRxPostSize);
    if (nb_rx_new == 0) return;
    for (size_t i = 0; i < nb_rx_new; i++) rte_pktmbuf_free(rx_pkts[i]);
  }
}

void DpdkDispatcher::free_rx_queue() {
  for (size_t i = 0; i < rx_queue_idx_; i++) {
    rte_pktmbuf_free(rx_queue_[i]);
  }
  rx_queue_idx_ = 0;
}

} // namespace dperf
