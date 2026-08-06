#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace axio {

class TcpClient {
 public:
  TcpClient() {
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

  ~TcpClient() { close(this->socket_fd_); }

  void connect_to_server(const char* ip_address, int port) {
    this->server_address_.sin_family = AF_INET;
    this->server_address_.sin_port = htons(port);
    inet_pton(AF_INET, ip_address, &this->server_address_.sin_addr);

    if (connect(this->socket_fd_,
                reinterpret_cast<struct sockaddr*>(&this->server_address_),
                sizeof(this->server_address_)) < 0) {
      std::cerr << "Axio: management connection failed." << std::endl;
      exit(1);
    }
  }

  void send_message(const std::string& message) {
    send(this->socket_fd_, message.c_str(), message.length(), 0);
  }

  std::string receive_message() {
    char buffer[1024] = {0};
    int bytes_read = read(this->socket_fd_, buffer, sizeof(buffer));
    return std::string(buffer, bytes_read);
  }

  void disconnect() { close(this->socket_fd_); }

 private:
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
