/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/neteq/decoder_database.h"

#include <utility>  // pair

#include "webrtc/base/checks.h"
#include "webrtc/base/logging.h"
#include "webrtc/modules/audio_coding/codecs/audio_decoder.h"

namespace webrtc {

DecoderDatabase::DecoderDatabase(
    const rtc::scoped_refptr<AudioDecoderFactory>& decoder_factory)
    : active_decoder_type_(-1),
      active_cng_decoder_type_(-1),
      decoder_factory_(decoder_factory) {}

DecoderDatabase::~DecoderDatabase() = default;

DecoderDatabase::DecoderInfo::DecoderInfo(
    NetEqDecoder ct,
    const std::string& nm,
    AudioDecoderFactory* factory)
    : codec_type(ct),
      name(nm),
      audio_format_(acm2::RentACodec::NetEqDecoderToSdpAudioFormat(ct)),
      factory_(factory),
      external_decoder_(nullptr),
      cng_decoder_(CngDecoder::Create(ct)) {}

DecoderDatabase::DecoderInfo::DecoderInfo(NetEqDecoder ct,
                                          const std::string& nm,
                                          AudioDecoder* ext_dec)
    : codec_type(ct),
      name(nm),
      audio_format_(acm2::RentACodec::NetEqDecoderToSdpAudioFormat(ct)),
      external_decoder_(ext_dec) {
  RTC_CHECK(ext_dec);
}

DecoderDatabase::DecoderInfo::DecoderInfo(DecoderInfo&&) = default;
DecoderDatabase::DecoderInfo::~DecoderInfo() = default;

AudioDecoder* DecoderDatabase::DecoderInfo::GetDecoder() const {
  if (external_decoder_) {
    RTC_DCHECK(!decoder_);
    RTC_DCHECK(!cng_decoder_);
    return external_decoder_;
  }
  if (IsRed() || IsComfortNoise() || IsDtmf())
    return nullptr;
  RTC_DCHECK(audio_format_);
  if (!decoder_) {
    RTC_DCHECK(factory_);
    decoder_ = factory_->MakeAudioDecoder(*audio_format_);
  }
  RTC_DCHECK(decoder_) << "Failed to create: " << *audio_format_;
  return decoder_.get();
}


bool DecoderDatabase::DecoderInfo::IsComfortNoise() const {
  return codec_type == NetEqDecoder::kDecoderCNGnb
      || codec_type == NetEqDecoder::kDecoderCNGwb
      || codec_type == NetEqDecoder::kDecoderCNGswb32kHz
      || codec_type == NetEqDecoder::kDecoderCNGswb48kHz;
}

bool DecoderDatabase::DecoderInfo::IsDtmf() const {
  return codec_type == NetEqDecoder::kDecoderAVT;
}

bool DecoderDatabase::DecoderInfo::IsRed() const {
  return codec_type == NetEqDecoder::kDecoderRED;
}

rtc::Optional<DecoderDatabase::DecoderInfo::CngDecoder>
DecoderDatabase::DecoderInfo::CngDecoder::Create(NetEqDecoder ct) {
  const auto cng = [](int sample_rate_hz) {
    return rtc::Optional<DecoderDatabase::DecoderInfo::CngDecoder>(
        {sample_rate_hz});
  };
  switch (ct) {
    case NetEqDecoder::kDecoderCNGnb:
      return cng(8000);
    case NetEqDecoder::kDecoderCNGwb:
      return cng(16000);
    case NetEqDecoder::kDecoderCNGswb32kHz:
      return cng(32000);
    case NetEqDecoder::kDecoderCNGswb48kHz:
      return cng(48000);
    default:
      return rtc::Optional<DecoderDatabase::DecoderInfo::CngDecoder>();
  }
}

bool DecoderDatabase::Empty() const { return decoders_.empty(); }

int DecoderDatabase::Size() const { return static_cast<int>(decoders_.size()); }

void DecoderDatabase::Reset() {
  decoders_.clear();
  active_decoder_type_ = -1;
  active_cng_decoder_type_ = -1;
}

int DecoderDatabase::RegisterPayload(uint8_t rtp_payload_type,
                                     NetEqDecoder codec_type,
                                     const std::string& name) {
  if (rtp_payload_type > 0x7F) {
    return kInvalidRtpPayloadType;
  }
  if (!CodecSupported(codec_type)) {
    return kCodecNotSupported;
  }
  DecoderInfo info(codec_type, name, decoder_factory_.get());
  auto ret =
      decoders_.insert(std::make_pair(rtp_payload_type, std::move(info)));
  if (ret.second == false) {
    // Database already contains a decoder with type |rtp_payload_type|.
    return kDecoderExists;
  }
  return kOK;
}

int DecoderDatabase::InsertExternal(uint8_t rtp_payload_type,
                                    NetEqDecoder codec_type,
                                    const std::string& codec_name,
                                    AudioDecoder* decoder) {
  if (rtp_payload_type > 0x7F) {
    return kInvalidRtpPayloadType;
  }
  if (!CodecSupported(codec_type)) {
    return kCodecNotSupported;
  }
  if (!decoder) {
    return kInvalidPointer;
  }
  std::pair<DecoderMap::iterator, bool> ret;
  DecoderInfo info(codec_type, codec_name, decoder);
  ret = decoders_.insert(std::make_pair(rtp_payload_type, std::move(info)));
  if (ret.second == false) {
    // Database already contains a decoder with type |rtp_payload_type|.
    return kDecoderExists;
  }
  return kOK;
}

int DecoderDatabase::Remove(uint8_t rtp_payload_type) {
  if (decoders_.erase(rtp_payload_type) == 0) {
    // No decoder with that |rtp_payload_type|.
    return kDecoderNotFound;
  }
  if (active_decoder_type_ == rtp_payload_type) {
    active_decoder_type_ = -1;  // No active decoder.
  }
  if (active_cng_decoder_type_ == rtp_payload_type) {
    active_cng_decoder_type_ = -1;  // No active CNG decoder.
  }
  return kOK;
}

const DecoderDatabase::DecoderInfo* DecoderDatabase::GetDecoderInfo(
    uint8_t rtp_payload_type) const {
  DecoderMap::const_iterator it = decoders_.find(rtp_payload_type);
  if (it == decoders_.end()) {
    // Decoder not found.
    return NULL;
  }
  return &(*it).second;
}

uint8_t DecoderDatabase::GetRtpPayloadType(
    NetEqDecoder codec_type) const {
  DecoderMap::const_iterator it;
  for (it = decoders_.begin(); it != decoders_.end(); ++it) {
    if ((*it).second.codec_type == codec_type) {
      // Match found.
      return (*it).first;
    }
  }
  // No match.
  return kRtpPayloadTypeError;
}

int DecoderDatabase::SetActiveDecoder(uint8_t rtp_payload_type,
                                      bool* new_decoder) {
  // Check that |rtp_payload_type| exists in the database.
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  if (!info) {
    // Decoder not found.
    return kDecoderNotFound;
  }
  RTC_CHECK(!info->IsComfortNoise());
  RTC_DCHECK(new_decoder);
  *new_decoder = false;
  if (active_decoder_type_ < 0) {
    // This is the first active decoder.
    *new_decoder = true;
  } else if (active_decoder_type_ != rtp_payload_type) {
    // Moving from one active decoder to another. Delete the first one.
    const DecoderInfo *old_info = GetDecoderInfo(active_decoder_type_);
    RTC_DCHECK(old_info);
    old_info->DropDecoder();
    *new_decoder = true;
  }
  active_decoder_type_ = rtp_payload_type;
  return kOK;
}

AudioDecoder* DecoderDatabase::GetActiveDecoder() const {
  if (active_decoder_type_ < 0) {
    // No active decoder.
    return NULL;
  }
  return GetDecoder(active_decoder_type_);
}

int DecoderDatabase::SetActiveCngDecoder(uint8_t rtp_payload_type) {
  // Check that |rtp_payload_type| exists in the database.
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  if (!info) {
    // Decoder not found.
    return kDecoderNotFound;
  }
  if (active_cng_decoder_type_ >= 0 &&
      active_cng_decoder_type_ != rtp_payload_type) {
    // Moving from one active CNG decoder to another. Delete the first one.
    RTC_DCHECK(active_cng_decoder_);
    active_cng_decoder_.reset();
  }
  active_cng_decoder_type_ = rtp_payload_type;
  return kOK;
}

ComfortNoiseDecoder* DecoderDatabase::GetActiveCngDecoder() const {
  if (active_cng_decoder_type_ < 0) {
    // No active CNG decoder.
    return NULL;
  }
  if (!active_cng_decoder_) {
    active_cng_decoder_.reset(new ComfortNoiseDecoder);
  }
  return active_cng_decoder_.get();
}

AudioDecoder* DecoderDatabase::GetDecoder(uint8_t rtp_payload_type) const {
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  return info ? info->GetDecoder() : nullptr;
}

bool DecoderDatabase::IsType(uint8_t rtp_payload_type,
                             NetEqDecoder codec_type) const {
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  return info && info->codec_type == codec_type;
}

bool DecoderDatabase::IsComfortNoise(uint8_t rtp_payload_type) const {
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  return info && info->IsComfortNoise();
}

bool DecoderDatabase::IsDtmf(uint8_t rtp_payload_type) const {
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  return info && info->IsDtmf();
}

bool DecoderDatabase::IsRed(uint8_t rtp_payload_type) const {
  const DecoderInfo *info = GetDecoderInfo(rtp_payload_type);
  return info && info->IsRed();
}

int DecoderDatabase::CheckPayloadTypes(const PacketList& packet_list) const {
  PacketList::const_iterator it;
  for (it = packet_list.begin(); it != packet_list.end(); ++it) {
    if (!GetDecoderInfo((*it)->header.payloadType)) {
      // Payload type is not found.
      LOG(LS_WARNING) << "CheckPayloadTypes: unknown RTP payload type "
                      << static_cast<int>((*it)->header.payloadType);
      return kDecoderNotFound;
    }
  }
  return kOK;
}


}  // namespace webrtc