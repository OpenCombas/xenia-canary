/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice_denoise.h"

#include <algorithm>
#include <cmath>

#include <rnnoise.h>

namespace xe {
namespace apu {

namespace {
// Anti-alias FIR for the 48 kHz -> 16 kHz (/3) decimation. 48 taps is plenty for
// voice; cutoff a little under the 8 kHz target-Nyquist for margin.
constexpr int kTaps = 48;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

VoiceDenoiser::~VoiceDenoiser() {
  if (rnnoise_) {
    rnnoise_destroy(rnnoise_);
    rnnoise_ = nullptr;
  }
}

bool VoiceDenoiser::Initialize() {
  if (rnnoise_) {
    return true;
  }
  rnnoise_ = rnnoise_create(nullptr);  // built-in (baked) model
  if (!rnnoise_) {
    return false;
  }

  // Windowed-sinc low-pass, cutoff 7.2 kHz @ 48 kHz, Hamming window, DC-normalized.
  const double fc = 7200.0 / 48000.0;  // cycles/sample
  const double center = (kTaps - 1) / 2.0;
  taps_.resize(kTaps);
  double sum = 0.0;
  for (int n = 0; n < kTaps; ++n) {
    const double m = n - center;
    const double sinc =
        (m == 0.0) ? 2.0 * fc
                   : std::sin(2.0 * kPi * fc * m) / (kPi * m);
    const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * n / (kTaps - 1));
    const double h = sinc * w;
    taps_[n] = static_cast<float>(h);
    sum += h;
  }
  for (float& t : taps_) {
    t = static_cast<float>(t / sum);  // unity DC gain
  }

  history_.assign(kTaps - 1, 0.0f);  // (taps-1) most recent denoised 48k samples
  return true;
}

float VoiceDenoiser::Process(const int16_t* in48k, int16_t* out16k) {
  if (!rnnoise_) {
    return 0.0f;
  }

  // 1) RNNoise: two 480-sample (10 ms) frames make one 20 ms unit. It both
  //    denoises and returns a speech probability; keep the louder of the two.
  float denoised[kIn48kSamples];
  float prob = 0.0f;
  float in_f[480], out_f[480];
  for (int f = 0; f < 2; ++f) {
    const int16_t* src = in48k + f * 480;
    for (int i = 0; i < 480; ++i) {
      in_f[i] = static_cast<float>(src[i]);  // RNNoise uses int16-range floats
    }
    const float p = rnnoise_process_frame(rnnoise_, out_f, in_f);
    prob = std::max(prob, p);
    for (int i = 0; i < 480; ++i) {
      denoised[f * 480 + i] = out_f[i];
    }
  }

  // 2) Anti-alias FIR + /3 decimation of the denoised 48k signal -> 16k. xat()
  //    reads the current frame or the carried history for negative indices.
  auto xat = [&](int idx) -> float {
    return idx >= 0 ? denoised[idx] : history_[(kTaps - 1) + idx];
  };
  for (int k = 0; k < static_cast<int>(kOut16kSamples); ++k) {
    const int i = 3 * k;  // decimation phase
    float acc = 0.0f;
    for (int j = 0; j < kTaps; ++j) {
      acc += taps_[j] * xat(i - j);
    }
    const float v = acc < -32768.0f ? -32768.0f : (acc > 32767.0f ? 32767.0f : acc);
    out16k[k] = static_cast<int16_t>(v);
  }

  // 3) Carry the last (taps-1) denoised 48k samples as the next call's history.
  for (int k = 0; k < kTaps - 1; ++k) {
    history_[k] = denoised[kIn48kSamples - (kTaps - 1) + k];
  }
  return prob;
}

}  // namespace apu
}  // namespace xe
