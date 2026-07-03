/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_INPUT_H_
#define XENIA_APU_VOICE_INPUT_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace apu {

// Host microphone capture for netplay voice-chat (voice Phase 2). Backed by
// WinMM waveIn on Windows; a no-op stub elsewhere. Captures mono 16-bit PCM at
// the voice rate into an internal queue; Read() pulls the requested number of
// samples (silence-padded on underrun). Samples are host-endian; the caller
// byte-swaps when writing into guest (big-endian) memory.
class VoiceInput {
 public:
  VoiceInput() = default;
  ~VoiceInput();

  bool Initialize(uint32_t sample_rate, uint32_t channels);
  void Shutdown();
  bool initialized() const { return initialized_; }

  // Fill `out` with `count` mono int16 samples (silence-padded on underrun).
  // Returns the number of REAL captured samples served (< count means the mic
  // underran and the tail was silence-padded; 0 means priming / fully drained).
  size_t Read(int16_t* out, size_t count);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool initialized_ = false;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_INPUT_H_
