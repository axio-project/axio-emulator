/**
 * @file roce_dispatcher_dataplane.cc
 * @brief Dispatcher-owned rings and link-owned RC credits.
 */
#include "roce_dispatcher.h"
#include "axio/datapath_batching.h"
#include "util/timer.h"
#include <algorithm>

namespace axio {

void RoceDispatcher::_post_receives(Link& link, size_t count) {
  ibv_recv_wr* first = &this->receive_work_requests_[link.rx_cursor];
  ibv_recv_wr* last = first;
  for (size_t i = 1; i < count; ++i) last = last->next;
  auto* next = last->next;
  last->next = nullptr;
  ibv_recv_wr* bad = nullptr;
  const int result = ibv_post_recv(link.qp, first, &bad);
  last->next = next;
  if (result) throw std::runtime_error("RoCE receive posting failed");
  link.rx_cursor = next->wr_id;
}

uint8_t RoceDispatcher::_resolve_packet_header(Buffer* buffer) {
  return reinterpret_cast<WorkspaceHeader*>(buffer->workspace_header())->workload_type_;
}

uint16_t RoceDispatcher::_select_link(Buffer* buffer) {
#if AXIO_NODE_TYPE == AXIO_SERVER
  return buffer->ingress_link_;
#else
  if (this->links_.size() == 1) return 0;
  const uint8_t workload = this->_resolve_packet_header(buffer);
  uint32_t total = 0;
  for (const auto& link : this->links_)
    if (link.route.workload == workload) total += link.route.weight;
  if (total == 0) throw std::runtime_error("request workload has no RC link");
  uint32_t position = this->route_cursor_[workload]++ % total;
  for (size_t i = 0; i < this->links_.size(); ++i) {
    const auto& link = this->links_[i];
    if (link.route.workload != workload) continue;
    if (position < link.route.weight) return i;
    position -= link.route.weight;
  }
  throw std::runtime_error("invalid RC route weight");
#endif
}

size_t RoceDispatcher::collect_tx_packets() {
  this->_reap_send_completions();
  size_t remaining_ring_size = kNumTxRingEntries - this->tx_queue_index_;
  size_t collected_queue_count = 0;
  size_t collected_packet_count = 0;
  while (remaining_ring_size != 0 &&
         collected_queue_count < this->workspace_tx_queues_.size()) {
    LockFreeQueue* workspace_queue =
        this->workspace_tx_queues_[this->workspace_queue_index_];
    const size_t pending_count = workspace_queue->size();
    if (!dispatcher_batch_ready(pending_count, this->tx_batch_size())) {
      this->workspace_queue_index_ =
          (this->workspace_queue_index_ + 1) %
          this->workspace_tx_queues_.size();
      collected_queue_count++;
      continue;
    }
    const size_t transmit_count =
        (pending_count > remaining_ring_size) ? remaining_ring_size
                                              : pending_count;
    for (size_t i = 0; i < transmit_count; i++) {
      this->tx_queue_[this->tx_queue_index_] =
          reinterpret_cast<Buffer*>(workspace_queue->dequeue());
      Buffer* buffer = this->tx_queue_[this->tx_queue_index_];
      buffer->ingress_link_ = this->_select_link(buffer);
      this->tx_queue_index_++;
    }
    this->workspace_queue_index_ =
        (this->workspace_queue_index_ + 1) %
        this->workspace_tx_queues_.size();
    collected_queue_count++;
    remaining_ring_size -= transmit_count;
    collected_packet_count += transmit_count;
  }
  return collected_packet_count;
}



size_t RoceDispatcher::_reap_send_completions() {
  const int count = ibv_poll_cq(this->send_completion_queue_, kSendQueueDepth,
                                this->send_completions_);
  if (count < 0) throw std::runtime_error("RoCE send CQ polling failed");
  Buffer* completed[kSendQueueDepth];
  for (int i = 0; i < count; ++i) {
    const auto& wc = this->send_completions_[i];
    if (AXIO_UNLIKELY(wc.status != IBV_WC_SUCCESS || wc.wr_id >= kSendQueueDepth))
      throw std::runtime_error("RoCE send completion failed");
    const size_t slot = wc.wr_id;
    Buffer* buffer = this->send_ring_[slot];
    if (AXIO_UNLIKELY(buffer == nullptr)) throw std::runtime_error("duplicate send completion");
    completed[i] = buffer;
    this->send_ring_[slot] = nullptr;
    this->links_[this->send_slot_link_[slot]].free_tx_slots.push_back(slot);
  }
#if AXIO_APPLY_NEW_BUFFER || AXIO_NODE_TYPE == AXIO_CLIENT
  this->buffer_pool_->free_bulk(completed, static_cast<size_t>(count));
#else
  for (int i = 0; i < count; ++i) completed[i]->mark_free();
#endif
  return count;
}

void RoceDispatcher::_post_send_chain(Link& link) {
  if (link.send_first == nullptr) return;
  link.send_last->next = nullptr;
  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(link.qp, link.send_first, &bad))
    throw std::runtime_error("RoCE send posting failed");
  link.send_first = link.send_last = nullptr;
  link.send_count = 0;
}

size_t RoceDispatcher::flush_tx() {
  this->_reap_send_completions();
  size_t pending = 0;
  // Each buffered packet is considered once. A credit-starved link cannot
  // prevent another link from posting or the next RX/repost phase from running.
  for (size_t i = 0; i < this->tx_queue_index_; ++i) {
    Buffer* buffer = this->tx_queue_[i];
    const size_t link_id = buffer->ingress_link_;
    if (AXIO_UNLIKELY(link_id >= this->links_.size())) throw std::runtime_error("invalid buffer RC link");
    auto& link = this->links_[link_id];
    if (link.free_tx_slots.empty()) {
      this->tx_queue_[pending++] = buffer;
      continue;
    }
    const size_t slot = link.free_tx_slots.back();
    link.free_tx_slots.pop_back();
    auto& wr = this->send_work_requests_[slot];
    auto& sge = this->send_scatter_gather_[slot];
#if !AXIO_APPLY_NEW_BUFFER && AXIO_NODE_TYPE == AXIO_SERVER
    buffer->mark_posted();
#endif
    sge.addr = reinterpret_cast<uint64_t>(buffer->data());
    sge.length = buffer->length_;
    sge.lkey = buffer->lkey_;
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    wr.wr.ud.ah = this->remote_address_handle_;
    wr.wr.ud.remote_qpn = this->remote_queue_pair_id_;
#endif
    this->send_ring_[slot] = buffer;
    if (link.send_last) link.send_last->next = &wr;
    else link.send_first = &wr;
    link.send_last = &wr;
    if (++link.send_count == this->nic_tx_post_size()) this->_post_send_chain(link);
  }
  for (auto& link : this->links_) this->_post_send_chain(link);
  const size_t sent = this->tx_queue_index_ - pending;
  this->tx_queue_index_ = pending;
  return sent;
}

ReceiveBurstResult RoceDispatcher::receive_burst(bool capture_completion_timestamp) {
  for (auto& link : this->links_) {
    size_t count = 0;
    Buffer* buffer = this->receive_ring_[link.rx_cursor];
    const size_t limit = std::min<size_t>(link.rx_count, this->nic_rx_post_size());
    while (count < limit && buffer->state() == Buffer::kFree) {
      buffer->mark_posted();
      ++count;
      buffer = buffer->next_;
    }
    if (count) this->_post_receives(link, count);
  }
  const size_t limit = nic_post_count(kReceiveQueueDepth - this->pending_dispatch_count_,
                                      this->nic_rx_post_size());
  if (limit == 0) return {};
  const int count = ibv_poll_cq(this->receive_completion_queue_, limit, this->receive_completions_);
  if (AXIO_UNLIKELY(count < 0)) return {0, 1};
  OrderedTscSample timestamp;
  if (count && capture_completion_timestamp) timestamp = read_ordered_tsc();
  size_t errors = 0, successful = 0;
  for (int i = 0; i < count; ++i) {
    const auto& wc = this->receive_completions_[i];
    if (AXIO_UNLIKELY(wc.status != IBV_WC_SUCCESS || wc.wr_id >= kReceiveQueueDepth)) {
      ++errors;
      continue;
    }
    Buffer* buffer = this->receive_ring_[wc.wr_id];
    buffer->length_ = wc.byte_len;
    buffer->ingress_link_ = this->receive_slot_link_[wc.wr_id];
    buffer->origin_application_ = reinterpret_cast<udphdr*>(buffer->udp_header())->source;
    this->pending_receive_[this->pending_dispatch_count_++] = buffer;
    ++successful;
  }
  return {successful, errors, timestamp.cycles, timestamp.cpu_id};
}

size_t RoceDispatcher::dispatch_rx_packets() {
  size_t dispatched = 0;
  for (size_t i = 0; i < this->pending_dispatch_count_; ++i) {
    Buffer* buffer = this->pending_receive_[i];
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC && AXIO_NODE_TYPE == AXIO_CLIENT
    const uint16_t application = reinterpret_cast<udphdr*>(buffer->udp_header())->dest;
#else
    const uint16_t application = this->rx_rule_table_->select_next(this->_resolve_packet_header(buffer));
#endif
    if (AXIO_UNLIKELY(application >= kWorkspaceMaxNum || this->workspace_rx_queues_[application] == nullptr))
      throw std::runtime_error("response belongs to a non-local application");
    buffer->mark_application_owned();
    if (this->workspace_rx_queues_[application]->enqueue(reinterpret_cast<uint8_t*>(buffer)))
      ++dispatched;
    else buffer->mark_free();
  }
  this->pending_dispatch_count_ = 0;
  return dispatched;
}

}  // namespace axio
