"""Audit description of the C++ interval-overlap RC graph (not a routing policy)."""
import math


def connection_document(config):
    network = config["network"]
    if network["backend"] != "roce" or network["roce_transport"] != "rc":
        return None
    client = config["deployment"]["role"] == "client"
    links = []
    for workload in config["deployment"]["topology"]["workloads"]:
        local = sorted({g["dispatcher"] for g in workload["groups"]})
        remote = sorted(workload["remote_dispatchers"])
        clients, servers = (local, remote) if client else (remote, local)
        span = math.lcm(len(clients), len(servers))
        cw, sw = span // len(clients), span // len(servers)
        for c, cid in enumerate(clients):
            for s, sid in enumerate(servers):
                overlap = min((c + 1) * cw, (s + 1) * sw) - max(c * cw, s * sw)
                if overlap > 0:
                    links.append(dict(workload=workload["id"], client=cid, server=sid,
                                      request_weight=overlap))
    role = "client" if client else "server"
    for dispatcher in sorted({link[role] for link in links}):
        owned = [link for link in links if link[role] == dispatcher]
        for i, link in enumerate(owned):
            for direction in ("rx", "tx"):
                budget = network[f"{direction}_ring_entries"]
                link[f"local_{direction}_slots"] = (i + 1) * budget // len(owned) - i * budget // len(owned)
    return dict(schema="axio.rc-connections/v1", endpoint_role=role,
                link_count=len(links), links=links,
                resource_model="One PD/MR/pool and TX/RX CQ per dispatcher; ring quotas are partitioned, not multiplied.",
                fixed_client="Client C1/C2 and batch knobs stay fixed; QP count follows target C2.")
