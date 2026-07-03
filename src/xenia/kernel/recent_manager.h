/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_RECENT_MANAGER_H_
#define XENIA_KERNEL_RECENT_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace xe {
namespace kernel {

// One recently-encountered player (server /recent RecentPlayer shape): someone
// you shared a game session with, captured server-side at join time.
struct RecentPlayer {
  uint64_t xuid = 0;
  std::string gamertag;
  std::string last_seen;       // server ISO timestamp (display only)
  uint32_t encounter_count = 0;  // distinct sessions shared
  bool online = false;
};

// Client-side "recent players" feed -- the add-friend feeder for the server-side
// friends flow. Read-mostly and low-urgency, so unlike Party/Friends this is NOT
// a background poll loop: the UI calls Refresh() while the Recent view is open
// (throttled) and reads GetRecent(). Actions go over HTTP. Singleton.
class RecentManager {
 public:
  static RecentManager* Get();

  // Refresh the list if it's stale and no fetch is in flight. Cheap to call
  // every frame while the Recent UI is visible (internally throttled).
  void Refresh();

  // Remove one entry (POST /recent/remove). Detached; drops it locally now.
  bool Remove(uint64_t other_xuid);

  // Wipe the caller's whole recent list (POST /recent/clear). Detached; clears
  // locally immediately.
  bool Clear();

  std::vector<RecentPlayer> GetRecent() const;

 private:
  RecentManager() = default;
  ~RecentManager() = default;

  void DoFetch(uint64_t me);
  void DoRemove(uint64_t me, uint64_t other);
  void DoClear(uint64_t me);
  void HandleBody(const char* body, size_t len);

  static uint64_t LocalOnlineXuid();

  mutable std::mutex mutex_;
  std::vector<RecentPlayer> recent_;
  std::atomic<bool> fetching_{false};
  std::atomic<int64_t> last_fetch_ms_{0};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_RECENT_MANAGER_H_
