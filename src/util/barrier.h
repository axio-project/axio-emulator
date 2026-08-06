#pragma once
#include <mutex>
#include <condition_variable>

namespace axio {
class ThreadBarrier {
 public:
  explicit ThreadBarrier(int thread_count)
      : count_(0), total_threads_(thread_count), generation_(0) {}

  void wait() {
    std::unique_lock<std::mutex> lock(this->mutex_);
    const int generation = this->generation_;

    if (++this->count_ < this->total_threads_) {
      this->condition_.wait(
          lock, [this, generation]() { return generation != this->generation_; });
    } else {
      this->count_ = 0;
      this->generation_++;
      this->condition_.notify_all();
    }
  }

 private:
  int count_;
  int total_threads_;
  int generation_;
  std::mutex mutex_;
  std::condition_variable condition_;
};
}  // namespace axio
