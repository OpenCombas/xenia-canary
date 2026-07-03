/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_FRIENDS_MANAGER_H_
#define XENIA_KERNEL_FRIENDS_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace kernel {

// An accepted friend plus their presence (the server's FriendPresence shape,
// which mirrors /players/presence).
struct FriendEntry {
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t state = 0;      // X_ONLINE_FRIENDSTATE_* presence bitmask
  uint32_t title_id = 0;   // title the friend is in, 0 if none
  std::string session_id;  // hex session id (for a future join-friend)
  std::string rich_presence;
  // Online is the FLAG_ONLINE bit specifically, NOT "state != 0": an offline
  // friend can still carry other bits (console type 0x1000, invite/request
  // flags, ...), so a bare non-zero test reads them as online.
  // (X_ONLINE_FRIENDSTATE_FLAG_ONLINE from xnet.h.)
  bool online() const { return (state & 0x00000001u) != 0; }
};

// A pending friend request. For an incoming request `xuid`/`gamertag` are the
// requester; for an outgoing one they're the target.
struct FriendRequest {
  uint64_t xuid = 0;
  std::string gamertag;
};

// Client-side friends manager. Mirrors PartyManager: a server-owned friend graph
// under the WebServices /friends module (operator chose a FRESH START -- this is
// NOT the title-facing UserProfile::friends_; it drives the netplay friends UI
// only). Live-events WS is the primary source (snapshot + friend.* deltas); a
// dashboard-level poll of GET /friends is the fallback + liveness heartbeat.
// Actions (request/accept/decline/cancel/remove) go over HTTP. Singleton;
// started from XLiveAPI::StartGNS.
class FriendsManager {
 public:
  static FriendsManager* Get();

  void Start();
  void Stop();

  // --- UI actions. Each fires the POST on a detached worker (the UI thread never
  // blocks on the network); state reconciles on the next poll / WS event. Return
  // false only on a local precondition failure (e.g. not signed in). ---
  bool RequestByXuid(uint64_t target_xuid);
  bool RequestByGamertag(const std::string& gamertag);
  bool Accept(uint64_t other_xuid);   // accept an incoming request
  bool Decline(uint64_t other_xuid);  // reject an incoming request
  bool Cancel(uint64_t other_xuid);   // withdraw an outgoing request
  bool Remove(uint64_t other_xuid);   // unfriend an accepted friend

  // --- Thread-safe snapshots for the UI. ---
  std::vector<FriendEntry> GetFriends() const;
  std::vector<FriendRequest> GetIncoming() const;
  std::vector<FriendRequest> GetOutgoing() const;
  size_t IncomingCount() const;  // unacked incoming requests (for a tab badge)

 private:
  FriendsManager() = default;
  ~FriendsManager();

  void PollThreadMain();
  void PollOnce(uint64_t me);  // one GET /friends + apply

  // Worker bodies for the UI actions (run detached).
  void DoRequestXuid(uint64_t me, uint64_t target);
  void DoRequestGamertag(uint64_t me, std::string gamertag);
  void DoAccept(uint64_t me, uint64_t other);
  void DoDecline(uint64_t me, uint64_t other);
  void DoCancel(uint64_t me, uint64_t other);
  void DoRemove(uint64_t me, uint64_t other);

  // Parse a GET /friends body ({friends, incoming, outgoing}) -> full replace.
  void HandlePollBody(const char* body, size_t len);

  // Live-events WS handler (primary path): snapshot / friend.request /
  // friend.accepted / friend.removed / presence. Registered in Start().
  void OnLiveEvent(const std::string& message_json);

  // Replace the whole friends/incoming/outgoing set (poll or WS snapshot).
  void ApplyFull(std::vector<FriendEntry> friends,
                 std::vector<FriendRequest> incoming,
                 std::vector<FriendRequest> outgoing);

  static uint64_t LocalOnlineXuid();

  std::atomic<bool> running_{false};
  std::thread poll_thread_;

  mutable std::mutex mutex_;
  std::vector<FriendEntry> friends_;
  std::vector<FriendRequest> incoming_;
  std::vector<FriendRequest> outgoing_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_FRIENDS_MANAGER_H_
