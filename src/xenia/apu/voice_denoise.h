/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_VOICE_DENOISE_H_
#define XENIA_APU_VOICE_DENOISE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

struct DenoiseState;  // rnnoise

namespace xe {
namespace apu {

// RNNoise-based voice isolation for the netplay voice side-channel. RNNoise runs
// at a fixed 48 kHz / 480-sample (10 ms) frame and both suppresses background
// noise and returns a per-frame speech probability -- a far better gate than raw
// energy. Our voice path is 16 kHz, so this captures at 48 kHz, denoises, and
// downsamples the cleaned signal to 16 kHz (the Opus wire format is unchanged, so
// a denoising sender still interops with the mesh).
//
// One 20 ms unit: 960 samples in @48k -> 320 samples out @16k, plus the speech
// probability [0,1] (the max over the two 10 ms RNNoise frames).
class VoiceDenoiser {
 public:
  VoiceDenoiser() = default;
  ~VoiceDenoiser();

  // Samples of 48 kHz input consumed / 16 kHz output produced per Process().
  static constexpr size_t kIn48kSamples = 960;   // 20 ms @ 48 kHz
  static constexpr size_t kOut16kSamples = 320;  // 20 ms @ 16 kHz

  bool Initialize();  // returns false if RNNoise couldn't be created
  bool initialized() const { return rnnoise_ != nullptr; }

  // Denoise one 20 ms frame. in48k must hold kIn48kSamples, out16k receives
  // kOut16kSamples. Returns the speech probability [0,1] for gating.
  float Process(const int16_t* in48k, int16_t* out16k);

 private:
  DenoiseState* rnnoise_ = nullptr;

  // Anti-alias FIR + /3 decimation (48 kHz -> 16 kHz). history_ carries the tail
  // of the previous 48 kHz frame so the filter is continuous across calls.
  std::vector<float> taps_;
  std::vector<float> history_;  // (taps-1) most recent 48k input samples
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_VOICE_DENOISE_H_
