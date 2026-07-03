/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/voice_channel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "xenia/apu/voice_codec.h"
#include "xenia/apu/voice_denoise.h"
#include "xenia/apu/voice_input.h"
#include "xenia/apu/voice_output.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/gns_transport.h"

DEFINE_bool(
    voice_channel, false,
    "Netplay voice: enable the out-of-band Opus voice side-channel -- capture "
    "the host mic, encode (voice_codec), and send to session peers over the GNS "
    "voice port; play received peer voice on the host output. Independent of the "
    "title's native voice; needs a netplay session with mapped peers.",
    "Live");

DEFINE_int32(
    voice_mic_gain, 100,
    "Netplay voice: side-channel mic input gain, in percent (100 = unity). Boost "
    "if your transmitted voice is quiet (try 200-400). Applied before encode "
    "with hard clipping, so avoid over-driving. Live.",
    "Live");

DEFINE_bool(
    voice_vad, true,
    "Netplay voice: energy-based voice activity detection -- only transmit "
    "frames whose level exceeds voice_vad_threshold (with hangover), so silence, "
    "breathing and background noise aren't sent. Layered on top of the title's "
    "own voice gate unless voice_vad_open_mic is set.",
    "Live");

DEFINE_int32(
    voice_vad_threshold, 300,
    "Netplay voice: VAD open threshold as RMS of the processed 16-bit frame "
    "(measured after the high-pass and mic gain). Raise if room/keyboard noise "
    "leaks through; lower if quiet speech gets clipped. Speech is typically "
    ">600, a quiet room <150.",
    "Live");

DEFINE_int32(
    voice_vad_hangover_ms, 250,
    "Netplay voice: keep transmitting this many ms after the level drops below "
    "the VAD threshold, so word-endings and short pauses aren't chopped.",
    "Live");

DEFINE_bool(
    voice_vad_open_mic, false,
    "Netplay voice: ignore the title's flags==1 voice gate and let VAD alone "
    "decide when to transmit (open mic) -- useful if the title holds its voice "
    "channel open continuously. With voice_vad off this transmits continuously.",
    "Live");

DEFINE_bool(
    voice_highpass, true,
    "Netplay voice: apply a ~90 Hz one-pole high-pass (DC blocker) to captured "
    "mic audio before encoding, removing DC bias and low-frequency rumble.",
    "Live");

DEFINE_int32(
    voice_output_gain, 100,
    "Netplay voice: master playback gain for received peer voice, in percent "
    "(100 = unity). The mixed output is soft-limited so a boost or several loud "
    "peers won't hard-clip.",
    "Live");

DEFINE_bool(
    voice_diag, true,
    "Netplay voice: emit periodic (~2s) pipeline diagnostics -- TX frames "
    "sent/gated + mic underrun, and per-peer RX jitter-buffer stats (received / "
    "played / PLC / stall / skip / reprime). For chasing choppiness; low volume, "
    "safe to leave on. Set false to silence.",
    "Live");

DEFINE_bool(
    voice_denoise, true,
    "Netplay voice: RNNoise noise suppression + speech-probability VAD for true "
    "voice isolation. Captures at 48 kHz, removes background noise, gates on the "
    "RNN speech probability (voice_vad_prob), then downsamples to the 16 kHz Opus "
    "wire format (unchanged -- still interops with the party mesh). Supersedes the "
    "energy VAD + high-pass when on. On by default (the best-quality path); set "
    "false to fall back to the energy VAD. Takes effect on restart.",
    "Live");

DEFINE_int32(
    voice_vad_prob, 60,
    "Netplay voice: RNNoise VAD open threshold as a speech-probability percent "
    "[0,100] (used only when voice_denoise is on). Frames whose RNN speech "
    "probability exceeds this are transmitted; raise to reject more residual "
    "noise, lower if quiet speech gets clipped. Uses the same hangover as the "
    "energy VAD.",
    "Live");

DEFINE_bool(
    voice_agc, true,
    "Netplay voice: automatic gain control -- adaptively level the mic toward "
    "voice_agc_target so quiet and loud talkers come out at a similar volume. "
    "Adapts only on speech-level frames (holds on silence so it doesn't pump up "
    "noise), attack-fast/release-slow, capped at voice_agc_max_gain. When on it "
    "SUPERSEDES the fixed voice_mic_gain.",
    "Live");

DEFINE_int32(
    voice_agc_target, 4000,
    "Netplay voice: AGC target level, as RMS of the 16-bit frame (0..32767). "
    "The gain adapts to bring speech toward this. Raise for louder transmitted "
    "voice, lower if it clips.",
    "Live");

DEFINE_int32(
    voice_agc_max_gain, 1000,
    "Netplay voice: AGC maximum boost, in percent (1000 = up to 10x). Caps how "
    "much a very quiet mic is amplified, so room noise in gaps isn't blown up.",
    "Live");

DEFINE_bool(
    voice_duck, true,
    "Netplay voice: feedback guard (half-duplex ducking) -- attenuate the mic "
    "while peer voice is actively playing on your output, so audio from your "
    "speakers isn't picked up and re-transmitted (the echo/howl loop). Harmless "
    "on a headset; important on open speakers. Not full echo cancellation.",
    "Live");

DEFINE_int32(
    voice_duck_atten, 25,
    "Netplay voice: how far to duck the mic while a peer is talking, in percent "
    "(25 = attenuate to 25%, ~-12 dB). Lower ducks harder (more feedback "
    "rejection, more of your own speech lost); 100 disables ducking.",
    "Live");

DECLARE_string(voice_codec);  // apu/audio_system.cc

namespace xe {
namespace kernel {

namespace {

// Keep transmitting this long after the last MarkTransmitActive so brief gaps
// between the title's flags==1 pumps don't chop the outbound stream.
constexpr int64_t kTransmitHoldMs = 300;

// Per-peer jitter buffer (ring of encoded frames, indexed by sequence).
constexpr size_t kRing = 64;         // ~1.3s of 20ms frames
constexpr int kPrimeFrames = 3;      // buffer 3 frames (~60ms) before playing
constexpr int kMaxDepthFrames = 12;  // ~240ms latency cap; skip ahead beyond it
constexpr int kTargetDepthFrames = 3;
constexpr int64_t kPeerExpiryMs = 3000;  // drop a peer silent this long
constexpr int kOutputTargetDepth = 8;    // waveOut frames kept queued (~160ms)
constexpr int64_t kScopeRefreshMs = 1000;  // recompute voice recipients this often

constexpr uint8_t kVersion = 1;
#pragma pack(push, 1)
struct VoiceHeader {
  uint8_t version;  // kVersion
  uint8_t codec;    // informational (0 = opus)
  uint16_t seq;     // sender sequence (host-endian; peers are same-arch)
};
#pragma pack(pop)

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Wrap-safe sequence comparison (uint16 counter).
inline int16_t SeqDiff(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(a - b);
}

// Soft peak limiter: linear below the knee, smooth tanh saturation above, so
// loud sums (a gain boost / several peers) round off instead of hard-clipping.
inline int16_t SoftLimitToPcm(float sample) {
  float norm = sample * (1.0f / 32768.0f);
  constexpr float knee = 0.8f;
  const float a = std::fabs(norm);
  if (a > knee) {
    const float over = (a - knee) / (1.0f - knee);
    norm = std::copysign(knee + (1.0f - knee) * std::tanh(over), norm);
  }
  if (norm > 1.0f) norm = 1.0f;
  if (norm < -1.0f) norm = -1.0f;
  return static_cast<int16_t>(norm * 32767.0f);
}

struct RingSlot {
  uint16_t seq = 0;
  bool valid = false;
  std::vector<uint8_t> data;
};

}  // namespace

// One receiving peer: its own stateful Opus decoder + a jitter buffer, so each
// stream decodes correctly (Opus is stateful) and network jitter/loss is
// absorbed independently.
struct VoicePeer {
  std::unique_ptr<apu::VoiceCodec> codec;
  RingSlot ring[kRing];
  uint16_t play_seq = 0;
  bool primed = false;
  uint16_t newest_seq = 0;
  bool have_newest = false;
  int filled = 0;
  int underrun_ = 0;  // consecutive live-edge underrun frames (stall counter)
  int64_t last_recv_ms = 0;
  float level = 0.0f;  // smoothed [0,1] output level for the party overlay

  // --- diagnostics (cumulative; deltas logged on a ~2s cadence) ---
  struct Diag {
    uint64_t received = 0;  // frames Push()ed (arrived on the wire)
    uint64_t played = 0;    // real frames decoded + emitted
    uint64_t plc = 0;       // concealed (lost/late before the live edge)
    uint64_t stall = 0;     // live-edge underruns (silence, no data yet)
    uint64_t skip = 0;      // latency-cap jump-aheads (buffer too deep)
    uint64_t reprime = 0;   // dropped back to priming after a sustained stall
  } diag, diag_logged;

  void Push(uint16_t seq, const uint8_t* d, size_t n, int64_t now_ms) {
    ++diag.received;
    RingSlot& s = ring[seq % kRing];
    if (!s.valid) {
      ++filled;
    }
    s.seq = seq;
    s.valid = true;
    s.data.assign(d, d + n);
    if (!have_newest || SeqDiff(seq, newest_seq) > 0) {
      newest_seq = seq;
      have_newest = true;
    }
    last_recv_ms = now_ms;
  }

  // Produce exactly `frame` samples: the buffered packet for play_seq (real),
  // Opus PLC on a gap, or silence while priming / underrun.
  void PullFrame(int16_t* out, size_t frame) {
    if (!primed) {
      if (!have_newest || filled < kPrimeFrames) {
        std::memset(out, 0, frame * sizeof(int16_t));
        return;
      }
      play_seq = static_cast<uint16_t>(newest_seq - (kPrimeFrames - 1));
      primed = true;
    }
    // Latency cap: if we've fallen too far behind the newest, jump forward.
    if (have_newest && SeqDiff(newest_seq, play_seq) > kMaxDepthFrames) {
      play_seq = static_cast<uint16_t>(newest_seq - kTargetDepthFrames);
      ++diag.skip;
    }
    RingSlot& s = ring[play_seq % kRing];
    size_t n;
    if (s.valid && s.seq == play_seq) {
      // Have the frame: decode + advance.
      n = codec->Decode(s.data.data(), s.data.size(), out, frame);
      s.valid = false;
      --filled;
      ++play_seq;
      underrun_ = 0;
      ++diag.played;
    } else if (SeqDiff(play_seq, newest_seq) < 0) {
      // Hole *before* the live edge -- a lost/late packet. Conceal (PLC) and
      // advance so we stay aligned with the live stream.
      n = codec->DecodeLost(out, frame);
      ++play_seq;
      underrun_ = 0;
      ++diag.plc;
    } else {
      // Underrun at the live edge: no data yet -- network/scheduler jitter, or
      // the sender paused (e.g. VAD gated a gap). STALL: emit silence WITHOUT
      // advancing play_seq, so playback waits for the next frame instead of
      // running ahead of the stream (which would warble on jitter and desync
      // for seconds after a long gap). Only after a sustained stall drop back to
      // priming, to rebuild the jitter cushion.
      ++diag.stall;
      if (++underrun_ > kMaxDepthFrames) {
        primed = false;
        ++diag.reprime;
      }
      std::memset(out, 0, frame * sizeof(int16_t));
      return;
    }
    if (n < frame) {
      std::memset(out + n, 0, (frame - n) * sizeof(int16_t));
    }
  }
};

VoiceChannel* VoiceChannel::Get() {
  static VoiceChannel instance;
  return &instance;
}

VoiceChannel::~VoiceChannel() { Stop(); }

void VoiceChannel::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  GNSTransport::Get()->SetVoiceHandler(
      [this](uint32_t src_ina, const uint8_t* data, size_t len) {
        OnVoicePacket(src_ina, data, len);
      });
  send_thread_ = std::thread(&VoiceChannel::SendThreadMain, this);
  playback_thread_ = std::thread(&VoiceChannel::PlaybackThreadMain, this);
  XELOGI("[voice] side-channel started (codec={})", cvars::voice_codec);
}

void VoiceChannel::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (send_thread_.joinable()) {
    send_thread_.join();
  }
  if (playback_thread_.joinable()) {
    playback_thread_.join();
  }
  GNSTransport::Get()->SetVoiceHandler(nullptr);
  {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_.clear();
  }
  output_.reset();
  XELOGI("[voice] side-channel stopped");
}

void VoiceChannel::MarkTransmitActive() {
  last_active_ms_.store(NowMs(), std::memory_order_relaxed);
}

float VoiceChannel::GetPeerActivity(uint32_t ina) const {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  auto it = peers_.find(ina);
  return it == peers_.end() ? 0.0f : it->second->level;
}

bool VoiceChannel::IsSelfActive() const {
  // ~250ms hold so the dot stays lit across the VAD hangover between frames.
  return (NowMs() - self_tx_ms_.load(std::memory_order_relaxed)) < 250;
}

void VoiceChannel::SetPartyRoster(std::vector<uint32_t> member_inas) {
  std::lock_guard<std::mutex> lock(roster_mutex_);
  party_roster_ = std::move(member_inas);
}

void VoiceChannel::SendThreadMain() {
  auto codec = apu::VoiceCodec::Create(cvars::voice_codec);
  const uint32_t rate = codec->sample_rate();
  const size_t frame = codec->frame_samples();
  // RNNoise isolation (voice_denoise): capture at 48 kHz, denoise + get a speech
  // probability, and downsample the cleaned signal to the codec's 16 kHz frame
  // (the Opus wire format is unchanged, so a denoising sender still interops with
  // the mesh). Off => the legacy energy-VAD path at the codec rate.
  apu::VoiceDenoiser denoiser;
  if (cvars::voice_denoise && !denoiser.Initialize()) {
    XELOGW("[voice] RNNoise init failed; using the energy VAD path");
  }
  const bool denoise_on = cvars::voice_denoise && denoiser.initialized();
  const uint32_t cap_rate = denoise_on ? 48000u : rate;
  const size_t cap_frame =
      denoise_on ? apu::VoiceDenoiser::kIn48kSamples : frame;
  apu::VoiceInput input;
  input.Initialize(cap_rate, 1);
  XELOGI("[voice] side-channel TX: codec={} {} Hz, {}-sample frames{}",
         codec->name(), rate, frame,
         denoise_on ? " (RNNoise denoise @48kHz)" : "");

  std::vector<int16_t> pcm(frame);      // 16 kHz signal handed to the encoder
  std::vector<int16_t> cap(cap_frame);  // raw mic capture (48 kHz when denoising)
  std::vector<uint8_t> pkt(sizeof(VoiceHeader) + 4000);
  auto* gns = GNSTransport::Get();
  const bool reliable = codec->needs_reliable();
  uint16_t seq = 0;
  // Party roster to broadcast to, snapshotted on a slow cadence (it changes far
  // slower than the frame tick). Non-empty => we're in a party.
  std::vector<uint32_t> recipients;
  int64_t last_scope_ms = 0;
  // TX DSP state: one-pole DC-block/high-pass (~90 Hz) + VAD hangover deadline.
  const float hpf_r =
      1.0f - 2.0f * 3.14159265f * 90.0f / static_cast<float>(rate);
  float hpf_x1 = 0.0f, hpf_y1 = 0.0f;
  int64_t vad_hold_until = 0;
  float agc_gain = 1.0f;  // smoothed AGC gain (voice_agc)

  const auto frame_dur =
      std::chrono::microseconds(static_cast<int64_t>(frame) * 1000000 / rate);
  auto next = std::chrono::steady_clock::now();
  // Diagnostics (deltas logged on a ~2s cadence): outbound frame accounting +
  // mic-capture health, to distinguish a starved sender from RX-side loss.
  uint64_t d_sent = 0, d_gated = 0, d_vad_opens = 0, d_mic_underrun = 0,
           d_mic_prime = 0;
  bool prev_vad_open = false;
  int64_t last_diag_ms = NowMs();
  while (running_.load(std::memory_order_relaxed)) {
    // Real samples served (< frame = mic underrun; 0 = the input jitter buffer
    // is priming / was fully drained -- both surface as outbound silence).
    const size_t mic_real = input.Read(cap.data(), cap_frame);
    if (mic_real == 0) {
      ++d_mic_prime;
    } else if (mic_real < cap_frame) {
      ++d_mic_underrun;
    }

    // Produce the 16 kHz frame to encode + a speech indicator. Denoising cleans
    // the 48k capture and downsamples to 16k (returning an RNN speech
    // probability); otherwise the capture already IS the 16k frame.
    float rn_prob = 0.0f;
    if (denoise_on) {
      rn_prob = denoiser.Process(cap.data(), pcm.data());
    } else {
      std::memcpy(pcm.data(), cap.data(), frame * sizeof(int16_t));
    }

    // --- TX DSP chain (runs every frame so filter + VAD state stay continuous)
    // 1) DC-block / high-pass: y = x - x1 + R*y1  (strips mic DC bias + rumble).
    // Skipped when denoising -- RNNoise already removes low-frequency noise.
    if (!denoise_on && cvars::voice_highpass) {
      for (size_t i = 0; i < frame; ++i) {
        const float x = pcm[i];
        const float y = x - hpf_x1 + hpf_r * hpf_y1;
        hpf_x1 = x;
        hpf_y1 = y;
        pcm[i] = static_cast<int16_t>(
            y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y));
      }
    }
    // 2) Input gain. AGC (voice_agc) adaptively levels toward voice_agc_target
    //    and supersedes the fixed voice_mic_gain; otherwise the fixed gain.
    if (cvars::voice_agc) {
      double acc = 0.0;
      for (size_t i = 0; i < frame; ++i) {
        acc += static_cast<double>(pcm[i]) * pcm[i];
      }
      const float rms = static_cast<float>(std::sqrt(acc / frame));
      const float max_g = std::max(1.0f, cvars::voice_agc_max_gain * 0.01f);
      const float target = static_cast<float>(cvars::voice_agc_target);
      // Adapt only on speech-level frames (>10% of target); hold on silence so
      // the gain doesn't creep up on room noise. Attack fast, release slow.
      if (rms > target * 0.1f) {
        const float desired = std::min(max_g, target / rms);
        const float a = desired < agc_gain ? 0.5f : 0.05f;
        agc_gain += (desired - agc_gain) * a;
      }
      agc_gain = std::clamp(agc_gain, 0.1f, max_g);
      for (size_t i = 0; i < frame; ++i) {
        const float v = pcm[i] * agc_gain;
        pcm[i] = static_cast<int16_t>(
            v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
      }
    } else {
      const int gain_pct = cvars::voice_mic_gain;
      if (gain_pct != 100) {
        const float g = gain_pct * 0.01f;
        for (size_t i = 0; i < frame; ++i) {
          const float v = pcm[i] * g;
          pcm[i] = static_cast<int16_t>(
              v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
      }
    }
    // 2b) Feedback guard (voice_duck): while a peer is actively playing on our
    //     output, attenuate the mic so speaker bleed isn't re-transmitted (the
    //     howl loop). Harmless on a headset; not full echo cancellation.
    if (cvars::voice_duck && cvars::voice_duck_atten < 100 &&
        NowMs() - far_end_active_ms_.load(std::memory_order_relaxed) < 120) {
      const float d = std::clamp(cvars::voice_duck_atten * 0.01f, 0.0f, 1.0f);
      for (size_t i = 0; i < frame; ++i) {
        pcm[i] = static_cast<int16_t>(pcm[i] * d);
      }
    }
    // 3) Voice-activity gate with hysteresis + hangover. Denoising gates on the
    //    RNN speech probability; otherwise on RMS energy of the processed frame.
    const int64_t now_ms = NowMs();
    if (denoise_on) {
      const float open_p = cvars::voice_vad_prob * 0.01f;
      const float close_p = open_p * 0.5f;
      if (rn_prob >= open_p ||
          (now_ms < vad_hold_until && rn_prob >= close_p)) {
        vad_hold_until = now_ms + cvars::voice_vad_hangover_ms;
      }
    } else {
      double acc = 0.0;
      for (size_t i = 0; i < frame; ++i) {
        acc += static_cast<double>(pcm[i]) * pcm[i];
      }
      const float rms = static_cast<float>(std::sqrt(acc / frame));
      const float open_th = static_cast<float>(cvars::voice_vad_threshold);
      const float close_th = open_th * 0.5f;
      if (rms >= open_th || (now_ms < vad_hold_until && rms >= close_th)) {
        vad_hold_until = now_ms + cvars::voice_vad_hangover_ms;
      }
    }
    const bool vad_open = now_ms < vad_hold_until;
    if (vad_open && !prev_vad_open) {
      ++d_vad_opens;
    }
    prev_vad_open = vad_open;

    // Refresh the party roster snapshot on a slow cadence (it changes far slower
    // than the frame tick). A non-empty roster means we're in a party.
    if (now_ms - last_scope_ms > kScopeRefreshMs) {
      last_scope_ms = now_ms;
      std::lock_guard<std::mutex> lock(roster_mutex_);
      recipients = party_roster_;
    }

    // --- Transmit gate: upstream permission AND VAD. In a party the mic is open
    // (VAD still gates real speech) regardless of any game voice pump, since a
    // party is decoupled from games and may run at the dashboard.
    const bool title_active =
        (now_ms - last_active_ms_.load(std::memory_order_relaxed)) <
        kTransmitHoldMs;
    const bool in_party = !recipients.empty();
    const bool upstream =
        cvars::voice_vad_open_mic || title_active || in_party;
    if (upstream && (!cvars::voice_vad || vad_open)) {
      uint8_t* body = pkt.data() + sizeof(VoiceHeader);
      const size_t n = codec->Encode(pcm.data(), frame, body,
                                     pkt.size() - sizeof(VoiceHeader));
      if (n) {
        ++d_sent;
        self_tx_ms_.store(now_ms, std::memory_order_relaxed);  // self "speaking"
        auto* h = reinterpret_cast<VoiceHeader*>(pkt.data());
        h->version = kVersion;
        h->codec = 0;
        h->seq = seq++;
        const size_t total = sizeof(VoiceHeader) + n;
        // Broadcast to the current party roster (snapshot refreshed above).
        for (uint32_t peer : recipients) {
          gns->SendTo(peer, GNSTransport::kVoicePort, GNSTransport::kVoicePort,
                      pkt.data(), total, reliable);
        }
      }
    } else {
      ++d_gated;  // gate closed this frame (title inactive and/or VAD closed)
    }

    // Periodic TX accounting: expect ~50 frames/s. sent+gated ~= the tick count;
    // a big gated share means VAD/title gating, mic_underrun/mic_prime means the
    // capture path is starving (outbound silence regardless of the gate).
    if (cvars::voice_diag && now_ms - last_diag_ms >= 2000) {
      const double secs = (now_ms - last_diag_ms) / 1000.0;
      XELOGI(
          "[voice] TX diag /{:.1f}s: sent={} gated={} vad_opens={} "
          "mic_underrun={} mic_prime={} recipients={}",
          secs, d_sent, d_gated, d_vad_opens, d_mic_underrun, d_mic_prime,
          recipients.size());
      d_sent = d_gated = d_vad_opens = d_mic_underrun = d_mic_prime = 0;
      last_diag_ms = now_ms;
    }
    next += frame_dur;
    std::this_thread::sleep_until(next);
  }
}

void VoiceChannel::PlaybackThreadMain() {
  auto probe = apu::VoiceCodec::Create(cvars::voice_codec);
  const uint32_t rate = probe->sample_rate();
  const size_t frame = probe->frame_samples();
  probe.reset();
  output_ = std::make_unique<apu::VoiceOutput>();
  output_->Initialize(rate, 1);
  XELOGI("[voice] side-channel RX/mix: {} Hz, {}-sample frames", rate, frame);

  std::vector<int32_t> mix(frame);
  std::vector<int16_t> peer_pcm(frame);
  std::vector<int16_t> out(frame);
  int64_t last_diag_ms = NowMs();
  while (running_.load(std::memory_order_relaxed)) {
    // Periodic RX accounting, per peer (deltas since the last line). Balanced
    // playback wants received ~= played ~= 50/s. played < received => draining
    // (skips); received < played => starved (stall/plc). This is where the
    // 62ms-audio/8ms-gap choppiness will show up as stall (RX underrun) vs the
    // TX diag showing under-production.
    if (cvars::voice_diag) {
      const int64_t now = NowMs();
      if (now - last_diag_ms >= 2000) {
        const double secs = (now - last_diag_ms) / 1000.0;
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [ina, p] : peers_) {
          auto& d = p->diag;
          auto& l = p->diag_logged;
          XELOGI(
              "[voice] RX diag peer {:08X} /{:.1f}s: received={} played={} "
              "plc={} stall={} skip={} reprime={}",
              ina, secs, d.received - l.received, d.played - l.played,
              d.plc - l.plc, d.stall - l.stall, d.skip - l.skip,
              d.reprime - l.reprime);
          l = d;
        }
        last_diag_ms = now;
      }
    }
    // Self-pace off the device: keep the waveOut queue topped up to
    // kOutputTargetDepth. As the device drains buffers, PendingCount drops and
    // we mix+submit to refill -- so the audio clock (not a drifting wall clock)
    // sets the rate, which is what prevents periodic underrun crackle. Bound the
    // burst so a wedged device can't spin.
    int budget = kOutputTargetDepth + 2;
    while (output_ && output_->initialized() &&
           output_->PendingCount() < kOutputTargetDepth && budget-- > 0) {
      std::fill(mix.begin(), mix.end(), 0);
      {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        const int64_t now = NowMs();
        for (auto it = peers_.begin(); it != peers_.end();) {
          VoicePeer* p = it->second.get();
          if (now - p->last_recv_ms > kPeerExpiryMs) {
            XELOGI("[voice] side-channel RX peer {:08X} expired", it->first);
            it = peers_.erase(it);
            continue;
          }
          p->PullFrame(peer_pcm.data(), frame);
          // Mix, and in the same pass accumulate energy for the overlay's
          // per-peer "speaking" level (smoothed one-pole; ~0 while silent/gated).
          double acc = 0.0;
          for (size_t i = 0; i < frame; ++i) {
            const int16_t s = peer_pcm[i];
            mix[i] += s;
            acc += static_cast<double>(s) * s;
          }
          const float rms =
              static_cast<float>(std::sqrt(acc / frame)) * (1.0f / 32768.0f);
          p->level += (rms - p->level) * 0.35f;
          ++it;
        }
      }
      const float out_g = cvars::voice_output_gain * 0.01f;
      double out_acc = 0.0;
      for (size_t i = 0; i < frame; ++i) {
        out[i] = SoftLimitToPcm(mix[i] * out_g);
        out_acc += static_cast<double>(out[i]) * out[i];
      }
      // Note real peer output so the send thread can duck the mic (feedback
      // guard). ~200 RMS floor keeps decode hiss / silence from triggering it.
      if (std::sqrt(out_acc / frame) > 200.0) {
        far_end_active_ms_.store(NowMs(), std::memory_order_relaxed);
      }
      output_->SubmitPcm(out.data(), frame);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void VoiceChannel::OnVoicePacket(uint32_t src_ina, const uint8_t* data,
                                 size_t len) {
  if (len <= sizeof(VoiceHeader)) {
    return;
  }
  const auto* h = reinterpret_cast<const VoiceHeader*>(data);
  if (h->version != kVersion) {
    return;
  }
  const uint8_t* body = data + sizeof(VoiceHeader);
  const size_t blen = len - sizeof(VoiceHeader);
  const uint16_t seq = h->seq;
  const int64_t now = NowMs();

  std::lock_guard<std::mutex> lock(peers_mutex_);
  auto it = peers_.find(src_ina);
  if (it == peers_.end()) {
    auto peer = std::make_unique<VoicePeer>();
    peer->codec = apu::VoiceCodec::Create(cvars::voice_codec);
    XELOGI("[voice] side-channel RX peer {:08X} (codec={})", src_ina,
           peer->codec->name());
    it = peers_.emplace(src_ina, std::move(peer)).first;
  }
  it->second->Push(seq, body, blen, now);
}

}  // namespace kernel
}  // namespace xe
