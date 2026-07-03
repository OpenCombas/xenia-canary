/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_VOICE_CHANNEL_H_
#define XENIA_KERNEL_VOICE_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xe {
namespace apu {
class VoiceCodec;
class VoiceOutput;
}  // namespace apu
namespace kernel {

// Netplay voice side-channel (Option 2). Captures the host mic, encodes it with
// the selected VoiceCodec (Opus by default), and broadcasts to the current
// PARTY roster's mapped peers over the GNS transport's reserved voice port;
// inbound voice is decoded and played on the host output. Fully independent of
// the title's own voice codec/transport AND of any game session -- the recipient
// set is a party roster supplied by the party manager (SetPartyRoster), not the
// session member list. Singleton; started/stopped by the party manager on
// join/leave.
class VoiceChannel {
 public:
  static VoiceChannel* Get();

  // Start the capture/send thread and register the GNS voice receive handler.
  // Idempotent.
  void Start();
  void Stop();

  // Set the party roster to broadcast to: the synthetic guest INAs of the other
  // party members (the party manager MapPeer()s each member's peer_key and passes
  // its SyntheticInaFromPeerKey here). Empty = not in a party (mic stays gated).
  // Thread-safe; the send thread snapshots it. A non-empty roster also opens the
  // mic (VAD-gated), so party voice works with no game/title voice pump running.
  void SetPartyRoster(std::vector<uint32_t> member_inas);

  // Optional: called from the title's voice pump (XamVoiceSubmitPacket flags==1)
  // to mark the local user actively transmitting. When a game IS driving voice
  // this gates transmission with its VAD/mute/PTT; at the dashboard (no pump) the
  // party roster opens the mic instead.
  void MarkTransmitActive();

  // --- Voice-activity readouts for the party overlay (thread-safe). ---
  // Smoothed recent output level [0,1] for the received peer with this synthetic
  // INA (GNSTransport::SyntheticInaFromPeerKey); 0 if that peer isn't currently
  // heard. The overlay thresholds this into a "speaking" indicator.
  float GetPeerActivity(uint32_t ina) const;
  // True while we're actively transmitting our own party voice (VAD open and a
  // frame actually sent within the last ~250 ms) -- the self "speaking" signal.
  bool IsSelfActive() const;

 private:
  VoiceChannel() = default;
  ~VoiceChannel();

  void SendThreadMain();
  void PlaybackThreadMain();
  void OnVoicePacket(uint32_t src_ina, const uint8_t* data, size_t len);

  std::atomic<bool> running_{false};
  std::thread send_thread_;
  std::thread playback_thread_;
  std::atomic<int64_t> last_active_ms_{0};
  // Last time we actually encoded+sent a party voice frame (VAD-passed). Drives
  // IsSelfActive() for the overlay; distinct from last_active_ms_ (the title's
  // flags==1 pump input).
  std::atomic<int64_t> self_tx_ms_{0};
  // Last time the playback thread emitted real peer audio. The send thread ducks
  // the mic while this is recent (voice_duck) to break the speaker->mic feedback
  // loop.
  std::atomic<int64_t> far_end_active_ms_{0};

  // Current party roster: synthetic guest INAs of the other members to broadcast
  // voice to. Set by the party manager (SetPartyRoster); snapshotted by the send
  // thread. Empty when not in a party.
  std::mutex roster_mutex_;
  std::vector<uint32_t> party_roster_;

  // Receive side: one jitter-buffered Opus decoder per source peer (correct
  // stateful decode for up to a full 20-player lobby), mixed to one host output.
  mutable std::mutex peers_mutex_;
  std::map<uint32_t, std::unique_ptr<struct VoicePeer>> peers_;
  std::unique_ptr<apu::VoiceOutput> output_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_VOICE_CHANNEL_H_
