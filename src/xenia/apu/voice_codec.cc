/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice_codec.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#include <opus.h>

DEFINE_int32(
    voice_opus_bitrate, 28000,
    "Netplay voice: Opus target bitrate in bits/sec for the 16 kHz mono voice "
    "side-channel. 20000 is intelligible-but-thin; 24000-32000 is clean "
    "wideband voice. Transport has ample headroom (~50 packets/s), so raise "
    "this if voice sounds robotic/metallic.",
    "Live");

DEFINE_int32(
    voice_opus_complexity, 10,
    "Netplay voice: Opus encoder complexity 0-10 (higher = better quality per "
    "bit, more CPU). 10 is best quality; the per-stream CPU cost is negligible.",
    "Live");

DEFINE_bool(
    voice_opus_fec, false,
    "Netplay voice: enable Opus in-band forward error correction (embeds a "
    "low-rate copy of the previous frame to recover single-packet loss). Costs "
    "bits that would otherwise improve base quality; only worth it on a lossy "
    "path. Pair with voice_opus_loss_perc. Off by default (the GNS voice path "
    "measures ~0% loss).",
    "Live");

DEFINE_int32(
    voice_opus_loss_perc, 0,
    "Netplay voice: expected packet-loss percentage (0-100) the Opus encoder "
    "hardens against. Above 0 it codes frames more defensively -- the classic "
    "'warbly/robotic' artifact -- so keep at 0 on a clean path and only raise "
    "it (with voice_opus_fec) if you actually see RX loss in the voice diag.",
    "Live");

namespace xe {
namespace apu {

namespace {

// Passthrough "codec": 20ms frames of 16 kHz mono, transported as raw int16
// bytes. Zero dependencies -- the plan's `pcm` debug option and the vehicle for
// proving the capture->transport->playback plumbing before Opus lands. Being
// stateless and self-describing, it tolerates loss (each packet is independent).
class PcmVoiceCodec : public VoiceCodec {
 public:
  const char* name() const override { return "pcm"; }
  uint32_t sample_rate() const override { return 16000; }
  size_t frame_samples() const override { return 320; }  // 20ms @ 16 kHz
  bool needs_reliable() const override { return false; }

  size_t Encode(const int16_t* pcm, size_t samples, uint8_t* out,
                size_t out_cap) override {
    const size_t bytes = samples * sizeof(int16_t);
    if (!pcm || !out || bytes > out_cap) {
      return 0;
    }
    std::memcpy(out, pcm, bytes);
    return bytes;
  }

  size_t Decode(const uint8_t* data, size_t size, int16_t* out,
                size_t out_cap) override {
    if (!data || !out) {
      return 0;
    }
    const size_t n = std::min(size / sizeof(int16_t), out_cap);
    std::memcpy(out, data, n * sizeof(int16_t));
    return n;
  }
  size_t DecodeLost(int16_t* out, size_t out_cap) override {
    return 0;  // stateless: caller silence-fills
  }
};

// libopus wideband voice codec: 16 kHz mono, 20ms frames, VoIP profile with
// in-band FEC so the loss-tolerant datagram path stays intelligible. This is the
// default netplay voice codec (Option 2).
class OpusVoiceCodec : public VoiceCodec {
 public:
  static std::unique_ptr<OpusVoiceCodec> TryCreate() {
    int err = OPUS_OK;
    OpusEncoder* enc =
        opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) {
      return nullptr;
    }
    OpusDecoder* dec = opus_decoder_create(kRate, 1, &err);
    if (err != OPUS_OK || !dec) {
      opus_encoder_destroy(enc);
      return nullptr;
    }
    const int bitrate = std::clamp(cvars::voice_opus_bitrate, 6000, 64000);
    const int complexity = std::clamp(cvars::voice_opus_complexity, 0, 10);
    const int loss_perc = std::clamp(cvars::voice_opus_loss_perc, 0, 100);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(cvars::voice_opus_fec ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(loss_perc));
    XELOGI(
        "[voice] opus encoder: {} bps, complexity {}, fec {}, loss_perc {}",
        bitrate, complexity, cvars::voice_opus_fec ? "on" : "off", loss_perc);
    return std::unique_ptr<OpusVoiceCodec>(new OpusVoiceCodec(enc, dec));
  }
  ~OpusVoiceCodec() override {
    if (enc_) {
      opus_encoder_destroy(enc_);
    }
    if (dec_) {
      opus_decoder_destroy(dec_);
    }
  }

  const char* name() const override { return "opus"; }
  uint32_t sample_rate() const override { return kRate; }
  size_t frame_samples() const override { return kFrame; }
  bool needs_reliable() const override { return false; }

  size_t Encode(const int16_t* pcm, size_t samples, uint8_t* out,
                size_t out_cap) override {
    if (!pcm || !out || samples != kFrame) {
      return 0;
    }
    const int n = opus_encode(enc_, pcm, static_cast<int>(kFrame), out,
                              static_cast<opus_int32>(out_cap));
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  size_t Decode(const uint8_t* data, size_t size, int16_t* out,
                size_t out_cap) override {
    if (!out) {
      return 0;
    }
    const int n = opus_decode(dec_, data, static_cast<opus_int32>(size), out,
                              static_cast<int>(std::min(out_cap, kFrame * 6u)),
                              /*decode_fec=*/0);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  size_t DecodeLost(int16_t* out, size_t out_cap) override {
    if (!out || out_cap < kFrame) {
      return 0;
    }
    // Opus packet-loss concealment: NULL packet -> conceal one frame.
    const int n = opus_decode(dec_, nullptr, 0, out, static_cast<int>(kFrame),
                              /*decode_fec=*/0);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

 private:
  static constexpr uint32_t kRate = 16000;
  static constexpr size_t kFrame = 320;  // 20ms @ 16 kHz
  OpusVoiceCodec(OpusEncoder* e, OpusDecoder* d) : enc_(e), dec_(d) {}
  OpusEncoder* enc_ = nullptr;
  OpusDecoder* dec_ = nullptr;
};

}  // namespace

std::unique_ptr<VoiceCodec> VoiceCodec::Create(const std::string& name) {
  if (name == "opus") {
    auto codec = OpusVoiceCodec::TryCreate();
    if (codec) {
      return codec;
    }
    XELOGW("[voice] opus init failed, falling back to pcm passthrough");
    return std::make_unique<PcmVoiceCodec>();
  }
  if (name != "pcm") {
    XELOGW("[voice] unknown codec '{}', using pcm passthrough", name);
  }
  return std::make_unique<PcmVoiceCodec>();
}

}  // namespace apu
}  // namespace xe
