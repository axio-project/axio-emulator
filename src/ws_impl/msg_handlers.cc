/**
 * @brief user defined message handler for emulation
 */
#include "workspace.h"

namespace dperf {
  /**
   * @brief message handler kernel
   */
    template <class TDispatcher>
    void Workspace<TDispatcher>::throughput_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // #if RhyR_CC
        //   RhyR::RhyR_server_process_req(reinterpret_cast<char*>((*mbuf_ptr)->get_buf()));
        //   RhyR::RhyR_server_process_begin(kAppReqPayloadSize + 14 + sizeof(struct iphdr) + sizeof(struct udphdr));
        // #endif
        // usleep(1);
        // std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 1ms, thp 0.001 mpps; no sleep, thp 6.5 mpps, thus app time is 1/6.5 us
        // std::this_thread::sleep_for(std::chrono::microseconds(1)); // 1us, thp 0.019 mpps

        // [step 1] scan the payload of the request
        // scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if ApplyNewMbuf
          set_payload(tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          // cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
        // #if RhyR_CC
        //   RhyR::RhyR_server_process_end();
        // #endif
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::latency_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] set the payload of a response with same size
        #if ApplyNewMbuf
          // set_payload(tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::memory_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access
        if constexpr (kMemoryAccessRangePerPkt > 0){
          for(size_t j=0; j<kMemoryAccessRangePerPkt/sizeof(uint64_t); j++){
            stateful_memory_access_ptr_ += 1;
            stateful_memory_access_ptr_ %= (kStatefulMemorySizePerCore/sizeof(uint64_t));
            // tmp = *(static_cast<uint64_t*>(stateful_memory_) + stateful_memory_access_ptr_);
            memcpy((static_cast<uint64_t*>(stateful_memory_) + stateful_memory_access_ptr_), &stateful_memory_access_ptr_, sizeof(uint64_t));
          }
        }

        // [step 3] set the payload of a response with same size
        #if ApplyNewMbuf        
          // set_payload(tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
          cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #endif
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::fs_write(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      MEM_REG_TYPE **temp_mbuf_ptr = mbuf_ptr;
      for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // scan_payload(*temp_mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp);
        if constexpr (kMemoryAccessRangePerPkt > 0){
          stateful_memory_access_ptr_ += 1;
          stateful_memory_access_ptr_ %= (kStatefulMemorySizePerCore / Dispatcher::kMTU);
        #ifdef DpdkMode
          memcpy(static_cast<uint8_t*>(stateful_memory_) + stateful_memory_access_ptr_ * Dispatcher::kMTU,
                mbuf_ws_payload(*temp_mbuf_ptr), Dispatcher::kMTU);
        #else
          memcpy(static_cast<uint8_t*>(stateful_memory_) + stateful_memory_access_ptr_ * Dispatcher::kMTU,
                (*temp_mbuf_ptr)->get_ws_payload(), Dispatcher::kMTU);
        #endif
        }
        temp_mbuf_ptr++;
      }
      for (size_t i = 0; i < msg_num; i++) {
        // [step 3] set response payload
        #if ApplyNewMbuf
          set_payload(tx_mbuf_buffer_[i], (char*)uh, (char*)hdr, kAppRespPayloadSize);
        #else
          set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          mbuf_ptr++;
        #endif
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::fs_read(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < msg_num; i++) {
        // [step 1] scan the payload of the request
        // scan_payload(*mbuf_ptr, kAppReqPayloadSize);

        // [step 2] conduct external memory access(local memcp) and set response payload;
        for (size_t j = 0; j < kAppReponsePktsNum; j++) {
          MEM_REG_TYPE *temp_mbuf_ptr = tx_mbuf_buffer_[i * kAppReponsePktsNum + j];
          if constexpr (kMemoryAccessRangePerPkt > 0){
            stateful_memory_access_ptr_ += 1;
            stateful_memory_access_ptr_ %= (kStatefulMemorySizePerCore / Dispatcher::kMTU);
            /// set header
            set_payload(temp_mbuf_ptr, (char*)uh, (char*)hdr, 0);
          #ifdef DpdkMode
            mbuf_push_data(temp_mbuf_ptr, kAppRespFullPaddingSize);
            /// set payload
            char *payload_ptr = mbuf_ws_payload(temp_mbuf_ptr);
            memcpy(payload_ptr, static_cast<uint8_t*>(stateful_memory_) + stateful_memory_access_ptr_ * Dispatcher::kMTU, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #else
            temp_mbuf_ptr->length_ += kAppRespFullPaddingSize;
            /// set payload
            uint8_t *payload_ptr = temp_mbuf_ptr->get_ws_payload();
            memcpy(payload_ptr, static_cast<uint8_t*>(stateful_memory_) + stateful_memory_access_ptr_ * Dispatcher::kMTU, kAppRespFullPaddingSize);
            payload_ptr[kAppRespFullPaddingSize] = '\0';
          #endif
          }
        }
        mbuf_ptr++;
      }
    }

    template <class TDispatcher>
    void Workspace<TDispatcher>::kv_handler(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
      for (size_t i = 0; i < pkt_num; i++) {
        uint8_t type;
        get_payload(*mbuf_ptr, 0, (char*)&type, 1);
        // if(type) { // kv get
        //   KV::key_t key;
        //   get_payload(*mbuf_ptr, 1, (char*)key.key, KV::kKeySize);
        //   std::optional<KV::value_t> value = kv->get(key);

        //   #if ApplyNewMbuf
        //     cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        //   #else
        //     set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
        //   #endif
        //   mbuf_ptr++;
        // } else { //kv put
          KV::key_t key;
          KV::value_t value;
          get_payload(*mbuf_ptr, 1, (char*)key.key, KV::kKeySize);
          get_payload(*mbuf_ptr, 1 + KV::kKeySize, (char*)value.value, KV::kValueSize);
          kv->put_test(key,value);

          #if ApplyNewMbuf
            cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
          #else
            set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppRespPayloadSize);
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
  void Workspace<TDispatcher>::msg_handler_server(MEM_REG_TYPE** msg, size_t msg_num) {
    udphdr uh;
    ws_hdr hdr;
    size_t drop_num = 0;
    size_t pkt_num = msg_num * kAppRequestPktsNum;
    size_t resp_pkt_num = msg_num * kAppReponsePktsNum;
    // printf("Recv %lu messages, %lu packets, need to generate %lu packets\n", msg_num, msg_num * kAppRequestPktsNum, resp_pkt_num);
    MEM_REG_TYPE **mbuf_ptr = msg;
  
    // set UDP header of the response
    uh.source = ws_id_;
    uh.dest = tx_rule_table_->rr_select(workload_type_);
    
    // set workspace header of the response
    hdr.workload_type_ = workload_type_;
    hdr.segment_num_ = kAppReponsePktsNum;

    // ------------------Begin of the message handler------------------
  #if ApplyNewMbuf
    while (unlikely(alloc_bulk(tx_mbuf_buffer_, resp_pkt_num) != 0)) {
      net_stats_app_apply_mbuf_stalls();
    }
  #endif
    if constexpr (handler == kRxMsgHandler_Empty) {return;}
    else if (handler == kRxMsgHandler_T_APP) this->throughput_intense_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_L_APP) this->latency_intense_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_M_APP) this->memory_intense_app(mbuf_ptr, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_FS_WRITE) this->fs_write(mbuf_ptr, msg_num, pkt_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_FS_READ) this->fs_read(mbuf_ptr, msg_num, &uh, &hdr);
    else if (handler == kRxMsgHandler_KV) this->kv_handler(mbuf_ptr, pkt_num, &uh, &hdr);
    else {DPERF_ERROR("Invalid message handler type!");}
    // ------------------End of the message handler------------------
  #if ApplyNewMbuf
    de_alloc_bulk(msg, pkt_num);
    mbuf_ptr = tx_mbuf_buffer_;
  #else
    mbuf_ptr = msg;
  #endif
    /// Insert packets to worker tx queue
    for (size_t i = 0; i < resp_pkt_num; i++) {
      if (unlikely(!tx_queue_->enqueue((uint8_t*)(*mbuf_ptr)))) {
        /// Drop the packet if the tx queue is full
        de_alloc(*mbuf_ptr);
        drop_num++;
      }
      mbuf_ptr++;
    }
    if (pkt_num > resp_pkt_num) {
      /// Drop the remaining packets
      de_alloc_bulk(mbuf_ptr, pkt_num - resp_pkt_num);
    }
    net_stats_app_drops(drop_num);
  }

// force compile
#ifdef RoceMode
  template void Workspace<RoceDispatcher>::msg_handler_server<kRxMsgHandler>(MEM_REG_TYPE** msg, size_t msg_num);
#elif DpdkMode
  template void Workspace<DpdkDispatcher>::msg_handler_server<kRxMsgHandler>(MEM_REG_TYPE** msg, size_t msg_num);
#endif

}