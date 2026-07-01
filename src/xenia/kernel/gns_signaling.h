/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GNS_SIGNALING_H_
#define XENIA_KERNEL_GNS_SIGNALING_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/kernel/gns_transport.h"

namespace xe {
namespace kernel {

// Carries GNS custom-signaling blobs over a dedicated standalone WebSocket
// relay (a separate Go service; see docs/gns_integration.md Phase 4b). The
// relay is a dumb, opaque forwarder keyed by peer_key; it never inspects the
// blob.
//
// Wire format — binary WebSocket frames (no JSON/base64):
//   HELLO  (client -> relay): [0x01][u64 LE local_peer_key]
//   SIGNAL (either direction): [0x02][u64 LE peer_key][blob bytes...]
// On an outbound SIGNAL peer_key is the destination; on an inbound SIGNAL it is
// the source. The blob is the GNS rendezvous payload, self-describing to GNS
// (it carries the sender identity), so the inbound source key is informational.
//
// A single worker thread owns the libcurl WebSocket handle and performs all
// curl calls (connect, send, recv, reconnect-with-backoff). SendSignal is safe
// to call from any thread: it enqueues a frame the worker drains.
class StandaloneSignalingBackend : public GNSSignalingBackend {
 public:
  StandaloneSignalingBackend();
  ~StandaloneSignalingBackend() override;

  // Connect to cvars::gns_signaling_url and register local_peer_key. Spawns the
  // worker thread. Returns false (and stays stopped) if no URL is configured.
  // Idempotent: a second call while running is a no-op returning true.
  bool Start(uint64_t local_peer_key);
  void Stop();

  bool running() const { return running_.load(); }

  // GNSSignalingBackend: enqueue an outbound blob for delivery to peer_key.
  void SendSignal(uint64_t peer_key, const uint8_t* data, size_t len) override;

 private:
  void WorkerThread();
  // Establish a websocket connection in CONNECT_ONLY mode. Returns a CURL*
  // (as void* to keep libcurl out of this header) or nullptr on failure.
  void* Connect();
  bool SendFrame(void* curl, const std::vector<uint8_t>& frame);
  void DrainSendQueue(void* curl, bool* connection_ok);
  bool PumpReceive(void* curl);  // false => connection lost/closed
  void HandleInboundFrame(const std::vector<uint8_t>& frame);

  uint64_t local_peer_key_ = 0;
  std::atomic<bool> running_{false};
  std::thread worker_;

  std::mutex send_mutex_;
  std::condition_variable send_cv_;
  std::deque<std::vector<uint8_t>> send_queue_;

  // Reassembly buffer for fragmented inbound websocket frames.
  std::vector<uint8_t> recv_accum_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GNS_SIGNALING_H_
