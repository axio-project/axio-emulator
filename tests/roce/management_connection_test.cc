#include "util/mgnt_connection.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

bool reused_descriptor_survives(bool disconnect_twice) {
  // Reserve the next descriptor number without exposing TcpClient internals.
  const int socket_fd = open("/dev/null", O_RDONLY);
  if (socket_fd < 0) return false;
  close(socket_fd);

  int replacement_fd = -1;
  bool passed = true;
  {
    axio::TcpClient client;
    int socket_type = 0;
    socklen_t length = sizeof(socket_type);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &socket_type, &length) != 0 ||
        socket_type != SOCK_STREAM) {
      std::fprintf(stderr, "FAIL: fixture did not locate the client socket\n");
      return false;
    }
    client.disconnect();
    passed = fcntl(socket_fd, F_GETFD) == -1;
    replacement_fd = open("/dev/null", O_RDONLY);
    if (replacement_fd != socket_fd) {
      if (replacement_fd >= 0) close(replacement_fd);
      std::fprintf(stderr, "FAIL: fixture did not reuse the socket descriptor\n");
      return false;
    }
    if (disconnect_twice) client.disconnect();
    passed = (fcntl(replacement_fd, F_GETFD) != -1) && passed;
  }
  passed = (fcntl(replacement_fd, F_GETFD) != -1) && passed;
  close(replacement_fd);
  if (!passed) {
    std::fprintf(stderr, "FAIL: %s closed a reused descriptor\n",
                 disconnect_twice ? "repeated disconnect" : "destructor");
  }
  return passed;
}

bool connection_waits_for_listener(bool start_listener) {
  using namespace std::chrono;
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  if (server < 0 || bind(server, reinterpret_cast<sockaddr*>(&address),
                         sizeof(address)) != 0 ||
      getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    if (server >= 0) close(server);
    return false;
  }

  const auto started = steady_clock::now();
  std::fflush(nullptr);
  const pid_t child = fork();
  if (child == 0) {
    close(server);
    axio::TcpClient client;
    client.connect_to_server("127.0.0.1", ntohs(address.sin_port));
    // A delayed reply also verifies that connect restores blocking I/O.
    const bool replied = client.receive_message() == "ready";
    client.disconnect();
    _exit(replied ? 0 : 2);
  }
  if (child < 0) {
    close(server);
    return false;
  }

  bool passed = true;
  if (start_listener) {
    std::this_thread::sleep_for(milliseconds(300));
    passed = listen(server, 1) == 0;
    pollfd pending{server, POLLIN, 0};
    passed = passed && poll(&pending, 1, 2000) > 0;
    if (passed) {
      const int peer = accept(server, nullptr, nullptr);
      passed = peer >= 0;
      if (peer >= 0) {
        std::this_thread::sleep_for(milliseconds(100));
        passed = send(peer, "ready", 5, 0) == 5;
        close(peer);
      }
    }
    close(server);
  }
  int status = 0;
  passed = (waitpid(child, &status, 0) == child) && passed;
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - started);
  if (!start_listener) close(server);
  passed = passed && WIFEXITED(status) &&
           WEXITSTATUS(status) == (start_listener ? 0 : 1);
  passed = passed && (start_listener ? elapsed.count() < 2500
                                    : elapsed.count() >= 2900 &&
                                          elapsed.count() < 4000);
  std::fprintf(passed ? stdout : stderr, "%s: %s (%lld ms)\n",
               passed ? "PASS" : "FAIL",
               start_listener ? "delayed listener and blocking receive"
                              : "unavailable listener times out in three seconds",
               static_cast<long long>(elapsed.count()));
  return passed;
}

}  // namespace

int main() {
  const bool destructor_safe = reused_descriptor_survives(false);
  const bool repeated_disconnect_safe = reused_descriptor_survives(true);
  const bool delayed_listener = connection_waits_for_listener(true);
  const bool timeout = connection_waits_for_listener(false);
  if (!destructor_safe || !repeated_disconnect_safe || !delayed_listener ||
      !timeout) return 1;
  std::puts("Management connection checks passed");
  return 0;
}
