#pragma once
#include <mutex>
#include <condition_variable>
#include <exception>

namespace axio {
class ThreadBarrier {
 public:
  explicit ThreadBarrier(int thread_count)
      : count_(0), total_threads_(thread_count), generation_(0) {}

  void wait() {
    std::unique_lock<std::mutex> lock(this->mutex_);
    if (this->error_) std::rethrow_exception(this->error_);
    const int generation = this->generation_;

    if (++this->count_ < this->total_threads_) {
      this->condition_.wait(
          lock, [this, generation]() { return this->error_ || generation != this->generation_; });
    } else {
      this->count_ = 0;
      this->generation_++;
      this->condition_.notify_all();
    }
    if (this->error_) std::rethrow_exception(this->error_);
  }

  void cancel(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->error_) this->error_ = error;
    this->condition_.notify_all();
  }

  bool failed() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    return this->error_ != nullptr;
  }

 private:
  int count_;
  int total_threads_;
  int generation_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::exception_ptr error_;
};
}  // namespace axio
