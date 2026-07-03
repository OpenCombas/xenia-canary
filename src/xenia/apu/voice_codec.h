/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_CODEC_H_
#define XENIA_APU_VOICE_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace xe {
namespace apu {

// Pluggable voice codec for the netplay voice side-channel (Option 2). Operates
// on fixed frames of mono int16 PCM at sample_rate(). Encode compresses one
// frame; Decode expands one received packet back to (up to) frame_samples()
// samples. Implementations: "pcm" (raw passthrough, zero-dep debug/reference)
// and "opus" (wideband, default). The transport reliability the caller uses is
// chosen per codec (Opus tolerates loss via PLC; stateful codecs need ordered
// delivery), so the codec advertises whether it needs a reliable channel.
class VoiceCodec {
 public:
  virtual ~VoiceCodec() = default;

  virtual const char* name() const = 0;
  virtual uint32_t sample_rate() const = 0;
  // Samples per Encode() frame (mono). Opus requires exactly this many.
  virtual size_t frame_samples() const = 0;
  // True if the codec's decoder is stateful and must receive packets in order
  // without loss (=> reliable transport). False for loss-tolerant codecs.
  virtual bool needs_reliable() const = 0;

  // Encode `samples` mono int16 PCM into `out` (<= out_cap bytes). Returns the
  // encoded byte count, or 0 on failure.
  virtual size_t Encode(const int16_t* pcm, size_t samples, uint8_t* out,
                        size_t out_cap) = 0;
  // Decode one packet into `out` (<= out_cap samples). Returns sample count.
  virtual size_t Decode(const uint8_t* data, size_t size, int16_t* out,
                        size_t out_cap) = 0;
  // Conceal one lost frame (jitter-buffer underrun). Advances decoder state so a
  // subsequent real packet stays continuous. Returns samples written (0 = the
  // caller should silence-fill). Opus uses its PLC; stateless codecs return 0.
  virtual size_t DecodeLost(int16_t* out, size_t out_cap) = 0;

  // Factory: "pcm" or "opus". Falls back to "pcm" for unknown/unavailable names.
  static std::unique_ptr<VoiceCodec> Create(const std::string& name);
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_CODEC_H_
