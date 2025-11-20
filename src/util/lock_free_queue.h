#pragma once
#include "common.h"


namespace dperf {
/**
 * @brief A lock-free queue for storing Application-generated packets. 
 * For TX, application is producer, and dispatcher is consumer. Application 
 * can only operate on the tail of the queue, and dispatcher can only operate
 * on the head of the queue. 
 * For RX, dispatcher is producer, and application is consumer. Similarly, 
 * dispatcher can only operate on the tail of the queue, and application can
 * only operate on the head of the queue.
 * 
 * NOTE: Memory barriers are critical for Arm architecture to ensure cache
 * coherency between producer and consumer threads on different cores.
*/

struct lock_free_queue {
    uint8_t* queue_[kWsQueueSize];
    volatile size_t head_ = 0;
    volatile size_t tail_ = 0;
    const size_t mask_ = kWsQueueSize - 1;  // Assuming kWsQueueSize is a power of 2
    public:
    lock_free_queue() {
        rt_assert(is_power_of_two<size_t>(kWsQueueSize), "The size of Ws Queue is not power of two.");
        memset(queue_, 0, sizeof(queue_));
    }
    inline bool enqueue(uint8_t *pkt) {
        size_t next_tail = (tail_ + 1) & mask_;
        size_t current_head = head_;  // Read head once with acquire semantics
        
        // Memory barrier: ensure head_ is read before checking queue full
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        
        if (next_tail == current_head) return false;
        
        queue_[tail_] = pkt;
        
        // Memory barrier: ensure data write completes before updating tail_
        __atomic_thread_fence(__ATOMIC_RELEASE);
        
        tail_ = next_tail;
        return true;
    }
    inline uint8_t* dequeue() {
        size_t current_head = head_;
        size_t current_tail = tail_;
        
        // Memory barrier: ensure tail_ is read with up-to-date value
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        
        if (current_head == current_tail) return nullptr;
        
        uint8_t* ret = queue_[current_head];
        
        // Memory barrier: ensure data read completes before updating head_
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        
        head_ = (current_head + 1) & mask_;
        return ret;
    }
    inline void reset_head() {
        head_ = 0;
    }
    inline void reset_tail() {
        tail_ = 0;
    }
    inline size_t get_size() {
        return (tail_ - head_) & mask_;
    }
};
} // namespace dperf