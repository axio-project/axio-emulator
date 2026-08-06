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
    void Workspace<TDispatcher>::_throughput_intensive_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if ApplyNewMbuf
          this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          // this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_latency_intensive_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if ApplyNewMbuf
          // this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_memory_intensive_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access
        if constexpr (kMemoryAccessRangePerPkt > 0){
          for(size_t j=0; j<kMemoryAccessRangePerPkt/sizeof(uint64_t); j++){
            this->stateful_memory_index_ += 1;
            this->stateful_memory_index_ %= (kStatefulMemorySizePerCore/sizeof(uint64_t));
            // tmp = *(static_cast<uint64_t*>(this->stateful_memory_) + this->stateful_memory_index_);
            memcpy((static_cast<uint64_t*>(this->stateful_memory_) + this->stateful_memory_index_), &this->stateful_memory_index_, sizeof(uint64_t));
          }
        }

        // [step 3] set the payload of a response with same size
        #if ApplyNewMbuf        
          // this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_fs_write(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      MEM_REG_TYPE **temp_mbuf_ptr = mbuf_ptr;
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*temp_mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp);
        if constexpr (kMemoryAccessRangePerPkt > 0){
          this->stateful_memory_index_ += 1;
          this->stateful_memory_index_ %= (kStatefulMemorySizePerCore / Dispatcher::kMTU);
        #ifdef DpdkMode
          memcpy(static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMTU,
                mbuf_ws_payload(*temp_mbuf_ptr), Dispatcher::kMTU);
        #else
          memcpy(static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMTU,
                (*temp_mbuf_ptr)->get_ws_payload(), Dispatcher::kMTU);
        #endif
        }
        temp_mbuf_ptr++;
      }
      for (size_t i = 0; i < msg_num; i++) {
        // [step 3] set response payload
        #if ApplyNewMbuf
          this->_write_payload(this->tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          mbuf_ptr++;
        #endif
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_fs_read(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < msg_num; i++) {
        // [step 1] scan the payload of the request
        // this->_scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp) and set response payload;
        for (size_t j = 0; j < kAppResponsePktsNum; j++) {
          MEM_REG_TYPE *temp_mbuf_ptr = this->tx_mbuf_buffer_[i * kAppResponsePktsNum + j];
          if constexpr (kMemoryAccessRangePerPkt > 0){
            this->stateful_memory_index_ += 1;
            this->stateful_memory_index_ %= (kStatefulMemorySizePerCore / Dispatcher::kMTU);
            /// set header
            this->_write_payload(temp_mbuf_ptr, (char*)uh, (char*)hdr, 0);
          #ifdef DpdkMode
            mbuf_push_data(temp_mbuf_ptr, kAppRespFullPaddingSize);
            /// set payload
            char *payload_ptr = mbuf_ws_payload(temp_mbuf_ptr);
            memcpy(payload_ptr, static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMTU, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #else
            temp_mbuf_ptr->length_ += kAppRespFullPaddingSize;
            /// set payload
            uint8_t *payload_ptr = temp_mbuf_ptr->get_ws_payload();
            memcpy(payload_ptr, static_cast<uint8_t*>(this->stateful_memory_) + this->stateful_memory_index_ * Dispatcher::kMTU, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #endif
          }
        }
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::_handle_kv(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        uint8_t type;
        this->_read_payload(*mbuf_ptr, 0, (char*)&type, 1);
        // if(type) { // kv get
        //   KV::key_t key;
        //   this->_read_payload(*mbuf_ptr, 1, (char*)key.key, KV::kKeySize);
        //   std::optional<KV::value_t> value = this->kv_store_->get(key);

        //   #if ApplyNewMbuf
        //     this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        //   #else
        //     this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        //   #endif
        //   mbuf_ptr++;
        // } else { //kv put
          KV::key_t key;
          KV::value_t value;
          this->_read_payload(*mbuf_ptr, 1, (char*)key.key, KV::kKeySize);
          this->_read_payload(*mbuf_ptr, 1 + KV::kKeySize, (char*)value.value, KV::kValueSize);
          this->kv_store_->put_test(key,value);

          #if ApplyNewMbuf
            this->_copy_payload(this->tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          #else
            this->_write_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          #endif
          mbuf_ptr++;
        // }
      }
    }
  /**
   * @brief message handler wrapper
   */
  template <class TDispatcher>
  template <msg_handler_type_t handler>
  void Workspace<TDispatcher>::_handle_server_messages(MEM_REG_TYPE** msg, size_t msg_num) {
    udphdr uh;
    ws_hdr hdr;
    size_t drop_num = 0;
    size_t pkt_num = msg_num * kAppRequestPktsNum;
    size_t resp_pkt_num = msg_num * kAppResponsePktsNum;
    // printf("Recv %lu messages, %lu packets, need to generate %lu packets\n", msg_num, msg_num * kAppRequestPktsNum, resp_pkt_num);
    MEM_REG_TYPE **mbuf_ptr = msg;
  
    // set UDP header of the response
    uh.source = this->ws_id_;
    uh.dest = this->tx_rule_table_->select_next(this->workload_type_);
    
    // set workspace header of the response
    hdr.workload_type_ = this->workload_type_;
    hdr.segment_num_ = kAppResponsePktsNum;

    // ------------------Begin of the message handler------------------
  #if ApplyNewMbuf
    while (unlikely(this->_allocate_bulk(this->tx_mbuf_buffer_, resp_pkt_num) != 0)) {
      net_stats_app_apply_mbuf_stalls();
    }
  #endif
    if constexpr (handler == kRxMsgHandler_Empty) {return;}
    else if (handler == kRxMsgHandler_T_APP) this->_throughput_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_L_APP) this->_latency_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_M_APP) this->_memory_intensive_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_FS_WRITE) this->_fs_write(mbuf_ptr, msg_num, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_FS_READ) this->_fs_read(mbuf_ptr, msg_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_KV) this->_handle_kv(mbuf_ptr, pkt_num, &uh, &hdr);
    else {AXIO_ERROR("Invalid message handler type!");}
    // ------------------End of the message handler------------------
  #if ApplyNewMbuf
    this->_deallocate_bulk(msg, pkt_num);
    mbuf_ptr = this->tx_mbuf_buffer_;
  #else
    mbuf_ptr = msg;
  #endif
    /// Insert packets to worker tx queue
    for (size_t i = 0; i < resp_pkt_num; i++) {
      if (unlikely(!this->tx_queue_->enqueue((uint8_t*)(*mbuf_ptr)))) {
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
    net_stats_app_drops(drop_num);
  }

// force compile
#ifdef RoceMode
  template void Workspace<RoceDispatcher>::_handle_server_messages<kRxMsgHandler>(MEM_REG_TYPE** msg, size_t msg_num);
#elif DpdkMode
  template void Workspace<DpdkDispatcher>::_handle_server_messages<kRxMsgHandler>(MEM_REG_TYPE** msg, size_t msg_num);
#endif

}
