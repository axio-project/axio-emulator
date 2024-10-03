/**
 * @file roce_datapath.cc
 * @brief Transmit / Receive packets with a RoCE NIC (CX5 / CX7)
 */
#include "roce_dispatcher.h"
#include "ws_impl/ws_hdr.h"

namespace dperf {

// GIDs are currently used only for RoCE. This default value works for most
// clusters, but we need a more robust GID selection method. Some observations:
//  * On physical clusters, gid_index = 0 always works (in my experience)
//  * On VM clusters (AWS/KVM), gid_index = 0 does not work, gid_index = 1 works
//  * Mellanox's `show_gids` script lists all GIDs on all NICs
static constexpr size_t kDefaultGIDIndex = 1;   // Currently, the GRH (ipv4 + udp port) is set by CPU

// Initialize the protection domain, queue pair, and memory registration 
// and deregistration functions. RECVs will be initialized later 
// when the hugepage allocator is provided.

RoceDispatcher::RoceDispatcher(uint8_t ws_id, uint8_t phy_port, size_t numa_node, UserConfig *user_config)
  : Dispatcher(DispatcherType::kDPDK, ws_id, phy_port, numa_node, user_config) {
    common_resolve_phy_port(user_config->server_config_->device_name, phy_port, kMTU, resolve_);
    roce_resolve_phy_port();
    init_verbs_structs();
    /// register memory region and register mem alloc/dealloc function
    init_mem_reg_funcs(numa_node);

    ipaddr_init(&resolve_.ipv4_addr_, kLocalIpStr);
    daddr_ = new ipaddr_t;
    ipaddr_init(daddr_, kRemoteIpStr);

    /// create management TCP connection
    struct QPInfo qp_info;
    struct QPInfo remote_qp_info;
    set_local_qp_info(&qp_info);
  #if NODE_TYPE == SERVER
    TCPServer mgnt_server(kDefaultMngtPort + ws_id);
    mgnt_server.acceptConnection();
    mgnt_server.sendMsg(qp_info.serialize());
    remote_qp_info.deserialize(mgnt_server.receiveMsg());
  #elif NODE_TYPE == CLIENT
    TCPClient mgnt_client;
    mgnt_client.connectToServer(kRemoteIpStr, kDefaultMngtPort + ws_id);
    mgnt_client.sendMsg(qp_info.serialize());
    remote_qp_info.deserialize(mgnt_client.receiveMsg());
  #endif
    set_remote_qp_info(&remote_qp_info);
    DPERF_INFO("RoceDispatcher is initialized\n");
}

RoceDispatcher::~RoceDispatcher() {
  DPERF_INFO("Destroying dispatcher for Qp %lu\n", qp_id_);

  // deregister memory region
  int ret = ibv_dereg_mr(mr_);
  if (ret != 0) {
    DPERF_ERROR("Memory degistration failed. size %zu B, lkey %u\n",
                mr_->length / MB(1), mr_->lkey);
  }
  DPERF_INFO("Deregistered %zu MB (lkey = %u)\n", mr_->length / MB(1), mr_->lkey);
  // delete Buffer in rx_queue_
  for (size_t i = 0; i < kRQDepth; i++) {
    delete rx_ring_[i];
  }
  // delete SHM
  delete huge_alloc_;

  // Destroy QPs and CQs. QPs must be destroyed before CQs.
  exit_assert(ibv_destroy_qp(qp_) == 0, "Failed to destroy send QP");
  exit_assert(ibv_destroy_cq(send_cq_) == 0, "Failed to destroy send CQ");
  exit_assert(ibv_destroy_cq(recv_cq_) == 0, "Failed to destroy recv CQ");

  exit_assert(ibv_destroy_ah(self_ah_) == 0, "Failed to destroy self AH");
  if (remote_ah_ != nullptr) {
      exit_assert(ibv_destroy_ah(remote_ah_) == 0,
                  "Failed to destroy remote AH");
  }
  for (auto *_ah : ah_to_free_vec) {
    exit_assert(ibv_destroy_ah(_ah) == 0, "Failed to destroy AH");
  }

  // Destroy protection domain and device context
  exit_assert(ibv_dealloc_pd(pd_) == 0, "Failed to destroy PD. Leaked MRs?");
  exit_assert(ibv_close_device(resolve_.ib_ctx) == 0, "Failed to close device");
}

struct ibv_ah *RoceDispatcher::create_ah(const ib_routing_info_t *ib_rinfo) const {
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
  ah_attr.is_global = 1;
  ah_attr.dlid = 0;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = resolve_.dev_port_id;  // Local port

  ah_attr.grh.dgid.global.interface_id = ib_rinfo->gid.global.interface_id;
  ah_attr.grh.dgid.global.subnet_prefix = ib_rinfo->gid.global.subnet_prefix;
  ah_attr.grh.sgid_index = kDefaultGIDIndex;
  ah_attr.grh.hop_limit = 2;

  return ibv_create_ah(pd_, &ah_attr);
}

void RoceDispatcher::fill_local_routing_info(routing_info_t *routing_info) const {
  memset(static_cast<void *>(routing_info), 0, kMaxRoutingInfoSize);
  auto *ib_routing_info = reinterpret_cast<ib_routing_info_t *>(routing_info);
  ib_routing_info->port_lid = resolve_.port_lid;
  ib_routing_info->qpn = qp_->qp_num;
  ib_routing_info->gid = resolve_.gid;
}

void RoceDispatcher::set_local_qp_info(QPInfo *qp_info) {
  qp_info->qp_num = qp_id_;
  qp_info->lid = resolve_.port_lid;
  // qp_info->gid = resolve_.gid;
  for (size_t i = 0; i < 16; i++) {
    qp_info->gid[i] = resolve_.gid.raw[i];
  }
  qp_info->mtu = kMTU;
  // qp_info->nic_name = resolve_.ib_ctx->device->name;
  memcpy(qp_info->nic_name, resolve_.ib_ctx->device->name, MAX_NIC_NAME_LEN);
  
}

bool RoceDispatcher::set_remote_qp_info(QPInfo *qp_info) {
  remote_qp_id_ = qp_info->qp_num;
  struct ibv_ah_attr ah_attr = {};
            ah_attr.sl = 0;
            ah_attr.src_path_bits = 0;
            ah_attr.port_num = 1;
            ah_attr.dlid = qp_info->lid;
            memcpy(&ah_attr.grh.dgid, qp_info->gid, 16);
            ah_attr.is_global = 1;
            ah_attr.grh.sgid_index = kDefaultGIDIndex;
            ah_attr.grh.hop_limit = 2;
            ah_attr.grh.traffic_class = 0;

            remote_ah_ = ibv_create_ah(pd_, &ah_attr);

  rt_assert(remote_ah_ != nullptr, "Failed to create remote AH.");
  return true;
}

void RoceDispatcher::roce_resolve_phy_port() {
  std::ostringstream xmsg;  // The exception message
  struct ibv_port_attr port_attr;

  if (ibv_query_port(resolve_.ib_ctx, resolve_.dev_port_id, &port_attr) != 0) {
    xmsg << "Failed to query port " << std::to_string(resolve_.dev_port_id)
         << " on device " << resolve_.ib_ctx->device->name;
    throw std::runtime_error(xmsg.str());
  }

  resolve_.port_lid = port_attr.lid;

  int ret = ibv_query_gid(resolve_.ib_ctx, resolve_.dev_port_id,
                          kDefaultGIDIndex, &resolve_.gid);
  rt_assert(ret == 0, "Failed to query GID");
}

void RoceDispatcher::init_verbs_structs() {
  assert(resolve_.ib_ctx != nullptr && resolve_.device_id != -1);

  // Create protection domain, send CQ, and recv CQ
  pd_ = ibv_alloc_pd(resolve_.ib_ctx);
  rt_assert(pd_ != nullptr, "Failed to allocate PD");

  send_cq_ = ibv_create_cq(resolve_.ib_ctx, kSQDepth, nullptr, nullptr, 0);
  rt_assert(send_cq_ != nullptr, "Failed to create SEND CQ. Forgot hugepages?");

  recv_cq_ = ibv_create_cq(resolve_.ib_ctx, kRQDepth, nullptr, nullptr, 0);
  rt_assert(recv_cq_ != nullptr, "Failed to create SEND CQ");

  // Initialize QP creation attributes
  struct ibv_qp_init_attr create_attr;
  memset(static_cast<void *>(&create_attr), 0, sizeof(struct ibv_qp_init_attr));
  create_attr.send_cq = send_cq_;
  create_attr.recv_cq = recv_cq_;
  create_attr.qp_type = IBV_QPT_UD;

  create_attr.cap.max_send_wr = kSQDepth;
  create_attr.cap.max_recv_wr = kRQDepth;
  create_attr.cap.max_send_sge = 1;
  create_attr.cap.max_recv_sge = 1;
  create_attr.cap.max_inline_data = kMaxInline;

  qp_ = ibv_create_qp(pd_, &create_attr);
  rt_assert(qp_ != nullptr, "Failed to create QP");
  qp_id_ = qp_->qp_num;

  // Transition QP to INIT state
  struct ibv_qp_attr init_attr;
  memset(static_cast<void *>(&init_attr), 0, sizeof(struct ibv_qp_attr));
  init_attr.qp_state = IBV_QPS_INIT;
  init_attr.pkey_index = 0;
  init_attr.port_num = static_cast<uint8_t>(resolve_.dev_port_id);
  init_attr.qkey = kQKey;

  int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
  if (ibv_modify_qp(qp_, &init_attr, attr_mask) != 0) {
    throw std::runtime_error("Failed to modify QP to init");
  }

  // RTR state
  struct ibv_qp_attr rtr_attr;
  memset(static_cast<void *>(&rtr_attr), 0, sizeof(struct ibv_qp_attr));
  rtr_attr.qp_state = IBV_QPS_RTR;

  if (ibv_modify_qp(qp_, &rtr_attr, IBV_QP_STATE)) {
    throw std::runtime_error("Failed to modify QP to RTR");
  }

  // Create self address handle. We use local routing info for convenience,
  // so this must be done after creating the QP.
  routing_info_t self_routing_info;
  fill_local_routing_info(&self_routing_info);
  self_ah_ =
      create_ah(reinterpret_cast<ib_routing_info_t *>(&self_routing_info));
  rt_assert(self_ah_ != nullptr, "Failed to create self AH.");

  // Reuse rtr_attr for RTS
  rtr_attr.qp_state = IBV_QPS_RTS;
  rtr_attr.sq_psn = 0;  // PSN does not matter for UD QPs

  if (ibv_modify_qp(qp_, &rtr_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    throw std::runtime_error("Failed to modify QP to RTS");
  }
  // Check if driver is modded for fast RECVs
  // struct ibv_recv_wr mod_probe_wr;
  // mod_probe_wr.wr_id = kModdedProbeWrID;
  // struct ibv_recv_wr *bad_wr = &mod_probe_wr;

  // int probe_ret = ibv_post_recv(qp_, nullptr, &bad_wr);
  // if (probe_ret != kModdedProbeRet) {
  //   ERPC_WARN("Modded driver unavailable. Performance will be low.\n");
  //   use_fast_recv = false;
  // } else {
  //   ERPC_WARN("Modded driver available.\n");
  //   use_fast_recv = true;
  // }
}

/// Mbuf allocation function
Buffer * roce_mbuf_alloc(void *huge_alloc) {
  Buffer * mbuf = ((HugeAlloc*)huge_alloc)->alloc(RoceDispatcher::kMbufSize); // Currently only support fixed size mbuf
  return mbuf;
}

/// Mbuf bulk allocation function
uint8_t roce_mbuf_alloc_bulk(void *huge_alloc, Buffer **mbufs, size_t num) {
  for (size_t i = 0; i < num; i++) {
    mbufs[i] = ((HugeAlloc*)huge_alloc)->alloc(RoceDispatcher::kMbufSize); // Currently only support fixed size mbuf
    if (mbufs[i]->buf_ == nullptr) {
      for (size_t j = 0; j < i; j++) {
        ((HugeAlloc*)huge_alloc)->free_buf(mbufs[j]);
      }
      return -1;
    }
  }
  return 0;
}

/// Mbuf de-allocation function
void roce_mbuf_de_alloc(Buffer *mbuf, void *huge_alloc) { 
  mbuf->state_ = Buffer::kFREE_BUF;
  return;
}

/// Mbuf bulk de-allocation function
void roce_mbuf_de_alloc_bulk(Buffer **mbufs, size_t num, void *huge_alloc) {
  for (size_t i = 0; i < num; i++) {
    mbufs[i]->state_ = Buffer::kFREE_BUF;
  }
  return;
}

/// Set mbuf payload
void roce_set_mbuf_paylod(Buffer *mbuf, char* uh, char* ws_header, size_t payload_size) {
  mbuf->length_ = sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) + sizeof(ws_hdr) + payload_size;
  memcpy(mbuf->get_uh(), uh, sizeof(udphdr)); 
  memcpy(mbuf->get_ws_hdr(), ws_header, sizeof(ws_hdr));
  if (unlikely(payload_size == 0)) {
    return;
  }
  char *payload_ptr = (char *)mbuf->get_ws_payload();
  memset(payload_ptr, 'a', payload_size - 1);
  payload_ptr[payload_size - 1] = '\0'; 
}

ws_hdr* roce_extracr_ws_hdr(Buffer *mbuf){
  return (ws_hdr*)(mbuf->get_ws_hdr());
}

/// Copy payload from src to dst
void roce_cp_payload(Buffer *dst, Buffer *src, char* uh, char* ws_header, size_t payload_size) {
  dst->length_ = sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) + sizeof(ws_hdr) + payload_size;

  memcpy(dst->get_uh(), uh, sizeof(udphdr)); 
  memcpy(dst->get_ws_hdr(), ws_header, sizeof(ws_hdr));
  char *payload_ptr = (char *)dst->get_ws_payload();
  memcpy(payload_ptr, src->get_ws_payload(), payload_size);
}

void RoceDispatcher::init_mem_reg_funcs(uint8_t numa_node) {
  std::ostringstream xmsg;  // The exception message

  /// create huge page allocator
  huge_alloc_ = new HugeAlloc(kMemRegionSize, numa_node);
  /// alloc and register memory region
  Buffer raw_mr = huge_alloc_->alloc_raw(kMemRegionSize, DoRegister::kTrue);
  if (raw_mr.buf_ == nullptr) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * kMemRegionSize / MB(1) << "MB for ring buffers. "
         << HugeAlloc::kAllocFailHelpStr;
    throw std::runtime_error(xmsg.str());
  }
  mr_ = ibv_reg_mr(pd_, raw_mr.buf_, kMemRegionSize, IBV_ACCESS_LOCAL_WRITE);
  rt_assert(mr_ != nullptr, "Failed to register mr.");
  raw_mr.set_lkey(mr_->lkey);
  /// split the raw buffer to freelist
  huge_alloc_->add_raw_buffer(raw_mr, kMemRegionSize);

  /// init rx / tx ring
  init_recvs();
  init_sends();
  /// register memory region and register mem alloc/dealloc function
  mem_reg_info_ = new mem_reg_info<Buffer>(huge_alloc_, &roce_mbuf_alloc, &roce_mbuf_de_alloc, &roce_mbuf_alloc_bulk, &roce_mbuf_de_alloc_bulk, &roce_set_mbuf_paylod, &roce_extracr_ws_hdr, &roce_cp_payload);
}

void RoceDispatcher::init_recvs() {
  std::ostringstream xmsg;  // The exception message

  // Initialize the memory region for RECVs
  const size_t ring_extent_size = kRQDepth * kMbufSize;
  assert(ring_extent_size <= HugeAlloc::k_max_class_size); // Currently the max memory size for rx ring is k_max_class_size

  Buffer * ring_extent = huge_alloc_->alloc(ring_extent_size);
  if (ring_extent->buf_ == nullptr) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * ring_extent_size / MB(1) << "MB for ring buffers. "
         << HugeAlloc::kAllocFailHelpStr;
    throw std::runtime_error(xmsg.str());
  }

  // Initialize constant fields of RECV descriptors
  for (size_t i = 0; i < kRQDepth; i++) {
    uint8_t *buf = ring_extent->buf_;

    // Break down the memory space into fixed-length (kMbufSize) chunks
    // Each chunk is a mbuf, and the first 64 Bytes are for GRH
    // const size_t offset = i * kMbufSize;
    const size_t offset = (i * kMbufSize) + (64 - kGRHBytes);
    assert(offset + (kGRHBytes + kMTU) <= ring_extent_size);

    recv_sgl[i].length = kMbufSize;
    recv_sgl[i].lkey = ring_extent->lkey_;
    recv_sgl[i].addr = reinterpret_cast<uint64_t>(&buf[offset]);

    // recv_wr[i].wr_id = recv_sgl[i].addr;  // For quick prefetch
    recv_wr[i].wr_id = i;
    recv_wr[i].sg_list = &recv_sgl[i];
    recv_wr[i].num_sge = 1;

    rx_ring_[i] = new Buffer(&buf[offset + kGRHBytes], kMbufSize, ring_extent->lkey_);  // RX ring entry
    rx_ring_[i]->state_ = Buffer::kPOSTED;

    // Circular link
    recv_wr[i].next = (i < kRQDepth - 1) ? &recv_wr[i + 1] : &recv_wr[0];
  }

  // Curcular link rx ring
  for (size_t i = 0; i < kRQDepth; i++) {
    rx_ring_[i]->next_ = (i < kRQDepth - 1) ? rx_ring_[i + 1] : rx_ring_[0];
  }

  // Fill the RECV queue. post_recvs() can use fast RECV and therefore not
  // actually fill the RQ, so post_recvs() isn't usable here.
  struct ibv_recv_wr *bad_wr;
  recv_wr[kRQDepth - 1].next = nullptr;  // Breaker of chains, mother of dragons

  int ret = ibv_post_recv(qp_, &recv_wr[0], &bad_wr);
  rt_assert(ret == 0, "Failed to fill RECV queue.");

  recv_wr[kRQDepth - 1].next = &recv_wr[0];  // Restore circularity
}

void RoceDispatcher::init_sends() {
  for (size_t i = 0; i < kSQDepth; i++) {
    send_wr[i].wr.ud.remote_qkey = kQKey;
    send_wr[i].opcode = IBV_WR_SEND;
    send_wr[i].send_flags = IBV_SEND_SIGNALED;
    send_wr[i].sg_list = &send_sgl[i];
    send_wr[i].num_sge = 1;

    // Curcular link send wr
    send_wr[i].next = (i < kSQDepth - 1) ? &send_wr[i + 1] : &send_wr[0];
  }
}

// /// Register memory region
// bool register_mr(Buffer *mbuf) {
//   struct ibv_mr *mr = ibv_reg_mr(pd_, mbuf->buf_, kMbufSize, IBV_ACCESS_LOCAL_WRITE);
//   if (mr == nullptr) {
//     DPERF_ERROR("Failed to register mr.");
//     return false;
//   }
//   mbuf->set_lkey(mr->lkey);
//   return true;
// }

// /// De-register memory region
// bool deregister_mr(ibv_mr *mr) {
//   int ret = ibv_dereg_mr(mr);
//   if (ret != 0) {
//     DPERF_ERROR("Memory degistration failed. size %zu B, lkey %u\n",
//                  mr->length / MB(1), mr->lkey);
//     return false;
//   }
//   DPERF_INFO("Deregistered %zu MB (lkey = %u)\n", mr->length / MB(1), mr->lkey);
//   return true;
// }

}