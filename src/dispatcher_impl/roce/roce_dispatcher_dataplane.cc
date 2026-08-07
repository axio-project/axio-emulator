/**
 * @file roce_dispatcher_dataplane.cc
 * @brief Define RoCE transmit and receive functions.
 */

#include "roce_dispatcher.h"
#include "axio/datapath_batching.h"
#include "util/timer.h"

#include <type_traits>

namespace axio {

static_assert(std::is_same_v<decltype(&RoceDispatcher::receive_burst),
                             ReceiveBurstResult (RoceDispatcher::*)(bool)>);

void RoceDispatcher::_post_receives(size_t receive_count) {
  // The posted receives span first_work_request through last_work_request.
  size_t first_index = this->receive_head_index_;
  size_t last_index = first_index + (receive_count - 1);
  if (last_index >= kReceiveQueueDepth) {
    last_index -= kReceiveQueueDepth;
  }

  ibv_recv_wr* first_work_request =
      &this->receive_work_requests_[first_index];
  ibv_recv_wr* last_work_request =
      &this->receive_work_requests_[last_index];
  ibv_recv_wr* next_work_request = last_work_request->next;
  last_work_request->next = nullptr;

  ibv_recv_wr* bad_work_request;
  int result =
      ibv_post_recv(this->queue_pair_, first_work_request, &bad_work_request);
  if (AXIO_UNLIKELY(result != 0)) {
    fprintf(stderr, "Axio: Post RECV error %d\n", result);
    exit(-1);
  }

  last_work_request->next = next_work_request;

  this->receive_head_index_ = last_index;
  this->receive_head_index_ = (this->receive_head_index_ + 1) % kReceiveQueueDepth;
}

uint8_t RoceDispatcher::_resolve_packet_header(Buffer* buffer) {
  auto* workspace_header =
      reinterpret_cast<WorkspaceHeader*>(buffer->workspace_header());
  return workspace_header->workload_type_;
}

size_t RoceDispatcher::collect_tx_packets() {
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

size_t RoceDispatcher::_transmit_burst(Buffer** buffers, size_t count) {
  size_t mounted_request_count = 0;
  const size_t post_count = nic_post_count(count, this->nic_tx_post_size());
  int completion_count = ibv_poll_cq(
      this->send_completion_queue_, kSendQueueDepth, this->send_completions_);
  assert(completion_count >= 0);
  this->free_send_request_count_ += completion_count;
#if AXIO_APPLY_NEW_BUFFER || AXIO_NODE_TYPE == AXIO_CLIENT
  for (int i = 0; i < completion_count; i++) {
    this->huge_allocator_->free_buffer(
        this->send_ring_[this->send_head_index_]);
    this->send_head_index_ = (this->send_head_index_ + 1) % kSendQueueDepth;
  }
#else
  for (int i = 0; i < completion_count; i++) {
    this->send_ring_[this->send_head_index_]->state_ = Buffer::kFree;
    this->send_head_index_ = (this->send_head_index_ + 1) % kSendQueueDepth;
  }
#endif

  ibv_send_wr* first_work_request =
      &this->send_work_requests_[this->send_tail_index_];
  ibv_send_wr* last_work_request = nullptr;
  while (this->free_send_request_count_ > 0 &&
         mounted_request_count < post_count) {
    last_work_request = &this->send_work_requests_[this->send_tail_index_];
    ibv_sge* scatter_gather =
        &this->send_scatter_gather_[this->send_tail_index_];
    Buffer* buffer = buffers[mounted_request_count];
    buffer->state_ = Buffer::kPosted;
    scatter_gather->addr = reinterpret_cast<uint64_t>(buffer->data());
    scatter_gather->length = buffer->length_;
    scatter_gather->lkey = buffer->lkey_;
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    last_work_request->wr.ud.ah = this->remote_address_handle_;
    last_work_request->wr.ud.remote_qpn = this->remote_queue_pair_id_;
#endif

    this->send_ring_[this->send_tail_index_] = buffer;

    this->send_tail_index_ = (this->send_tail_index_ + 1) % kSendQueueDepth;
    this->free_send_request_count_--;
    mounted_request_count++;
  }

  if (mounted_request_count > 0) {
    ibv_send_wr* bad_work_request;
    ibv_send_wr* next_work_request = last_work_request->next;
    last_work_request->next = nullptr;
    int result = ibv_post_send(this->queue_pair_, first_work_request,
                               &bad_work_request);
    if (AXIO_UNLIKELY(result != 0)) {
      fprintf(stderr,
              "Axio: Fatal error. ibv_post_send failed. result = %d\n",
              result);
      assert(result == 0);
      exit(-1);
    }
    last_work_request->next = next_work_request;
  }
  return mounted_request_count;
}

size_t RoceDispatcher::flush_tx() {
  size_t transmitted_count = 0;
  Buffer** next_buffer = &this->tx_queue_[0];
  while (transmitted_count < this->tx_queue_index_) {
    size_t burst_count = this->_transmit_burst(
        next_buffer, this->tx_queue_index_ - transmitted_count);
    next_buffer += burst_count;
    transmitted_count += burst_count;
  }
  this->tx_queue_index_ = 0;
  return transmitted_count;
}

ReceiveBurstResult RoceDispatcher::receive_burst(
    bool capture_completion_timestamp) {
  Buffer* ring_entry = this->receive_ring_[this->receive_head_index_];
  size_t receive_count = 0;

  while (ring_entry->state_ == Buffer::kFree &&
         receive_count < this->nic_rx_post_size()) {
    receive_count++;
    ring_entry->state_ = Buffer::kPosted;
    ring_entry = ring_entry->next_;
  }
  if (receive_count != 0) {
    this->_post_receives(receive_count);
  }

  const size_t completion_limit = nic_post_count(
      kReceiveQueueDepth - this->pending_dispatch_count_,
      this->nic_rx_post_size());
  if (completion_limit == 0) return {};
  int completion_count =
      ibv_poll_cq(this->receive_completion_queue_,
                  static_cast<int>(completion_limit),
                  this->receive_completions_);
  if (AXIO_UNLIKELY(completion_count < 0)) {
    return {0, 1};
  }
  OrderedTscSample completion_timestamp;
  if (completion_count != 0 && capture_completion_timestamp) {
    completion_timestamp = read_ordered_tsc();
  }
  size_t completion_error_count = 0;
  for (int i = 0; i < completion_count; i++) {
    if (AXIO_UNLIKELY(this->receive_completions_[i].status !=
                      IBV_WC_SUCCESS)) {
      completion_error_count++;
    }
  }
  if (AXIO_UNLIKELY(completion_error_count != 0)) {
    return {0, completion_error_count, completion_timestamp.cycles,
            completion_timestamp.cpu_id};
  }
  for (int i = 0; i < completion_count; i++) {
    size_t receive_index =
        (this->receive_ring_head_ + this->pending_dispatch_count_ + i) %
        kReceiveQueueDepth;
    this->receive_ring_[receive_index]->length_ =
        this->receive_completions_[i].byte_len;
  }
  this->pending_dispatch_count_ += completion_count;
  return {static_cast<size_t>(completion_count), 0,
          completion_timestamp.cycles, completion_timestamp.cpu_id};
}

size_t RoceDispatcher::dispatch_rx_packets() {
  size_t dispatched_count = 0;
  LockFreeQueue* workspace_queue = nullptr;
  Buffer* ring_entry = this->receive_ring_[this->receive_ring_head_];
  for (size_t i = 0; i < this->pending_dispatch_count_; i++) {
    uint8_t workload_type = this->_resolve_packet_header(ring_entry);
    uint8_t workspace_id = this->rx_rule_table_->select_next(workload_type);
    workspace_queue = this->workspace_rx_queues_[workspace_id];
    if (AXIO_UNLIKELY(!workspace_queue->enqueue(
            reinterpret_cast<uint8_t*>(ring_entry)))) {
      ring_entry->state_ = Buffer::kFree;
      ring_entry = ring_entry->next_;
      continue;
    }
    ring_entry->state_ = Buffer::kApplicationOwned;
    ring_entry = ring_entry->next_;
    dispatched_count++;
  }
  this->receive_ring_head_ =
      (this->receive_ring_head_ + this->pending_dispatch_count_) %
      kReceiveQueueDepth;
  this->pending_dispatch_count_ = 0;
  return dispatched_count;
}

}  // namespace axio
