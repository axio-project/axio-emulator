#pragma once
#include "common.h"

namespace axio {
/**
 * @brief A lock-free queue for storing Application-generated packets. 
 * For TX, application is producer, and dispatcher is consumer. Application 
 * can only operate on the tail of the queue, and dispatcher can only operate
 * on the head of the queue. 
 * For RX, dispatcher is producer, and application is consumer. Similarly, 
 * dispatcher can only operate on the tail of the queue, and application can
 * only operate on the head of the queue.
*/

class LockFreeQueue {
  static_assert(kWsQueueSize > 1 && (kWsQueueSize & (kWsQueueSize - 1)) == 0,
                "kWsQueueSize must be a power of two");

 public:
  LockFreeQueue() { memset(this->queue_, 0, sizeof(this->queue_)); }

  inline bool enqueue(uint8_t *packet) {
    const size_t next_tail = (this->tail_ + 1) & this->mask_;
    if (next_tail == this->head_) return false;
    this->queue_[this->tail_] = packet;
    this->tail_ = next_tail;
    return true;
  }

  inline uint8_t* dequeue() {
    if (this->head_ == this->tail_) return nullptr;
    uint8_t* packet = this->queue_[this->head_];
    this->head_ = (this->head_ + 1) & this->mask_;
    return packet;
  }

  inline void reset_head() { this->head_ = 0; }
  inline void reset_tail() { this->tail_ = 0; }
  inline size_t size() const { return (this->tail_ - this->head_) & this->mask_; }

 private:
  uint8_t* queue_[kWsQueueSize];
  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;
  const size_t mask_ = kWsQueueSize - 1;
};
}  // namespace axio
