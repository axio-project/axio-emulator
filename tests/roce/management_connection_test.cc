#include "util/mgnt_connection.h"

#include <fcntl.h>

#include <cstdio>

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

}  // namespace

int main() {
  const bool destructor_safe = reused_descriptor_survives(false);
  const bool repeated_disconnect_safe = reused_descriptor_survives(true);
  if (!destructor_safe || !repeated_disconnect_safe) return 1;
  std::puts("Management connection descriptor reuse checks passed");
  return 0;
}
