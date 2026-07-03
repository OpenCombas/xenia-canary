/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_OUTPUT_H_
#define XENIA_APU_VOICE_OUTPUT_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace apu {

// Minimal host audio sink for netplay voice-chat playback (voice Phase 1).
// Deliberately independent of the guest/APU audio path: it opens its own host
// output device and plays mono 16-bit PCM at the voice sample rate, so voice is
// mixed with game audio by the OS. Currently backed by WinMM waveOut on Windows;
// a no-op stub elsewhere until a cross-platform backend is added.
class VoiceOutput {
 public:
  VoiceOutput() = default;
  ~VoiceOutput();

  // Opens the host device. Returns false if unavailable (caller should treat
  // voice playback as disabled). Idempotent.
  bool Initialize(uint32_t sample_rate, uint32_t channels);
  void Shutdown();

  bool initialized() const { return initialized_; }

  // Queue mono 16-bit PCM for playback. Best-effort: on host-buffer overrun the
  // frame is dropped (voice tolerates loss). Safe to call from the guest voice
  // thread.
  void SubmitPcm(const int16_t* samples, size_t count);

  // Number of submitted buffers still queued/playing (not yet finished). Lets a
  // producer self-pace off the device's consumption -- keep this near a small
  // target instead of relying on a wall-clock submit cadence, which drifts
  // against the audio clock and underruns.
  int PendingCount();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool initialized_ = false;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_OUTPUT_H_
