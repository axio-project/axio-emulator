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

namespace axio {

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
      if (error == 0 && fcntl(this->socket_fd_, F_SETFL, flags) == 0) return;

      this->disconnect();
      if (error != ECONNREFUSED && error != EINTR) break;
      std::this_thread::sleep_until(std::min(
          deadline, Clock::now() + std::chrono::milliseconds(100)));
      if (Clock::now() < deadline) this->__create_socket();
    }
    this->disconnect();
    std::cerr << "Axio: management connection failed." << std::endl;
    exit(1);
  }

  void send_message(const std::string& message) {
    send(this->socket_fd_, message.c_str(), message.length(), 0);
  }

  std::string receive_message() {
    char buffer[1024] = {0};
    int bytes_read = read(this->socket_fd_, buffer, sizeof(buffer));
    return std::string(buffer, bytes_read);
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
    if ((this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
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

  ~TcpServer() { close(this->server_fd_); }

  void accept_connection() {
    if ((this->client_socket_ =
             accept(this->server_fd_,
                    reinterpret_cast<struct sockaddr*>(&this->address_),
                    reinterpret_cast<socklen_t*>(&this->address_length_))) <
        0) {
      std::cerr << "Axio: management socket accept failed." << std::endl;
      exit(1);
    }
  }

  void send_message(const std::string& message) {
    send(this->client_socket_, message.c_str(), message.length(), 0);
  }

  std::string receive_message() {
    char buffer[1024] = {0};
    int bytes_read = read(this->client_socket_, buffer, sizeof(buffer));
    return std::string(buffer, bytes_read);
  }

  void disconnect() { close(this->client_socket_); }

 private:
  int server_fd_;
  int client_socket_;
  struct sockaddr_in address_;
  int address_length_ = sizeof(this->address_);
};

}  // namespace axio
