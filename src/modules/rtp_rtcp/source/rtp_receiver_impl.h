/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_RTP_RECEIVER_IMPL_H_
#define MODULES_RTP_RTCP_SOURCE_RTP_RECEIVER_IMPL_H_

#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "modules/rtp_rtcp/include/rtp_receiver.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_receiver_strategy.h"
#include "rtc_base/criticalsection.h"
#include "typedefs.h"  // NOLINT(build/include)

namespace webrtc {

class RtpReceiverImpl : public RtpReceiver {
 public:
  // Callbacks passed in here may not be NULL (use Null Object callbacks if you
  // want callbacks to do nothing). This class takes ownership of the media
  // receiver but nothing else.
  RtpReceiverImpl(Clock* clock,
                  RtpFeedback* incoming_messages_callback,
                  RTPPayloadRegistry* rtp_payload_registry,
                  RTPReceiverStrategy* rtp_media_receiver);

  virtual ~RtpReceiverImpl();

  int32_t RegisterReceivePayload(const CodecInst& audio_codec) override;
  int32_t RegisterReceivePayload(const VideoCodec& video_codec) override;

  int32_t DeRegisterReceivePayload(const int8_t payload_type) override;

  bool IncomingRtpPacket(const RTPHeader& rtp_header,
                         const uint8_t* payload,
                         size_t payload_length,
                         PayloadUnion payload_specific,
                         bool in_order) override;

  // Returns the last received timestamp.
  bool Timestamp(uint32_t* timestamp) const override;
  bool LastReceivedTimeMs(int64_t* receive_time_ms) const override;

  uint32_t SSRC() const override;

  int32_t CSRCs(uint32_t array_of_csrc[kRtpCsrcSize]) const override;

  int32_t Energy(uint8_t array_of_energy[kRtpCsrcSize]) const override;

  TelephoneEventHandler* GetTelephoneEventHandler() override;

  std::vector<RtpSource> GetSources() const override;

  const std::vector<RtpSource>& ssrc_sources_for_testing() const {
    return ssrc_sources_;
  }

  const std::list<RtpSource>& csrc_sources_for_testing() const {
    return csrc_sources_;
  }

 private:
  bool HaveReceivedFrame() const;

  void CheckSSRCChanged(const RTPHeader& rtp_header);
  void CheckCSRC(const WebRtcRTPHeader& rtp_header);
  int32_t CheckPayloadChanged(const RTPHeader& rtp_header,
                              const int8_t first_payload_byte,
                              bool* is_red,
                              PayloadUnion* payload);

  void UpdateSources(const rtc::Optional<uint8_t>& ssrc_audio_level);
  void RemoveOutdatedSources(int64_t now_ms);

  Clock* clock_;
  RTPPayloadRegistry* rtp_payload_registry_;
  std::unique_ptr<RTPReceiverStrategy> rtp_media_receiver_;

  RtpFeedback* cb_rtp_feedback_;

  rtc::CriticalSection critical_section_rtp_receiver_;
  int64_t last_receive_time_;
  size_t last_received_payload_length_;

  // SSRCs.
  uint32_t ssrc_;
  uint8_t num_csrcs_;
  uint32_t current_remote_csrc_[kRtpCsrcSize];

  uint32_t last_received_timestamp_;
  int64_t last_received_frame_time_ms_;
  uint16_t last_received_sequence_number_;

  std::unordered_map<uint32_t, std::list<RtpSource>::iterator>
      iterator_by_csrc_;
  // The RtpSource objects are sorted chronologically.
  std::list<RtpSource> csrc_sources_;
  std::vector<RtpSource> ssrc_sources_;
};
}  // namespace webrtc
#endif  // MODULES_RTP_RTCP_SOURCE_RTP_RECEIVER_IMPL_H_
