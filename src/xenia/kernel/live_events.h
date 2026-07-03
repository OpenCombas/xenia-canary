/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_LIVE_EVENTS_H_
#define XENIA_KERNEL_LIVE_EVENTS_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace kernel {

// Client for the WebServices live-events WebSocket (server->client push for
// presence / party / friends), replacing the dashboard poll loops. Connects to
// wss://<webservices host>/events?xuid=<our online XUID> as a raw (text-JSON)
// WebSocket, keyed by identity -- a SECOND, distinct connection from the GNS
// signaling relay (which is peer_key-keyed and Xbox-unaware).
//
// The server pushes JSON frames { "type": ..., "payload": ... }: a `snapshot` on
// connect (full current state) then deltas (`presence`, `party.invite`,
// `party.roster`, `party.dissolved`, `friend.request`, `friend.accepted`,
// `friend.removed`). All client->server ACTIONS stay HTTP; the only client->
// server socket traffic is ping/pong. Liveness = the connection.
//
// A single worker thread owns the libcurl WebSocket handle (connect / recv /
// reconnect-with-backoff / ping), mirroring StandaloneSignalingBackend. Phase 1:
// transport + dispatch to a handler (default logs). The party/friends managers
// register the handler in later phases. Opt-in via the `live_events` cvar until
// the server gateway is live. Singleton; started from XLiveAPI::StartGNS.
class LiveEventsClient {
 public:
  static LiveEventsClient* Get();

  // Called for each complete inbound message (the raw JSON text). The party and
  // friends managers each add one and filter by the message's `type`; until any
  // is added, messages are just logged. Handlers run on the WS worker thread.
  using Handler = std::function<void(const std::string& message_json)>;
  void AddHandler(Handler handler);

  void Start();  // no-op unless cvars::live_events; idempotent
  void Stop();

  bool connected() const { return connected_.load(); }

 private:
  LiveEventsClient() = default;
  ~LiveEventsClient();

  void WorkerThread();
  // CURL* (void* to keep curl out of the header). On a failed handshake,
  // *http_status (if non-null) receives the server's HTTP response code (e.g.
  // 401), so the caller can re-authenticate; 0 if none was received.
  void* Connect(const std::string& url, long* http_status);
  bool PumpReceive(void* curl, bool* got_close);  // false => connection error
  bool SendPong(void* curl);
  bool SendPing(void* curl);
  void Dispatch(const std::string& message);

  static uint64_t LocalOnlineXuid();  // 0 if not signed in / offline

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread worker_;

  std::mutex handler_mutex_;
  std::vector<Handler> handlers_;

  std::vector<uint8_t> recv_accum_;  // reassembly for fragmented inbound frames
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_LIVE_EVENTS_H_
