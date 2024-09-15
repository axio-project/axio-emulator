/**
 * @brief user defined message handler for emulation
 */
#include "workspace.h"

namespace dperf {
  /**
   * @brief message handler kernel
   */
    template <class TDispatcher>
    void Workspace<TDispatcher>::T_APP(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr) {
        for (size_t i = 0; i < pkt_num; i++) {
        // [step 1] scan the payload of the request
        // scan_payload(*mbuf_ptr, kAppPayloadSize);

        // [step 2] set the payload of a response with same size
        #if ApplyNewMbuf
            // set_payload(tx_mbuf_buffer_[i], (char*)&uh, (char*)&hdr, kAppPayloadSize);
            cp_payload(tx_mbuf_buffer_[i], *mbuf_ptr, (char*)uh, (char*)hdr, kAppPayloadSize);
            mbuf_ptr++;
        #else
            set_payload(*mbuf_ptr, (char*)uh, (char*)hdr, kAppPayloadSize);
            mbuf_ptr++;
        #endif
        }
    }

  /**
   * @brief message handler wrapper
   */
  template <class TDispatcher>
  template <msg_handler_type_t handle>
  void Workspace<TDispatcher>::msg_handler_server(MEM_REG_TYPE** msg, size_t pkt_num) {
    uint64_t i, j;
    udphdr uh;
    ws_hdr hdr;
    size_t drop_num = 0;
    MEM_REG_TYPE **mbuf_ptr = msg;
  
    // set UDP header of the response
    uh.source = ws_id_;
    uh.dest = tx_rule_table_->rr_select(workload_type_);
    
    // set workspace header of the response
    hdr.workload_type_ = workload_type_;
    hdr.segment_num_ = kAppGeneratePktsNum;

    // ------------------Begin of the message handler------------------
  #if ApplyNewMbuf
      while (unlikely(alloc_bulk(tx_mbuf_buffer_, pkt_num) != 0)) {
        net_stats_app_apply_mbuf_stalls();
      }
  #endif
    if constexpr (handle == kRxMsgHandler_Empty) {}
    else if constexpr (handle == kRxMsgHandler_T_APP) this->T_APP(mbuf_ptr, pkt_num, &uh, &hdr);
    else DPERF_ERROR("Invalid message handler type!");
    // ------------------End of the message handler------------------
  #if ApplyNewMbuf
    de_alloc_bulk(msg, pkt_num);
    mbuf_ptr = tx_mbuf_buffer_;
  #else
    mbuf_ptr = msg;
  #endif
    /// Insert packets to worker tx queue
    for (i = 0; i < pkt_num; i++) {
      if (unlikely(!tx_queue_->enqueue((uint8_t*)(*mbuf_ptr)))) {
        /// Drop the packet if the tx queue is full
        de_alloc(*mbuf_ptr);
        drop_num++;
      }
      mbuf_ptr++;
    }
    net_stats_app_drops(drop_num);
  }
}