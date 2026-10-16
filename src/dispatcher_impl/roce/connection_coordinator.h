#pragma once

#include "util/mgnt_connection.h"
#include "util/qpinfo.hh"
#include <condition_variable>
#include <exception>
#include <map>
#include <memory>
#include <mutex>

namespace axio {

// Startup-only rendezvous. No datapath operations take this mutex.
class RoceConnectionCoordinator {
 public:
  using Manifest = std::map<std::string, QueuePairInfo>;

  void prepare(size_t dispatchers, const char* remote_ip, int port, bool server) {
    this->expected_ = dispatchers;
    this->server_role_ = server;
    if (server) this->server_ = std::make_unique<TcpServer>(port);
    else {
      this->client_ = std::make_unique<TcpClient>();
      this->client_->connect_to_server(remote_ip, port);
    }
  }

  Manifest exchange(const Manifest& local) {
    std::unique_lock<std::mutex> lock(this->mutex_);
    if (this->error_) std::rethrow_exception(this->error_);
    for (const auto& item : local) {
      if (!this->local_.insert(item).second) throw std::runtime_error("duplicate local RC link");
    }
    if (++this->arrived_ == this->expected_) {
      try {
        std::ostringstream payload;
        for (const auto& [key, info] : this->local_) payload << key << ' ' << info.serialize() << '\n';
        std::string incoming;
        if (this->server_role_) {
          this->server_->accept_connection();
          incoming = this->server_->receive_message();
          this->server_->send_message(payload.str());
        } else {
          this->client_->send_message(payload.str());
          incoming = this->client_->receive_message();
        }
        std::istringstream input(incoming);
        std::string line;
        while (std::getline(input, line)) {
          const size_t split = line.find(' ');
          if (split == std::string::npos) throw std::runtime_error("invalid RC manifest record");
          const std::string key = line.substr(0, split);
          QueuePairInfo info;
          info.deserialize(line.substr(split + 1));
          if (!this->local_.count(key) || !info.initialized_ || info.queue_pair_number_ == 0 ||
              !this->remote_.emplace(key, info).second) throw std::runtime_error("unexpected RC manifest link");
          if (info.mtu_ != this->local_.at(key).mtu_) throw std::runtime_error("RC peer MTU mismatch");
        }
        if (this->remote_.size() != this->local_.size()) throw std::runtime_error("missing RC manifest link");
        this->ready_ = true;
      } catch (...) {
        this->error_ = std::current_exception();
      }
      this->condition_.notify_all();
    } else {
      this->condition_.wait(lock, [this] { return this->ready_ || this->error_; });
    }
    if (this->error_) std::rethrow_exception(this->error_);
    Manifest result;
    for (const auto& item : local) result.emplace(item.first, this->remote_.at(item.first));
    return result;
  }

  void synchronize(const std::string& phase) {
    if (this->server_role_) {
      if (this->server_->receive_message() != phase) throw std::runtime_error("invalid peer readiness");
      this->server_->send_message(phase);
    } else {
      this->client_->send_message(phase);
      if (this->client_->receive_message() != phase) throw std::runtime_error("invalid peer readiness");
    }
  }

  void cancel(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->error_) this->error_ = error;
    this->condition_.notify_all();
  }

 private:
  size_t expected_ = 0;
  size_t arrived_ = 0;
  bool server_role_ = false;
  bool ready_ = false;
  Manifest local_, remote_;
  std::exception_ptr error_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::unique_ptr<TcpServer> server_;
  std::unique_ptr<TcpClient> client_;
};

inline RoceConnectionCoordinator roce_connections;

}  // namespace axio
