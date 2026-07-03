/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice_input.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

// clang-format off
#include "xenia/base/platform_win.h"
#include <mmeapi.h>
// clang-format on

namespace xe {
namespace apu {

struct VoiceInput::Impl {
  static constexpr int kNumBuffers = 16;
  // Buffer sizes are TIME-based, computed from the sample rate in Initialize --
  // fixed sample counts were tuned for 16 kHz and broke at 48 kHz (the denoise
  // capture rate): an 800-sample prime is 50ms @16k but only 16.7ms @48k, less
  // than one 20ms (960-sample) read, so every frame underran into silence.
  size_t buf_samples = 160;    // ~10 ms waveIn buffer granularity
  size_t prime_samples = 800;  // ~50 ms jitter buffer (must exceed one read)
  size_t max_queue = 4000;     // ~250 ms overrun cap

  HWAVEIN hwi = nullptr;
  WAVEHDR headers[kNumBuffers] = {};
  std::vector<int16_t> storage[kNumBuffers];
  std::deque<int16_t> queue;
  bool primed = false;
  std::mutex mutex;
};

VoiceInput::~VoiceInput() { Shutdown(); }

bool VoiceInput::Initialize(uint32_t sample_rate, uint32_t channels) {
  if (initialized_) {
    return true;
  }
  impl_ = new Impl();
  // Time-based buffering so 48 kHz capture is as smooth as 16 kHz: ~10ms waveIn
  // buffers, a ~50ms prime (always larger than one read), a ~250ms overrun cap.
  impl_->buf_samples = sample_rate / 100;
  impl_->prime_samples = sample_rate / 20;
  impl_->max_queue = sample_rate / 4;

  WAVEFORMATEX wfx = {};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = static_cast<WORD>(channels);
  wfx.nSamplesPerSec = sample_rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveInOpen(&impl_->hwi, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) {
    XELOGE("[voice] waveInOpen failed (MMRESULT {})", static_cast<uint32_t>(r));
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  for (int i = 0; i < Impl::kNumBuffers; ++i) {
    impl_->storage[i].resize(impl_->buf_samples);
    WAVEHDR& h = impl_->headers[i];
    std::memset(&h, 0, sizeof(h));
    h.lpData = reinterpret_cast<LPSTR>(impl_->storage[i].data());
    h.dwBufferLength =
        static_cast<DWORD>(impl_->buf_samples * sizeof(int16_t));
    waveInPrepareHeader(impl_->hwi, &h, sizeof(h));
    waveInAddBuffer(impl_->hwi, &h, sizeof(h));
  }
  waveInStart(impl_->hwi);
  initialized_ = true;
  XELOGI("[voice] host mic opened: {} Hz, {} ch (WinMM)", sample_rate, channels);
  return true;
}

size_t VoiceInput::Read(int16_t* out, size_t count) {
  if (!out || !count) {
    return 0;
  }
  if (!initialized_) {
    std::memset(out, 0, count * sizeof(int16_t));
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // Drain completed capture buffers into the queue and recycle them.
  for (int i = 0; i < Impl::kNumBuffers; ++i) {
    WAVEHDR& h = impl_->headers[i];
    if (h.dwFlags & WHDR_DONE) {
      const size_t n = h.dwBytesRecorded / sizeof(int16_t);
      const int16_t* p = reinterpret_cast<const int16_t*>(h.lpData);
      impl_->queue.insert(impl_->queue.end(), p, p + n);
      waveInUnprepareHeader(impl_->hwi, &h, sizeof(h));
      std::memset(&h, 0, sizeof(h));
      h.lpData = reinterpret_cast<LPSTR>(impl_->storage[i].data());
      h.dwBufferLength =
          static_cast<DWORD>(impl_->buf_samples * sizeof(int16_t));
      waveInPrepareHeader(impl_->hwi, &h, sizeof(h));
      waveInAddBuffer(impl_->hwi, &h, sizeof(h));
    }
  }
  // Overrun cap: drop oldest if the queue backs up (capture/consume rate drift).
  while (impl_->queue.size() > impl_->max_queue) {
    impl_->queue.pop_front();
  }
  // Jitter buffer: accumulate ~kPrimeSamples before serving so the bursty
  // capture delivery can't underrun against the title's frame pulls (the cause
  // of the ~8-10 Hz choppiness). Re-prime only after a full drain.
  if (!impl_->primed) {
    if (impl_->queue.size() >= impl_->prime_samples) {
      impl_->primed = true;
    } else {
      std::memset(out, 0, count * sizeof(int16_t));
      return 0;  // priming: no real samples served yet
    }
  }
  const size_t avail = std::min(count, impl_->queue.size());
  for (size_t i = 0; i < avail; ++i) {
    out[i] = impl_->queue.front();
    impl_->queue.pop_front();
  }
  for (size_t i = avail; i < count; ++i) {
    out[i] = 0;  // underrun -> silence
  }
  if (impl_->queue.empty()) {
    impl_->primed = false;  // fully drained -> rebuild the jitter buffer
  }
  return avail;
}

void VoiceInput::Shutdown() {
  if (!initialized_) {
    return;
  }
  waveInStop(impl_->hwi);
  waveInReset(impl_->hwi);
  for (int i = 0; i < Impl::kNumBuffers; ++i) {
    if (impl_->headers[i].dwFlags & WHDR_PREPARED) {
      waveInUnprepareHeader(impl_->hwi, &impl_->headers[i], sizeof(WAVEHDR));
    }
  }
  waveInClose(impl_->hwi);
  delete impl_;
  impl_ = nullptr;
  initialized_ = false;
}

}  // namespace apu
}  // namespace xe

#else  // !XE_PLATFORM_WIN32 -- no host mic backend yet.

namespace xe {
namespace apu {

struct VoiceInput::Impl {};
VoiceInput::~VoiceInput() {}
bool VoiceInput::Initialize(uint32_t, uint32_t) { return false; }
size_t VoiceInput::Read(int16_t* out, size_t count) {
  if (out && count) {
    std::memset(out, 0, count * sizeof(int16_t));
  }
  return 0;
}
void VoiceInput::Shutdown() {}

}  // namespace apu
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
