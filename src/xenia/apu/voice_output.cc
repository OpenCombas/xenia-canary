/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice_output.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

// clang-format off
#include "xenia/base/platform_win.h"
#include <mmeapi.h>
// clang-format on

namespace xe {
namespace apu {

struct VoiceOutput::Impl {
  // Enough headroom to hold a deep play-out queue (see kOutputTargetDepth) plus
  // free slots to stage the next frames while the queue is full -- so a burst of
  // scheduler preemption from the emulator's hot audio threads can't drain the
  // device to silence between refills.
  static constexpr int kNumBuffers = 24;
  static constexpr size_t kMaxSamplesPerBuffer = 2048;  // ~256ms @ 8kHz

  HWAVEOUT hwo = nullptr;
  WAVEHDR headers[kNumBuffers] = {};
  std::vector<int16_t> storage[kNumBuffers];
  int next = 0;
  std::mutex mutex;
};

VoiceOutput::~VoiceOutput() { Shutdown(); }

bool VoiceOutput::Initialize(uint32_t sample_rate, uint32_t channels) {
  if (initialized_) {
    return true;
  }
  impl_ = new Impl();

  WAVEFORMATEX wfx = {};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = static_cast<WORD>(channels);
  wfx.nSamplesPerSec = sample_rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveOutOpen(&impl_->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) {
    XELOGE("[voice] waveOutOpen failed (MMRESULT {})", static_cast<uint32_t>(r));
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  for (int i = 0; i < Impl::kNumBuffers; ++i) {
    impl_->storage[i].resize(Impl::kMaxSamplesPerBuffer);
    // WHDR_DONE marks a header free for reuse; not yet prepared.
    impl_->headers[i].dwFlags = WHDR_DONE;
  }
  initialized_ = true;
  XELOGI("[voice] host output opened: {} Hz, {} ch (WinMM)", sample_rate,
         channels);
  return true;
}

void VoiceOutput::SubmitPcm(const int16_t* samples, size_t count) {
  if (!initialized_ || !samples || !count) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (int tries = 0; tries < Impl::kNumBuffers; ++tries) {
    const int i = impl_->next;
    impl_->next = (impl_->next + 1) % Impl::kNumBuffers;
    WAVEHDR& h = impl_->headers[i];
    if (h.dwFlags & WHDR_PREPARED) {
      if (!(h.dwFlags & WHDR_DONE)) {
        continue;  // still playing -- try another buffer
      }
      waveOutUnprepareHeader(impl_->hwo, &h, sizeof(h));
    }
    const size_t n = std::min(count, Impl::kMaxSamplesPerBuffer);
    std::memcpy(impl_->storage[i].data(), samples, n * sizeof(int16_t));
    std::memset(&h, 0, sizeof(h));
    h.lpData = reinterpret_cast<LPSTR>(impl_->storage[i].data());
    h.dwBufferLength = static_cast<DWORD>(n * sizeof(int16_t));
    if (waveOutPrepareHeader(impl_->hwo, &h, sizeof(h)) == MMSYSERR_NOERROR) {
      waveOutWrite(impl_->hwo, &h, sizeof(h));
    }
    return;
  }
  // All host buffers busy -> drop this frame (overrun).
}

int VoiceOutput::PendingCount() {
  if (!initialized_) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  int pending = 0;
  for (const WAVEHDR& h : impl_->headers) {
    if ((h.dwFlags & WHDR_PREPARED) && !(h.dwFlags & WHDR_DONE)) {
      ++pending;
    }
  }
  return pending;
}

void VoiceOutput::Shutdown() {
  if (!initialized_) {
    return;
  }
  waveOutReset(impl_->hwo);
  for (int i = 0; i < Impl::kNumBuffers; ++i) {
    if (impl_->headers[i].dwFlags & WHDR_PREPARED) {
      waveOutUnprepareHeader(impl_->hwo, &impl_->headers[i], sizeof(WAVEHDR));
    }
  }
  waveOutClose(impl_->hwo);
  delete impl_;
  impl_ = nullptr;
  initialized_ = false;
}

}  // namespace apu
}  // namespace xe

#else  // !XE_PLATFORM_WIN32 -- no host voice backend yet.

namespace xe {
namespace apu {

struct VoiceOutput::Impl {};
VoiceOutput::~VoiceOutput() {}
bool VoiceOutput::Initialize(uint32_t, uint32_t) { return false; }
void VoiceOutput::SubmitPcm(const int16_t*, size_t) {}
int VoiceOutput::PendingCount() { return 0; }
void VoiceOutput::Shutdown() {}

}  // namespace apu
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
