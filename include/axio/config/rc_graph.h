#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace axio::config {

struct RcLink {
  uint32_t workload;
  uint32_t client;
  uint32_t server;
  uint32_t weight;
};

// Integer interval intersections preserve equal aggregate load on both sides.
inline std::vector<RcLink> rc_links(uint32_t workload,
                                   std::vector<uint32_t> clients,
                                   std::vector<uint32_t> servers) {
  if (clients.empty() || servers.empty()) {
    throw std::invalid_argument("RC graph requires dispatchers on both endpoints");
  }
  std::sort(clients.begin(), clients.end());
  std::sort(servers.begin(), servers.end());
  const size_t span = std::lcm(clients.size(), servers.size());
  const size_t c_width = span / clients.size();
  const size_t s_width = span / servers.size();
  std::vector<RcLink> links;
  for (size_t c = 0, s = 0; c < clients.size() && s < servers.size();) {
    const size_t end = std::min((c + 1) * c_width, (s + 1) * s_width);
    const size_t begin = std::max(c * c_width, s * s_width);
    links.push_back({workload, clients[c], servers[s],
                     static_cast<uint32_t>(end - begin)});
    const bool advance_c = (c + 1) * c_width == end;
    const bool advance_s = (s + 1) * s_width == end;
    c += advance_c;
    s += advance_s;
  }
  return links;
}

}  // namespace axio::config
