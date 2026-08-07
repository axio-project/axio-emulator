/**
 * @file roce_dispatcher.cc
 * @brief Transmit / Receive packets with a RoCE NIC (CX5 / CX7)
 */
#include "roce_dispatcher.h"
#include "ws_impl/workspace_header.h"

namespace axio {

#if AXIO_ROCE_MODE
static_assert(AXIO_CONFIG_MEMPOOL_HANDLER ==
                  AXIO_MEMPOOL_HANDLER_HUGE_ALLOC,
              "RoCE requires the huge_alloc memory-pool handler");
#endif

// GIDs are currently used only for RoCE. This default value works for most
// clusters, but we need a more robust GID selection method. Some observations:
//  * On physical clusters, gid_index = 0 always works (in my experience)
//  * On VM clusters (AWS/KVM), gid_index = 0 does not work, gid_index = 1 works
//  * Mellanox's `show_gids` script lists all GIDs on all NICs
static constexpr size_t kDefaultGidIndex = 3;

// Initialize the protection domain, queue pair, and memory registration
// and deregistration functions. RECVs will be initialized later
// when the hugepage allocator is provided.

RoceDispatcher::RoceDispatcher(uint8_t workspace_id, uint8_t physical_port,
                               size_t numa_node, UserConfig* user_config)
    : Dispatcher(DispatcherType::kRoce, workspace_id, physical_port, numa_node,
                 user_config) {
  resolve_verbs_port(user_config->server().device_name_, physical_port, kMtu,
                     this->resolved_port_);
  this->_resolve_roce_port();

  // Initialize the IP and MAC addresses.
  parse_ip_address(&this->resolved_port_.ipv4_addr_, this->local_ip());
  memcpy(this->resolved_port_.mac_addr_, user_config->server().local_mac_, 6);
  this->destination_ip_ = new IpAddress;
  parse_ip_address(this->destination_ip_, this->remote_ip());

  this->_initialize_verbs(workspace_id);
  this->_initialize_memory_region_functions(numa_node);

  AXIO_INFO("RoceDispatcher is initialized\n");
}

RoceDispatcher::~RoceDispatcher() {
  AXIO_INFO("Destroying dispatcher for Qp %lu\n", this->queue_pair_id_);

  // deregister memory region
  int ret = ibv_dereg_mr(this->memory_region_);
  if (ret != 0) {
    AXIO_ERROR("Memory deregistration failed. size %zu B, lkey %u\n",
               this->memory_region_->length / AXIO_MB(1),
               this->memory_region_->lkey);
  }
  AXIO_INFO("Deregistered %zu MiB (lkey = %u)\n",
            this->memory_region_->length / AXIO_MB(1),
            this->memory_region_->lkey);
  // delete Buffer in rx_queue_
  for (size_t i = 0; i < kReceiveQueueDepth; i++) {
    delete this->receive_ring_[i];
  }
  // delete SHM
  delete this->huge_allocator_;

  // Destroy QPs and CQs. QPs must be destroyed before CQs.
  exit_assert(ibv_destroy_qp(this->queue_pair_) == 0,
              "Failed to destroy send QP");
  exit_assert(ibv_destroy_cq(this->send_completion_queue_) == 0,
              "Failed to destroy send CQ");
  exit_assert(ibv_destroy_cq(this->receive_completion_queue_) == 0,
              "Failed to destroy recv CQ");

  exit_assert(ibv_destroy_ah(this->self_address_handle_) == 0,
              "Failed to destroy self AH");
  if (this->remote_address_handle_ != nullptr) {
    exit_assert(ibv_destroy_ah(this->remote_address_handle_) == 0,
                "Failed to destroy remote AH");
  }
  for (auto* address_handle : this->address_handles_to_free_) {
    exit_assert(ibv_destroy_ah(address_handle) == 0, "Failed to destroy AH");
  }

  // Destroy protection domain and device context
  exit_assert(ibv_dealloc_pd(this->protection_domain_) == 0,
              "Failed to destroy PD. Leaked MRs?");
  exit_assert(ibv_close_device(this->resolved_port_.context_) == 0,
              "Failed to close device");
}

ibv_ah* RoceDispatcher::_create_address_handle(
    const IbRoutingInfo* routing_info) const {
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
  ah_attr.is_global = 1;
  ah_attr.dlid = 0;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = this->resolved_port_.port_id_;  // Local port

  ah_attr.grh.dgid.global.interface_id =
      routing_info->gid_.global.interface_id;
  ah_attr.grh.dgid.global.subnet_prefix =
      routing_info->gid_.global.subnet_prefix;
  ah_attr.grh.sgid_index = kDefaultGidIndex;
  ah_attr.grh.hop_limit = 2;

  return ibv_create_ah(this->protection_domain_, &ah_attr);
}

void RoceDispatcher::_fill_local_routing_info(RoutingInfo* routing_info) const {
  memset(static_cast<void*>(routing_info), 0, kMaxRoutingInfoSize);
  auto* ib_routing_info = reinterpret_cast<IbRoutingInfo*>(routing_info);
  ib_routing_info->port_lid_ = this->resolved_port_.port_lid_;
  ib_routing_info->queue_pair_number_ = this->queue_pair_->qp_num;
  ib_routing_info->gid_ = this->resolved_port_.gid_;
}

void RoceDispatcher::_set_local_queue_pair_info(
    QueuePairInfo* queue_pair_info) {
  queue_pair_info->queue_pair_number_ = this->queue_pair_id_;
  queue_pair_info->lid_ = this->resolved_port_.port_lid_;
  for (size_t i = 0; i < 16; i++) {
    queue_pair_info->gid_[i] = this->resolved_port_.gid_.raw[i];
  }
  queue_pair_info->gid_table_index_ = this->resolved_port_.gid_index_;
  queue_pair_info->mtu_ = kMtu;
  memcpy(queue_pair_info->nic_name_,
         this->resolved_port_.context_->device->name,
         kMaxNicNameLength);
  memcpy(queue_pair_info->mac_address_, this->resolved_port_.mac_addr_, 6);
  queue_pair_info->initialized_ = true;
}

bool RoceDispatcher::_set_remote_queue_pair_info(
    QueuePairInfo* queue_pair_info) {
  this->remote_queue_pair_id_ = queue_pair_info->queue_pair_number_;
  struct ibv_ah_attr ah_attr = {};
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = 1;
  ah_attr.dlid = queue_pair_info->lid_;
  memcpy(&ah_attr.grh.dgid, queue_pair_info->gid_, 16);
  ah_attr.is_global = 1;
  ah_attr.grh.sgid_index = kDefaultGidIndex;
  ah_attr.grh.hop_limit = 2;
  ah_attr.grh.traffic_class = 0;

  this->remote_address_handle_ =
      ibv_create_ah(this->protection_domain_, &ah_attr);

  rt_assert(this->remote_address_handle_ != nullptr,
            "Failed to create remote AH.");
  return true;
}

void RoceDispatcher::_resolve_roce_port() {
  std::ostringstream xmsg;  // The exception message
  struct ibv_port_attr port_attr;

  if (ibv_query_port(this->resolved_port_.context_,
                     this->resolved_port_.port_id_, &port_attr) != 0) {
    xmsg << "Failed to query port "
         << std::to_string(this->resolved_port_.port_id_)
         << " on device " << this->resolved_port_.context_->device->name;
    throw std::runtime_error(xmsg.str());
  }

  this->resolved_port_.port_lid_ = port_attr.lid;

  // Query GID information using ibv_query_gid_ex
  struct ibv_gid_entry gid_entry;
  int ret = ibv_query_gid_ex(
      this->resolved_port_.context_, this->resolved_port_.port_id_,
      kDefaultGidIndex, &gid_entry, 0);
  rt_assert(ret == 0, "Failed to query GID");
  // Validate GID
  if (gid_entry.gid_type != IBV_GID_TYPE_ROCE_V2) {
    xmsg << "Invalid GID type: expected RoCE v2, got " << gid_entry.gid_type;
    throw std::runtime_error(xmsg.str());
  }
  // Copy GID
  memcpy(&this->resolved_port_.gid_, &gid_entry.gid, sizeof(union ibv_gid));
  this->resolved_port_.gid_index_ = gid_entry.gid_index;
}

void RoceDispatcher::_initialize_verbs(uint8_t workspace_id) {
  assert(this->resolved_port_.context_ != nullptr &&
         this->resolved_port_.device_id_ != -1);

  // Create protection domain, send CQ, and recv CQ
  this->protection_domain_ = ibv_alloc_pd(this->resolved_port_.context_);
  rt_assert(this->protection_domain_ != nullptr, "Failed to allocate PD");

  this->send_completion_queue_ = ibv_create_cq(
      this->resolved_port_.context_, kSendQueueDepth, nullptr, nullptr, 0);
  rt_assert(this->send_completion_queue_ != nullptr,
            "Failed to create SEND CQ. Forgot hugepages?");

  this->receive_completion_queue_ = ibv_create_cq(
      this->resolved_port_.context_, kReceiveQueueDepth, nullptr, nullptr, 0);
  rt_assert(this->receive_completion_queue_ != nullptr,
            "Failed to create RECV CQ");

  // Initialize QP creation attributes
  struct ibv_qp_init_attr create_attr;
  memset(static_cast<void*>(&create_attr), 0, sizeof(struct ibv_qp_init_attr));
  create_attr.send_cq = this->send_completion_queue_;
  create_attr.recv_cq = this->receive_completion_queue_;
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
  create_attr.qp_type = IBV_QPT_UD;
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
  create_attr.qp_type = IBV_QPT_RC;
#endif

  create_attr.cap.max_send_wr = kSendQueueDepth;
  create_attr.cap.max_recv_wr = kReceiveQueueDepth;
  create_attr.cap.max_send_sge = 1;
  create_attr.cap.max_recv_sge = 1;
  create_attr.cap.max_inline_data = kMaxInline;

  this->queue_pair_ = ibv_create_qp(this->protection_domain_, &create_attr);
  rt_assert(this->queue_pair_ != nullptr, "Failed to create QP");
  this->queue_pair_id_ = this->queue_pair_->qp_num;

  // Exchange queue-pair information over the management connection.
  QueuePairInfo local_queue_pair_info;
  QueuePairInfo remote_queue_pair_info;
  this->_set_local_queue_pair_info(&local_queue_pair_info);
#if AXIO_NODE_TYPE == AXIO_SERVER
  TcpServer management_server(kDefaultMngtPort + workspace_id);
  management_server.accept_connection();
  management_server.send_message(local_queue_pair_info.serialize());
  remote_queue_pair_info.deserialize(management_server.receive_message());
  management_server.disconnect();
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  TcpClient management_client;
  management_client.connect_to_server(this->remote_ip(),
                                      kDefaultMngtPort + workspace_id);
  management_client.send_message(local_queue_pair_info.serialize());
  remote_queue_pair_info.deserialize(management_client.receive_message());
  management_client.disconnect();
#endif

#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
  this->_set_remote_queue_pair_info(&remote_queue_pair_info);
#endif

  // Transition QP to INIT state
  struct ibv_qp_attr init_attr;
  memset(static_cast<void*>(&init_attr), 0, sizeof(struct ibv_qp_attr));
  init_attr.qp_state = IBV_QPS_INIT;
  init_attr.pkey_index = 0;
  init_attr.port_num = static_cast<uint8_t>(this->resolved_port_.port_id_);
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
  init_attr.qkey = kQueueKey;
  int attr_mask =
      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
  init_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                              IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_ATOMIC;
  int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                  IBV_QP_ACCESS_FLAGS;
#endif

  if (ibv_modify_qp(this->queue_pair_, &init_attr, attr_mask) != 0) {
    throw std::runtime_error("Failed to modify QP to init");
  }

  // RTR state
  struct ibv_qp_attr rtr_attr;
  memset(static_cast<void*>(&rtr_attr), 0, sizeof(struct ibv_qp_attr));
  rtr_attr.qp_state = IBV_QPS_RTR;
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
  if (ibv_modify_qp(this->queue_pair_, &rtr_attr, IBV_QP_STATE)) {
    throw std::runtime_error("Failed to modify QP to RTR");
  }
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
  switch (kMtu) {
    case 1024:
      rtr_attr.path_mtu = IBV_MTU_1024;
      break;
    case 2048:
      rtr_attr.path_mtu = IBV_MTU_2048;
      break;
    case 4096:
      rtr_attr.path_mtu = IBV_MTU_4096;
      break;
    default:
      AXIO_ERROR("Invalid MTU when setting RDMA QP's RTR state: %zu\n", kMtu);
  }
  rtr_attr.dest_qp_num = remote_queue_pair_info.queue_pair_number_;
  rtr_attr.rq_psn = 0;
  rtr_attr.max_dest_rd_atomic = 1;
  rtr_attr.min_rnr_timer = 12;

  rtr_attr.ah_attr.sl = 0;
  rtr_attr.ah_attr.src_path_bits = 0;
  rtr_attr.ah_attr.port_num = 1;
  rtr_attr.ah_attr.dlid = remote_queue_pair_info.lid_;
  memcpy(&rtr_attr.ah_attr.grh.dgid, remote_queue_pair_info.gid_, 16);
  rtr_attr.ah_attr.is_global = 1;
  rtr_attr.ah_attr.grh.sgid_index = kDefaultGidIndex;
  rtr_attr.ah_attr.grh.hop_limit = 2;
  rtr_attr.ah_attr.grh.traffic_class = 0;

  if (ibv_modify_qp(this->queue_pair_, &rtr_attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
    throw std::runtime_error("Failed to modify QP to RTR " +
                             std::string(strerror(errno)));
  }
#endif

  // Create self address handle. We use local routing info for convenience,
  // so this must be done after creating the QP.
  RoutingInfo self_routing_info;
  this->_fill_local_routing_info(&self_routing_info);
  this->self_address_handle_ =
      this->_create_address_handle(
          reinterpret_cast<IbRoutingInfo*>(&self_routing_info));
  rt_assert(this->self_address_handle_ != nullptr, "Failed to create self AH.");

  // Reuse rtr_attr for RTS
  rtr_attr.qp_state = IBV_QPS_RTS;
  rtr_attr.sq_psn = 0;  // PSN does not matter for UD QPs

#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
  if (ibv_modify_qp(this->queue_pair_, &rtr_attr,
                    IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    throw std::runtime_error("Failed to modify QP to RTS");
  }
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
  rtr_attr.timeout = 14;
  rtr_attr.retry_cnt = 7;
  rtr_attr.rnr_retry = 7;
  rtr_attr.max_rd_atomic = 1;

  if (ibv_modify_qp(this->queue_pair_, &rtr_attr,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC)) {
    throw std::runtime_error("Failed to modify QP to RTS");
  }
#endif

}

/// Allocate one fixed-size RoCE buffer.
Buffer* roce_allocate_buffer(void* allocator_context) {
  auto* huge_allocator = static_cast<HugeAlloc*>(allocator_context);
  return huge_allocator->allocate(RoceDispatcher::kMbufSize);
}

/// Allocate a batch of fixed-size RoCE buffers.
uint8_t roce_allocate_buffers(void* allocator_context, Buffer** buffers,
                              size_t count) {
  auto* huge_allocator = static_cast<HugeAlloc*>(allocator_context);
  return huge_allocator->allocate_bulk(RoceDispatcher::kMbufSize, buffers,
                                       count)
             ? 0
             : static_cast<uint8_t>(-1);
}

/// Return one RoCE buffer to the allocator.
void roce_deallocate_buffer(Buffer* buffer, void* allocator_context) {
  AXIO_UNUSED(allocator_context);
  buffer->state_ = Buffer::kFree;
}

/// Return a batch of RoCE buffers to the allocator.
void roce_deallocate_buffers(Buffer** buffers, size_t count,
                             void* allocator_context) {
  AXIO_UNUSED(allocator_context);
  for (size_t i = 0; i < count; i++) {
    buffers[i]->state_ = Buffer::kFree;
  }
}

/// Set a RoCE buffer payload.
void roce_set_buffer_payload(Buffer* buffer, char* udp_header,
                             char* workspace_header, size_t payload_size) {
  buffer->length_ = sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) +
                    sizeof(WorkspaceHeader) + payload_size;
  memcpy(buffer->udp_header(), udp_header, sizeof(udphdr));
  memcpy(buffer->workspace_header(), workspace_header, sizeof(WorkspaceHeader));
  if (AXIO_UNLIKELY(payload_size == 0)) {
    return;
  }
  auto* payload = reinterpret_cast<char*>(buffer->workspace_payload());
  memset(payload, 'a', payload_size - 1);
  payload[payload_size - 1] = '\0';
}

WorkspaceHeader* roce_extract_workspace_header(Buffer* buffer) {
  return reinterpret_cast<WorkspaceHeader*>(buffer->workspace_header());
}

/// Copy payload from src to dst
void roce_copy_buffer_payload(Buffer* destination, Buffer* source,
                              char* udp_header, char* workspace_header,
                              size_t payload_size) {
  destination->length_ = sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) +
                         sizeof(WorkspaceHeader) + payload_size;

  memcpy(destination->udp_header(), udp_header, sizeof(udphdr));
  memcpy(destination->workspace_header(), workspace_header, sizeof(WorkspaceHeader));
  memcpy(destination->workspace_payload(), source->workspace_payload(),
         payload_size);
}

void RoceDispatcher::_initialize_memory_region_functions(uint8_t numa_node) {
  std::ostringstream xmsg;  // The exception message

  // Create the hugepage allocator.
  this->huge_allocator_ = new HugeAlloc(kMemoryRegionSize, numa_node);
  Buffer raw_memory_region = this->huge_allocator_->allocate_raw(
      kMemoryRegionSize, MemoryRegistration::kEnabled);
  if (raw_memory_region.buf_ == nullptr) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * kMemoryRegionSize / AXIO_MB(1) << " MiB for ring buffers. "
         << HugeAlloc::kAllocationFailureHelp;
    throw std::runtime_error(xmsg.str());
  }
  this->memory_region_ = ibv_reg_mr(
      this->protection_domain_, raw_memory_region.buf_, kMemoryRegionSize,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
          IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
  rt_assert(this->memory_region_ != nullptr, "Failed to register mr.");
  raw_memory_region.set_lkey(this->memory_region_->lkey);
  this->huge_allocator_->add_raw_buffer(raw_memory_region, kMemoryRegionSize);

  this->_initialize_receives();
  this->_initialize_sends();
  this->memory_region_info_ = new MemoryRegionInfo<Buffer>(
      this->huge_allocator_, &roce_allocate_buffer, &roce_deallocate_buffer,
      &roce_allocate_buffers, &roce_deallocate_buffers,
      &roce_set_buffer_payload, &roce_extract_workspace_header,
      &roce_copy_buffer_payload);
}

void RoceDispatcher::_initialize_receives() {
  std::ostringstream xmsg;  // The exception message

  // Initialize the memory region for RECVs.
  const size_t ring_extent_size = kReceiveQueueDepth * kMbufSize;
  assert(ring_extent_size <= HugeAlloc::kMaxClassSize);

  Buffer* ring_extent = this->huge_allocator_->allocate(ring_extent_size);
  if (ring_extent->buf_ == nullptr) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * ring_extent_size / AXIO_MB(1) << " MiB for ring buffers. "
         << HugeAlloc::kAllocationFailureHelp;
    throw std::runtime_error(xmsg.str());
  }

  // Initialize constant fields of RECV descriptors
  for (size_t i = 0; i < kReceiveQueueDepth; i++) {
    uint8_t* buffer = ring_extent->buf_;
    // Break down the memory space into fixed-length chunks.
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    // Each chunk is a buffer, and the first 64 bytes are reserved for the GRH.
    const size_t offset = (i * kMbufSize) + (64 - kGlobalRouteHeaderBytes);
    assert(offset + (kGlobalRouteHeaderBytes + kMtu) <= ring_extent_size);
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
    const size_t offset = (i * kMbufSize);
    assert(offset + kMtu <= ring_extent_size);
#endif
    this->receive_scatter_gather_[i].length = kMbufSize;
    this->receive_scatter_gather_[i].lkey = ring_extent->lkey_;
    this->receive_scatter_gather_[i].addr =
        reinterpret_cast<uint64_t>(&buffer[offset]);
    // The scatter-gather address can be used as wr_id for quick prefetching.
    this->receive_work_requests_[i].wr_id = i;
    this->receive_work_requests_[i].sg_list = &this->receive_scatter_gather_[i];
    this->receive_work_requests_[i].num_sge = 1;

#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    this->receive_ring_[i] =
        new Buffer(&buffer[offset + kGlobalRouteHeaderBytes], kMbufSize,
                   ring_extent->lkey_);
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
    this->receive_ring_[i] =
        new Buffer(&buffer[offset], kMbufSize, ring_extent->lkey_);
#endif
    this->receive_ring_[i]->state_ = Buffer::kPosted;

    // Circular link
    this->receive_work_requests_[i].next =
        (i < kReceiveQueueDepth - 1)
            ? &this->receive_work_requests_[i + 1]
            : &this->receive_work_requests_[0];
  }

  // Circularly link the receive ring.
  for (size_t i = 0; i < kReceiveQueueDepth; i++) {
    this->receive_ring_[i]->next_ =
        (i < kReceiveQueueDepth - 1) ? this->receive_ring_[i + 1]
                                    : this->receive_ring_[0];
  }

  // Fill the RECV queue. _post_receives() can use fast RECV and therefore not
  // actually fill the RQ, so _post_receives() isn't usable here.
  struct ibv_recv_wr* bad_work_request;
  this->receive_work_requests_[kReceiveQueueDepth - 1].next = nullptr;

  int ret = ibv_post_recv(this->queue_pair_,
                          &this->receive_work_requests_[0],
                          &bad_work_request);
  rt_assert(ret == 0, "Failed to fill RECV queue.");

  this->receive_work_requests_[kReceiveQueueDepth - 1].next =
      &this->receive_work_requests_[0];
}

void RoceDispatcher::_initialize_sends() {
  for (size_t i = 0; i < kSendQueueDepth; i++) {
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    this->send_work_requests_[i].wr.ud.remote_qkey = kQueueKey;
#endif
    this->send_work_requests_[i].opcode = IBV_WR_SEND;
    this->send_work_requests_[i].send_flags = IBV_SEND_SIGNALED;
    this->send_work_requests_[i].sg_list = &this->send_scatter_gather_[i];
    this->send_work_requests_[i].num_sge = 1;

    // Circularly link the send work requests.
    this->send_work_requests_[i].next =
        (i < kSendQueueDepth - 1) ? &this->send_work_requests_[i + 1]
                                  : &this->send_work_requests_[0];
  }
}

}  // namespace axio
