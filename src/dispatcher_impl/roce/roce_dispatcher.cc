/**
 * @file roce_dispatcher.cc
 * @brief Transmit / Receive packets with a RoCE NIC (CX5 / CX7)
 */
#include "roce_dispatcher.h"
#include "ws_impl/workspace_header.h"

#include <cerrno>
#include <cstring>
#include <mutex>

namespace axio {

namespace {

std::mutex verbs_initialization_mutex;

std::string verbs_creation_error(const char* resource) {
  return std::string("Failed to create ") + resource + ": " +
         std::strerror(errno);
}

bool requires_concurrent_buffer_access(uint8_t dispatcher_id,
                                       const UserConfig& user_config) {
  const config::ValidatedTopology& topology = user_config.topology();
  for (const config::WorkspaceId application_id :
       topology.active_workspace_ids()) {
    if (!config::has_role(topology.roles(application_id),
                          config::WorkspaceRole::kApplication)) {
      continue;
    }
    if (topology.application_owner(application_id).dispatcher.value() ==
            dispatcher_id &&
        application_id.value() != dispatcher_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

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
static constexpr uint16_t kStartSynchronizationPort =
    Dispatcher::kDefaultMngtPort + kWorkspaceMaxNum;
static constexpr char kReadyMessage[] = "ready";
static constexpr char kStartMessage[] = "start";
static constexpr char kStopReadyMessage[] = "stop-ready";
static constexpr char kStopMessage[] = "stop";

// Initialize the protection domain, queue pair, and memory registration
// and deregistration functions. RECVs will be initialized later
// when the hugepage allocator is provided.

RoceDispatcher::RoceDispatcher(uint8_t workspace_id, uint8_t physical_port,
                               size_t numa_node, UserConfig* user_config)
    : Dispatcher(DispatcherType::kRoce, workspace_id, physical_port, numa_node,
                 user_config) {
  this->_initialize_local_verbs_resources(
      user_config->server().device_name_, physical_port);

  // Initialize the IP and MAC addresses.
  parse_ip_address(&this->resolved_port_.ipv4_addr_, this->local_ip());
  memcpy(this->resolved_port_.mac_addr_, user_config->server().local_mac_, 6);
  this->destination_ip_ = new IpAddress;
  parse_ip_address(this->destination_ip_, this->remote_ip());

  this->_connect_queue_pair(workspace_id);
  this->_initialize_memory_region_functions(
      numa_node, requires_concurrent_buffer_access(workspace_id, *user_config));

  AXIO_INFO("RoceDispatcher is initialized\n");
}

RoceDispatcher::~RoceDispatcher() {
  AXIO_INFO("Destroying dispatcher for Qp %lu\n", this->queue_pair_id_);

#if AXIO_NODE_TYPE == AXIO_SERVER
  delete this->management_server_;
  this->management_server_ = nullptr;
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  delete this->management_client_;
  this->management_client_ = nullptr;
#endif

  delete this->memory_region_info_;
  delete this->buffer_pool_;

  for (size_t i = 0; i < kReceiveQueueDepth; i++) {
    delete this->receive_ring_[i];
  }

  const size_t registered_bytes = this->memory_region_->length;
  const uint32_t local_key = this->memory_region_->lkey;
  const int deregistration_result = ibv_dereg_mr(this->memory_region_);
  if (deregistration_result != 0) {
    AXIO_ERROR("Memory deregistration failed. size %zu B, lkey %u\n",
               registered_bytes, local_key);
  }
  AXIO_INFO("Deregistered %zu MiB (lkey = %u)\n",
            registered_bytes / AXIO_MB(1), local_key);
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

  delete this->destination_ip_;
}

void RoceDispatcher::synchronize_peer_start() {
#if AXIO_NODE_TYPE == AXIO_SERVER
  rt_assert(this->management_server_ == nullptr,
            "Peer synchronization server already exists");
  this->management_server_ = new TcpServer(kStartSynchronizationPort);
  this->management_server_->accept_connection();
  rt_assert(this->management_server_->receive_message() == kReadyMessage,
            "Invalid peer measurement-ready message");
  this->management_server_->send_message(kStartMessage);
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  rt_assert(this->management_client_ == nullptr,
            "Peer synchronization client already exists");
  this->management_client_ = new TcpClient();
  this->management_client_->connect_to_server(this->remote_ip(),
                                              kStartSynchronizationPort);
  this->management_client_->send_message(kReadyMessage);
  rt_assert(this->management_client_->receive_message() == kStartMessage,
            "Invalid peer measurement-start message");
#endif
}

void RoceDispatcher::synchronize_peer_stop() {
#if AXIO_NODE_TYPE == AXIO_SERVER
  rt_assert(this->management_server_ != nullptr,
            "Peer synchronization server is unavailable");
  rt_assert(this->management_server_->receive_message() == kStopReadyMessage,
            "Invalid peer measurement-stop-ready message");
  this->management_server_->send_message(kStopMessage);
  this->management_server_->disconnect();
  delete this->management_server_;
  this->management_server_ = nullptr;
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  rt_assert(this->management_client_ != nullptr,
            "Peer synchronization client is unavailable");
  this->management_client_->send_message(kStopReadyMessage);
  rt_assert(this->management_client_->receive_message() == kStopMessage,
            "Invalid peer measurement-stop message");
  this->management_client_->disconnect();
  delete this->management_client_;
  this->management_client_ = nullptr;
#endif
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
  if (ret != 0) {
    throw std::runtime_error("Failed to query GID: " +
                             std::string(std::strerror(errno)));
  }
  // Validate GID
  if (gid_entry.gid_type != IBV_GID_TYPE_ROCE_V2) {
    xmsg << "Invalid GID type: expected RoCE v2, got " << gid_entry.gid_type;
    throw std::runtime_error(xmsg.str());
  }
  // Copy GID
  memcpy(&this->resolved_port_.gid_, &gid_entry.gid, sizeof(union ibv_gid));
  this->resolved_port_.gid_index_ = gid_entry.gid_index;
}

void RoceDispatcher::_release_partial_local_verbs_resources() {
  if (this->self_address_handle_ != nullptr) {
    ibv_destroy_ah(this->self_address_handle_);
    this->self_address_handle_ = nullptr;
  }
  if (this->queue_pair_ != nullptr) {
    ibv_destroy_qp(this->queue_pair_);
    this->queue_pair_ = nullptr;
  }
  if (this->send_completion_queue_ != nullptr) {
    ibv_destroy_cq(this->send_completion_queue_);
    this->send_completion_queue_ = nullptr;
  }
  if (this->receive_completion_queue_ != nullptr) {
    ibv_destroy_cq(this->receive_completion_queue_);
    this->receive_completion_queue_ = nullptr;
  }
  if (this->protection_domain_ != nullptr) {
    ibv_dealloc_pd(this->protection_domain_);
    this->protection_domain_ = nullptr;
  }
  if (this->resolved_port_.context_ != nullptr) {
    ibv_close_device(this->resolved_port_.context_);
    this->resolved_port_.context_ = nullptr;
  }
}

void RoceDispatcher::_initialize_local_verbs_resources(
    const char* device_name, uint8_t physical_port) {
  // Some mlx5 providers transiently fail concurrent device/CQ operations.
  // The peer handshake is deliberately performed later, outside this lock.
  const std::lock_guard<std::mutex> lock(verbs_initialization_mutex);
  try {
    resolve_verbs_port(device_name, physical_port, kMtu,
                       this->resolved_port_);
    this->_resolve_roce_port();

    this->protection_domain_ = ibv_alloc_pd(this->resolved_port_.context_);
    if (this->protection_domain_ == nullptr) {
      throw std::runtime_error(verbs_creation_error("protection domain"));
    }

    this->send_completion_queue_ = ibv_create_cq(
        this->resolved_port_.context_, kSendQueueDepth, nullptr, nullptr, 0);
    if (this->send_completion_queue_ == nullptr) {
      throw std::runtime_error(verbs_creation_error("SEND CQ"));
    }

    this->receive_completion_queue_ = ibv_create_cq(
        this->resolved_port_.context_, kReceiveQueueDepth, nullptr, nullptr, 0);
    if (this->receive_completion_queue_ == nullptr) {
      throw std::runtime_error(verbs_creation_error("RECV CQ"));
    }

    struct ibv_qp_init_attr create_attr;
    memset(static_cast<void*>(&create_attr), 0,
           sizeof(struct ibv_qp_init_attr));
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
    if (this->queue_pair_ == nullptr) {
      throw std::runtime_error(verbs_creation_error("queue pair"));
    }
    this->queue_pair_id_ = this->queue_pair_->qp_num;

    struct ibv_qp_attr init_attr;
    memset(static_cast<void*>(&init_attr), 0, sizeof(struct ibv_qp_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.pkey_index = 0;
    init_attr.port_num = static_cast<uint8_t>(this->resolved_port_.port_id_);
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    init_attr.qkey = kQueueKey;
    const int attr_mask =
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
    init_attr.qp_access_flags =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    const int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS;
#endif
    if (ibv_modify_qp(this->queue_pair_, &init_attr, attr_mask) != 0) {
      throw std::runtime_error("Failed to modify QP to INIT: " +
                               std::string(std::strerror(errno)));
    }

    RoutingInfo self_routing_info;
    this->_fill_local_routing_info(&self_routing_info);
    this->self_address_handle_ = this->_create_address_handle(
        reinterpret_cast<IbRoutingInfo*>(&self_routing_info));
    if (this->self_address_handle_ == nullptr) {
      throw std::runtime_error(verbs_creation_error("self address handle"));
    }
  } catch (...) {
    this->_release_partial_local_verbs_resources();
    throw;
  }
}

void RoceDispatcher::_connect_queue_pair(uint8_t workspace_id) {
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
  auto* buffer_pool = static_cast<RoceBufferPool*>(allocator_context);
  return buffer_pool->allocate();
}

/// Allocate a batch of fixed-size RoCE buffers.
uint8_t roce_allocate_buffers(void* allocator_context, Buffer** buffers,
                              size_t count) {
  auto* buffer_pool = static_cast<RoceBufferPool*>(allocator_context);
  return buffer_pool->allocate_bulk(buffers, count)
             ? 0
             : static_cast<uint8_t>(-1);
}

/// Return one RoCE buffer to the allocator.
void roce_deallocate_buffer(Buffer* buffer, void* allocator_context) {
  auto* buffer_pool = static_cast<RoceBufferPool*>(allocator_context);
  if (AXIO_UNLIKELY(buffer_pool->owns(buffer))) {
    buffer_pool->free(buffer);
  } else {
    buffer->mark_free();
  }
}

/// Return a batch of RoCE buffers to the allocator.
void roce_deallocate_buffers(Buffer** buffers, size_t count,
                             void* allocator_context) {
  if (count == 0) return;
  auto* buffer_pool = static_cast<RoceBufferPool*>(allocator_context);
  const bool pool_owned = buffer_pool->owns(buffers[0]);
  if (AXIO_UNLIKELY(pool_owned)) {
    buffer_pool->free_bulk(buffers, count);
    return;
  }
  for (size_t index = 0; index < count; ++index) {
    assert(!buffer_pool->owns(buffers[index]));
    buffers[index]->mark_free();
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

void RoceDispatcher::_initialize_memory_region_functions(
    uint8_t numa_node, bool concurrent_buffer_access) {
  std::ostringstream xmsg;  // The exception message

  // Create the hugepage allocator.
  this->huge_allocator_ = new HugeAlloc(numa_node);
  RegisteredMemorySlice raw_memory_region =
      this->huge_allocator_->reserve(kMemoryRegionSize);
  if (!raw_memory_region) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * kMemoryRegionSize / AXIO_MB(1) << " MiB for ring buffers. "
         << HugeAlloc::kAllocationFailureHelp;
    throw std::runtime_error(xmsg.str());
  }
  this->memory_region_ = ibv_reg_mr(
      this->protection_domain_, raw_memory_region.data_,
      raw_memory_region.size_,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
          IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
  rt_assert(this->memory_region_ != nullptr, "Failed to register mr.");
  raw_memory_region.lkey_ = this->memory_region_->lkey;
  this->huge_allocator_->add_registered_region(raw_memory_region);

  const size_t ring_extent_size = kReceiveQueueDepth * kMbufSize;
  RegisteredMemorySlice ring_extent =
      this->huge_allocator_->allocate(ring_extent_size, kMbufSize);
  if (!ring_extent) {
    xmsg << "Failed to allocate " << std::setprecision(2)
         << 1.0 * ring_extent_size / AXIO_MB(1)
         << " MiB for RX ring buffers. "
         << HugeAlloc::kAllocationFailureHelp;
    throw std::runtime_error(xmsg.str());
  }
  this->_initialize_receives(ring_extent);
  this->_initialize_sends();

  const RoceBufferPoolAccess pool_access =
      concurrent_buffer_access ? RoceBufferPoolAccess::kShared
                               : RoceBufferPoolAccess::kLocal;
  this->buffer_pool_ =
      new RoceBufferPool(this->huge_allocator_, kMbufSize, pool_access);
  this->memory_region_info_ = new MemoryRegionInfo<Buffer>(
      this->buffer_pool_, &roce_allocate_buffer, &roce_deallocate_buffer,
      &roce_allocate_buffers, &roce_deallocate_buffers,
      &roce_set_buffer_payload, &roce_extract_workspace_header,
      &roce_copy_buffer_payload);
}

void RoceDispatcher::_initialize_receives(
    RegisteredMemorySlice ring_extent) {
  assert(ring_extent);
  assert(ring_extent.size_ == kReceiveQueueDepth * kMbufSize);
  // Initialize constant fields of RECV descriptors
  for (size_t i = 0; i < kReceiveQueueDepth; i++) {
    uint8_t* buffer = ring_extent.data_;
    // Break down the memory space into fixed-length chunks.
#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    // Each chunk is a buffer, and the first 64 bytes are reserved for the GRH.
    const size_t offset = (i * kMbufSize) + (64 - kGlobalRouteHeaderBytes);
    assert(offset + (kGlobalRouteHeaderBytes + kMtu) <= ring_extent.size_);
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
    const size_t offset = (i * kMbufSize);
    assert(offset + kMtu <= ring_extent.size_);
#endif
    this->receive_scatter_gather_[i].length = kMbufSize;
    this->receive_scatter_gather_[i].lkey = ring_extent.lkey_;
    this->receive_scatter_gather_[i].addr =
        reinterpret_cast<uint64_t>(&buffer[offset]);
    // The scatter-gather address can be used as wr_id for quick prefetching.
    this->receive_work_requests_[i].wr_id = i;
    this->receive_work_requests_[i].sg_list = &this->receive_scatter_gather_[i];
    this->receive_work_requests_[i].num_sge = 1;

#if AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD
    this->receive_ring_[i] =
        new Buffer(&buffer[offset + kGlobalRouteHeaderBytes], kMbufSize,
                   ring_extent.lkey_);
#elif AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
    this->receive_ring_[i] =
        new Buffer(&buffer[offset], kMbufSize, ring_extent.lkey_);
#endif
    this->receive_ring_[i]->mark_posted();

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
