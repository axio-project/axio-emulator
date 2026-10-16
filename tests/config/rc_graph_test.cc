#include "axio/config/rc_graph.h"
#include <cassert>
#include <map>

int main() {
  const std::vector<uint32_t> clients{8, 9, 10, 11, 12, 13, 14, 15};
  for (const auto [count, expected] : std::vector<std::pair<int, int>>{
           {16,16}, {15,22}, {8,8}, {7,14}, {1,8}}) {
    std::vector<uint32_t> servers(count);
    std::iota(servers.begin(), servers.end(), 0);
    const auto links = axio::config::rc_links(1, clients, servers);
    assert(links.size() == static_cast<size_t>(expected));
    std::map<uint32_t, size_t> c_weights, s_weights;
    for (const auto& link : links) {
      c_weights[link.client] += link.weight;
      s_weights[link.server] += link.weight;
    }
    for (const auto& item : c_weights) assert(item.second == c_weights.begin()->second);
    for (const auto& item : s_weights) assert(item.second == s_weights.begin()->second);
  }
}
