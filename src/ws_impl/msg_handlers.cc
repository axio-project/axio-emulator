/**
 * @brief user defined message handler for emulation
 */
#include "workspace.h"
#include "util/kv.h"

namespace axio {
  /**
   * @brief message handler kernel
   */
    template <class TDispatcher>
    void Workspace<TDispatcher>::_throughput_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, WorkspaceHeader *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if AXIO_APPLY_NEW_BUFFER
          this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          // this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_latency_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, WorkspaceHeader *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if AXIO_APPLY_NEW_BUFFER
          // this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_memory_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, WorkspaceHeader *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access
        this->stateful_memory_index_ = this->memory_workload_->read_modify_write(
            static_cast<uint8_t*>(this->stateful_memory_));

        // [step 3] set the payload of a response with same size
        #if AXIO_APPLY_NEW_BUFFER
          // this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_fs_write(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t msg_num, size_t pkt_num, udphdr *uh, WorkspaceHeader *hdr) {
      AXIO_MEMORY_BUFFER_TYPE **temp_mbuf_ptr = mbuf_ptr;
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*temp_mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp);
        if constexpr (kMemoryAccessRangePerPkt > 0){
          this->stateful_memory_index_ += 1;
          this->stateful_memory_index_ %=
              (kFileStatefulMemorySizePerCore / Dispatcher::kMtu);
        #if AXIO_DPDK_MODE
          memcpy(static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMtu,
                AXIO_MBUF_WORKSPACE_PAYLOAD(*temp_mbuf_ptr), Dispatcher::kMtu);
        #else
          memcpy(static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMtu,
                (*temp_mbuf_ptr)->workspace_payload(), Dispatcher::kMtu);
        #endif
        }
        temp_mbuf_ptr++;
      }
      for (size_t i = 0; i < msg_num; i++) {
        // [step 3] set response payload
        #if AXIO_APPLY_NEW_BUFFER
          this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          mbuf_ptr++;
        #endif
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_fs_read(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t msg_num, udphdr *uh, WorkspaceHeader *hdr) {
      for (size_t i = 0; i < msg_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp) and set response payload;
        for (size_t j = 0; j < kAppResponsePktsNum; j++) {
          AXIO_MEMORY_BUFFER_TYPE *temp_mbuf_ptr = this->tx_mbuf_buffer_[i * kAppResponsePktsNum + j];
          if constexpr (kMemoryAccessRangePerPkt > 0){
            this->stateful_memory_index_ += 1;
            this->stateful_memory_index_ %=
                (kFileStatefulMemorySizePerCore / Dispatcher::kMtu);
            /// set header
            this->_write_payload(temp_mbuf_ptr, (char*)uh, (char*)hdr, 0);
          #if AXIO_DPDK_MODE
            AXIO_MBUF_APPEND_DATA(temp_mbuf_ptr, kAppRespFullPaddingSize);
            /// set payload
            char *payload_ptr = AXIO_MBUF_WORKSPACE_PAYLOAD(temp_mbuf_ptr);
            memcpy(payload_ptr, static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMtu, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #else
            temp_mbuf_ptr->length_ += kAppRespFullPaddingSize;
            /// set payload
            uint8_t *payload_ptr = temp_mbuf_ptr->workspace_payload();
            memcpy(payload_ptr, static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMtu, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #endif
          }
        }
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_handle_kv(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, WorkspaceHeader *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        uint8_t type;
        this->_read_payload(*mbuf_ptr, 0, (char*)&type, 1);
        KeyValueStore::Key requested_key;
        this->_read_payload(*mbuf_ptr, 1, (char*)requested_key.bytes_,
                            KeyValueStore::kKeySize);
        const KeyValueStore::Key local_key =
            this->key_value_store_->resolve_local_key(requested_key);
        if (type != 0) {
          static_cast<void>(this->key_value_store_->get(local_key));
        } else {
          KeyValueStore::Value value;
          this->_read_payload(*mbuf_ptr, 1 + KeyValueStore::kKeySize,
                              (char*)value.bytes_,
                              KeyValueStore::kValueSize);
          this->key_value_store_->put(local_key, value);
        }

        #if AXIO_APPLY_NEW_BUFFER
          this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }
  /**
   * @brief message handler wrapper
   */
  template <class TDispatcher>
  template <MessageHandlerType handler>
  void Workspace<TDispatcher>::_handle_server_messages(AXIO_MEMORY_BUFFER_TYPE** msg, size_t msg_num) {
    udphdr uh;
    WorkspaceHeader hdr;
    size_t drop_num = 0;
    size_t pkt_num = msg_num * kAppRequestPktsNum;
    size_t resp_pkt_num = msg_num * kAppResponsePktsNum;
    // printf("Recv %lu messages, %lu packets, need to generate %lu packets\n", msg_num, msg_num * kAppRequestPktsNum, resp_pkt_num);
    AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr = msg;
  
    // set UDP header of the response
    uh.source = this->ws_id_;
    uh.dest = this->tx_rule_table_->select_next(this->workload_type_);
    
    // set workspace header of the response
    hdr.workload_type_ = this->workload_type_;
    hdr.segment_num_ = kAppResponsePktsNum;

    // ------------------Begin of the message handler------------------
  #if AXIO_APPLY_NEW_BUFFER
    while (AXIO_UNLIKELY(this->_allocate_bulk(this->tx_mbuf_buffer_, resp_pkt_num) != 0)) {
      AXIO_RECORD_APP_MBUF_STALL();
    }
  #endif
    if constexpr (handler == kMessageHandlerEmpty) {return;}
    else if (handler == kMessageHandlerThroughput) this->_throughput_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kMessageHandlerLatency) this->_latency_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kMessageHandlerMemory) this->_memory_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kMessageHandlerFileWrite) this->_fs_write(mbuf_ptr, msg_num, pkt_num, &uh, &hdr);
    else if (handler == kMessageHandlerFileRead) this->_fs_read(mbuf_ptr, msg_num, &uh, &hdr);
    else if (handler == kMessageHandlerKeyValue) this->_handle_kv(mbuf_ptr, pkt_num, &uh, &hdr);
    else {AXIO_ERROR("Invalid message handler type!");}
    // ------------------End of the message handler------------------
  #if AXIO_APPLY_NEW_BUFFER
    this->_deallocate_bulk(msg, pkt_num);
    mbuf_ptr = this->tx_mbuf_buffer_;
  #else
    mbuf_ptr = msg;
  #endif
    /// Insert packets to worker tx queue
    for (size_t i = 0; i < resp_pkt_num; i++) {
      if (AXIO_UNLIKELY(!this->tx_queue_->enqueue((uint8_t*)(*mbuf_ptr)))) {
        /// Drop the packet if the tx queue is full
        this->_deallocate(*mbuf_ptr);
        drop_num++;
      }
      mbuf_ptr++;
    }
    if (pkt_num > resp_pkt_num) {
      /// Drop the remaining packets
      this->_deallocate_bulk(mbuf_ptr, pkt_num - resp_pkt_num);
    }
    AXIO_RECORD_APP_DROP(drop_num);
  }

// force compile
#if AXIO_ROCE_MODE
  template void Workspace<RoceDispatcher>::_handle_server_messages<AXIO_RX_MESSAGE_HANDLER>(AXIO_MEMORY_BUFFER_TYPE** msg, size_t msg_num);
#elif AXIO_DPDK_MODE
  template void Workspace<DpdkDispatcher>::_handle_server_messages<AXIO_RX_MESSAGE_HANDLER>(AXIO_MEMORY_BUFFER_TYPE** msg, size_t msg_num);
#endif

}
