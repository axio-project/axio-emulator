/**
 * @file roce_dispatcher_dataplane.cc
 * @brief Define Transmit / Receive functions of RoCE
 */

#include "roce_dispatcher.h"

namespace dperf {

void RoceDispatcher::post_recvs(size_t num_recvs) {

  // The recvs posted are @first_wr through @last_wr, inclusive
  struct ibv_recv_wr *first_wr, *last_wr, *temp_wr, *bad_wr;

  int ret;
  size_t first_wr_i = recv_head_;
  size_t last_wr_i = first_wr_i + (num_recvs - 1);
  if (last_wr_i >= kRQDepth) last_wr_i -= kRQDepth;

  first_wr = &recv_wr[first_wr_i];
  last_wr = &recv_wr[last_wr_i];
  temp_wr = last_wr->next;

  last_wr->next = nullptr;  // Breaker of chains, queen of the First Men

  ret = ibv_post_recv(qp_, first_wr, &bad_wr);
  if (unlikely(ret != 0)) {
    fprintf(stderr, "eRPC IBTransport: Post RECV (normal) error %d\n", ret);
    exit(-1);
  }

  last_wr->next = temp_wr;  // Restore circularity

  // Update RECV head: go to the last wr posted and take 1 more step
  recv_head_ = last_wr_i;
  recv_head_ = (recv_head_ + 1) % kRQDepth;
}

uint8_t RoceDispatcher::resolve_pkt_hdr(Buffer *m) {
  struct ws_hdr *wh = NULL;
  // struct iphdr *iph = NULL;
  // struct udphdr *uh = NULL;
  // iph = reinterpret_cast<iphdr *>(m->get_iph());
  wh = reinterpret_cast<ws_hdr *>(m->get_ws_hdr());
  // uh = reinterpret_cast<udphdr *>(m->get_uh());
  // for (int i = 0; i < 40; i++) {
  //   printf("%x ", m->get_buf()[i]);
  // }
  // printf("\n");
  // printf("src port: %u, dst port: %u, buffer len: %u\n", uh->source, uh->dest,ntohs(iph->tot_len));
  return wh->workload_type_;
}

size_t RoceDispatcher::collect_tx_pkts() {
  size_t remain_ring_size = kNumTxRingEntries - tx_queue_idx_;
  uint8_t nb_collect_queue = 0;
  size_t nb_collect_num = 0;
  while (remain_ring_size && nb_collect_queue < ws_tx_queues_.size()) {
    /// select a workspace tx queue
    lock_free_queue *worker_queue = ws_tx_queues_[ws_queue_idx_];
    size_t tx_size = (worker_queue->get_size() > remain_ring_size) 
                          ? remain_ring_size : worker_queue->get_size();
    // printf("%lu, %lu\n", worker_queue->head_, worker_queue->tail_);
    for (size_t i = 0; i < tx_size; i++) {
      tx_queue_[tx_queue_idx_] = (Buffer*)worker_queue->dequeue();
      tx_queue_idx_++;
    }
    ws_queue_idx_ = (ws_queue_idx_ + 1) % ws_tx_queues_.size();
    nb_collect_queue++;
    remain_ring_size -= tx_size;
    nb_collect_num += tx_size;
  }
  return nb_collect_num;
}

size_t RoceDispatcher::tx_burst(Buffer **tx, size_t nb_tx) {
  // Mount buffers to send wr, generate corresponding sge
  size_t nb_tx_res = 0;   // total number of mounted wr for this burst tx
  /// post send cq first
  int ret = ibv_poll_cq(send_cq_, kPostlist, send_wc);
  assert(ret >= 0);
  free_send_wr_num_ += ret;
  for (int i = 0; i < ret; i++) {
    huge_alloc_->free_buf(sw_ring_[send_head_]);
    send_head_ = (send_head_ + 1) % kSQDepth;
  }
  /// post send wr
  struct ibv_send_wr* first_wr = &send_wr[send_tail_];
  struct ibv_send_wr* tail_wr = nullptr;
  while (free_send_wr_num_ > 0 && nb_tx_res < nb_tx) {
    tail_wr = &send_wr[send_tail_];
    struct ibv_sge* sgl = &send_sgl[send_tail_];
    Buffer *m = tx[nb_tx_res];
    m->state_ = Buffer::kPOSTED;
    sgl->addr = reinterpret_cast<uint64_t>(m->get_buf());
    sgl->length = m->length_;
    sgl->lkey = m->lkey_;
    /// TODO: set remote ah for wr
    // tail_wr->wr.ud.ah = self_ah_;
    // tail_wr->wr.ud.remote_qpn = qp_->qp_num;
    /// mount buffer to sw_ring
    sw_ring_[send_tail_] = m;

    send_tail_ = (send_tail_ + 1) % kSQDepth;
    free_send_wr_num_--;
    nb_tx_res++;
  }

  if (nb_tx_res > 0) {
    struct ibv_send_wr* bad_send_wr;
    struct ibv_send_wr* temp_wr = tail_wr->next;
    tail_wr->next = nullptr; // Breaker of chains
    ret = ibv_post_send(qp_, first_wr, &bad_send_wr);
    if (unlikely(ret != 0)) {
      fprintf(stderr, "dPerf: Fatal error. ibv_post_send failed. ret = %d\n", ret);
      assert(ret == 0);
      exit(-1);
    }
    tail_wr->next = temp_wr;  // Restore circularity
  }
  return nb_tx_res;
}

size_t RoceDispatcher::tx_flush() {
  size_t nb_tx = 0, tx_total = 0;
  Buffer **tx = &tx_queue_[0];
  while(tx_total < tx_queue_idx_) {
    nb_tx = tx_burst(tx, tx_queue_idx_ - tx_total);
    tx += nb_tx;
    tx_total += nb_tx;
  }
  tx_queue_idx_ = 0;
  return tx_total;
}

size_t RoceDispatcher::rx_burst() {
  /// post recvs first
  Buffer *ring_entry = rx_ring_[recv_head_];  // the first unpost recv buffer (owned by app)
  size_t num_recvs = 0;
  while (ring_entry->state_ == Buffer::kFREE_BUF) {    // if the buffer is freed by app, post it
    num_recvs++;
    ring_entry->state_ = Buffer::kPOSTED;
    ring_entry = ring_entry->next_;
  }
  if (num_recvs){
    post_recvs(num_recvs);  // post recvs
  }

  /// poll cq
  int ret = ibv_poll_cq(recv_cq_, kPostlist, recv_wc);
  assert(ret >= 0);
  if (ret > 0) {
    for (int i = 0; i < ret; i++)
      printf("ibv status: %u, opcode: %u, wr_id: %lu, imm_flag: %u, recv buf size: %u\n", recv_wc[i].status, recv_wc[i].opcode, recv_wc[i].wr_id, recv_wc[i].wc_flags & IBV_WC_WITH_IMM, recv_wc[i].byte_len);
  }
  wait_for_disp_ += ret;
  return static_cast<size_t>(ret);
}

template<dispatcher_handler_type_t handler>
size_t RoceDispatcher::pre_dispatch_pkts(){
  /// pre_dispatch_pkts is not enabled for RDMA dispatcher
  return 0;
}
// force compile
template size_t RoceDispatcher::pre_dispatch_pkts<kRxDispatcherHandler>();


size_t RoceDispatcher::dispatch_rx_pkts() {
  /// dispatch rx_burst packets to worker rx queue; flush the rx queue
  size_t dispatch_total = 0;
  lock_free_queue *worker_queue = nullptr;
  uint8_t worload_type = 0;
  Buffer *ring_entry = rx_ring_[ring_head_];    // the first un-dispatched buffer
  for (size_t i = 0; i < wait_for_disp_; i++) {
    /// resolve pkt header to get workload_type
    worload_type = resolve_pkt_hdr(ring_entry);
    /// get corresponding workspace id
    uint8_t ws_id = rx_rule_table_->rr_select(worload_type);
    /// get workspace rx queue
    worker_queue = ws_rx_queues_[ws_id];
    /// dispatch to worker rx queue
    if (unlikely(!worker_queue->enqueue((uint8_t*)ring_entry->get_buf()))) {
      /// drop the packet if the ws queue is full
      ring_entry->state_ = Buffer::kFREE_BUF;
      ring_entry = ring_entry->next_;
      continue;
    }
    ring_entry->state_ = Buffer::kAPP_OWNED_BUF;
    ring_entry = ring_entry->next_;
    dispatch_total++;
  }
  /// update ring_head_
  ring_head_ = (ring_head_ + wait_for_disp_) % kRQDepth;
  wait_for_disp_ = 0;
  return dispatch_total;
}

}