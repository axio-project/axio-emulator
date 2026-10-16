#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <stdexcept>
#include <limits>

namespace axio {

inline void management_timeout(int fd) {
  timeval timeout{120, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

inline void management_transfer(int fd, void* data, size_t size, bool sending) {
  auto* cursor = static_cast<char*>(data);
  while (size != 0) {
    const ssize_t done = sending ? send(fd, cursor, size, MSG_NOSIGNAL)
                                 : recv(fd, cursor, size, 0);
    if (done < 0 && errno == EINTR) continue;
    if (done <= 0) throw std::runtime_error("management connection closed or timed out");
    cursor += done;
    size -= static_cast<size_t>(done);
  }
}

inline void send_management_message(int fd, const std::string& message) {
  if (message.size() > 1024 * 1024) throw std::runtime_error("management message too large");
  uint32_t size = htonl(static_cast<uint32_t>(message.size()));
  management_transfer(fd, &size, sizeof(size), true);
  management_transfer(fd, const_cast<char*>(message.data()), message.size(), true);
}

inline std::string receive_management_message(int fd) {
  uint32_t size = 0;
  management_transfer(fd, &size, sizeof(size), false);
  size = ntohl(size);
  if (size > 1024 * 1024) throw std::runtime_error("management message too large");
  std::string message(size, '\0');
  management_transfer(fd, message.data(), size, false);
  return message;
}

class TcpClient {
 public:
  TcpClient() { this->__create_socket(); }

  ~TcpClient() { this->disconnect(); }

  void connect_to_server(const char* ip_address, int port) {
    this->server_address_.sin_family = AF_INET;
    this->server_address_.sin_port = htons(port);
    inet_pton(AF_INET, ip_address, &this->server_address_.sin_addr);

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    // A peer may still be finishing initialization. Bound both each pending
    // connect and all retries by the same deadline, then restore blocking I/O.
    while (Clock::now() < deadline) {
      const int flags = fcntl(this->socket_fd_, F_GETFL, 0);
      if (flags < 0 ||
          fcntl(this->socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) break;

      int error = 0;
      if (connect(this->socket_fd_,
                  reinterpret_cast<struct sockaddr*>(&this->server_address_),
                  sizeof(this->server_address_)) < 0) {
        error = errno;
        if (error == EINPROGRESS) {
          pollfd pending{this->socket_fd_, POLLOUT, 0};
          while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() < 0) {
              error = ETIMEDOUT;
              break;
            }
            const int ready = poll(&pending, 1,
                                   static_cast<int>(remaining.count()) + 1);
            if (ready < 0 && errno == EINTR) continue;
            error = ETIMEDOUT;
            if (ready > 0) {
              socklen_t length = sizeof(error);
              if (getsockopt(this->socket_fd_, SOL_SOCKET, SO_ERROR,
                             &error, &length) < 0) error = errno;
            } else if (ready < 0) {
              error = errno;
            }
            break;
          }
        }
      }
      if (error == 0 && fcntl(this->socket_fd_, F_SETFL, flags) == 0) {
        management_timeout(this->socket_fd_);
        return;
      }

      this->disconnect();
      if (error != ECONNREFUSED && error != EINTR) break;
      std::this_thread::sleep_until(std::min(
          deadline, Clock::now() + std::chrono::milliseconds(100)));
      if (Clock::now() < deadline) this->__create_socket();
    }
    this->disconnect();
    throw std::runtime_error("Axio: management connection failed.");
  }

  void send_message(const std::string& message) {
    send_management_message(this->socket_fd_, message);
  }

  std::string receive_message() {
    return receive_management_message(this->socket_fd_);
  }

  void disconnect() {
    if (this->socket_fd_ >= 0) {
      close(this->socket_fd_);
      this->socket_fd_ = -1;
    }
  }

 private:
  void __create_socket() {
    this->socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->socket_fd_ < 0) {
      std::cerr << "Axio: error creating management socket." << std::endl;
      exit(1);
    }

    struct linger linger_option;
    linger_option.l_onoff = 1;
    linger_option.l_linger = 0;
    setsockopt(this->socket_fd_, SOL_SOCKET, SO_LINGER,
               reinterpret_cast<const char*>(&linger_option),
               sizeof(linger_option));

    int reuse_address = 1;
    setsockopt(this->socket_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse_address),
               sizeof(reuse_address));
  }

  int socket_fd_;
  struct sockaddr_in server_address_;
};

class TcpServer {
 public:
  explicit TcpServer(int port) {
    if ((this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      std::cerr << "Axio: management socket creation failed." << std::endl;
      exit(1);
    }

    struct linger linger_option;
    linger_option.l_onoff = 1;
    linger_option.l_linger = 0;
    setsockopt(this->server_fd_, SOL_SOCKET, SO_LINGER,
               reinterpret_cast<const char*>(&linger_option),
               sizeof(linger_option));

    int reuse_address = 1;
    setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse_address),
               sizeof(reuse_address));

    this->address_.sin_family = AF_INET;
    this->address_.sin_addr.s_addr = INADDR_ANY;
    this->address_.sin_port = htons(port);

    if (bind(this->server_fd_,
             reinterpret_cast<struct sockaddr*>(&this->address_),
             sizeof(this->address_)) < 0) {
      std::cerr << "Axio: management socket bind failed." << std::endl;
      exit(1);
    }

    if (listen(this->server_fd_, 3) < 0) {
      std::cerr << "Axio: management socket listen failed." << std::endl;
      exit(1);
    }
  }

  ~TcpServer() { this->disconnect(); close(this->server_fd_); }

  void accept_connection() {
    pollfd pending{this->server_fd_, POLLIN, 0};
    if (poll(&pending, 1, 120000) <= 0) throw std::runtime_error("management accept timed out");
    if ((this->client_socket_ =
             accept(this->server_fd_,
                    reinterpret_cast<struct sockaddr*>(&this->address_),
                    reinterpret_cast<socklen_t*>(&this->address_length_))) <
        0) {
      std::cerr << "Axio: management socket accept failed." << std::endl;
      exit(1);
    }
    management_timeout(this->client_socket_);
  }

  void send_message(const std::string& message) {
    send_management_message(this->client_socket_, message);
  }

  std::string receive_message() {
    return receive_management_message(this->client_socket_);
  }

  void disconnect() {
    if (this->client_socket_ >= 0) { close(this->client_socket_); this->client_socket_ = -1; }
  }

 private:
  int server_fd_;
  int client_socket_ = -1;
  struct sockaddr_in address_;
  int address_length_ = sizeof(this->address_);
};

}  // namespace axio
