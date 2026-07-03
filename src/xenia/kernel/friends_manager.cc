/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/friends_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Match the kernel's JSON headers so the rapidjson std::string support macro
// isn't redefined (they set it before including rapidjson).
#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/live_events.h"
#include "xenia/kernel/util/shim_utils.h"  // kernel_state()
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xnet.h"  // HTTP_STATUS_CODE
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"

DEFINE_bool(
    friend_request_notify, true,
    "Netplay: pop an on-screen notification for an incoming friend request and "
    "when someone accepts your request (in addition to the Friends tab).",
    "Live");

namespace xe {
namespace kernel {

namespace {

std::string HexXuid(uint64_t xuid) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(xuid));
  return std::string(buf);
}

uint64_t ParseHexU64(const std::string& s) {
  if (s.empty()) {
    return 0;
  }
  return std::strtoull(s.c_str(), nullptr, 16);
}

std::string GetStr(const rapidjson::Value& obj, const char* key) {
  if (obj.HasMember(key) && obj[key].IsString()) {
    return std::string(obj[key].GetString(), obj[key].GetStringLength());
  }
  return std::string();
}

// Presence numbers (state, titleId) come as either a JSON number or a hex
// string depending on the field; accept both.
uint32_t GetU32(const rapidjson::Value& obj, const char* key) {
  if (!obj.HasMember(key)) {
    return 0;
  }
  const rapidjson::Value& v = obj[key];
  if (v.IsUint()) {
    return v.GetUint();
  }
  if (v.IsUint64()) {
    return static_cast<uint32_t>(v.GetUint64());
  }
  if (v.IsString()) {
    return static_cast<uint32_t>(
        std::strtoull(std::string(v.GetString(), v.GetStringLength()).c_str(),
                      nullptr, 16));
  }
  return 0;
}

// The signed-in user's online XUID, or 0 if not signed in / offline.
uint64_t OnlineXuidNow() {
  if (!kernel_state()) {
    return 0;
  }
  auto* xam = kernel_state()->xam_state();
  if (!xam) {
    return 0;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!xam->IsUserSignedIn(i)) {
      continue;
    }
    xam::UserProfile* p = xam->GetUserProfile(i);
    if (p) {
      const uint64_t x = p->GetOnlineXUID();
      if (x) {
        return x;
      }
    }
  }
  return 0;
}

XLiveAPI* Api() {
  return kernel_state() ? kernel_state()->GetXboxLiveAPI() : nullptr;
}

// The /friends endpoints reply 200 (GET) or 201 Created (POST, NestJS default).
bool HttpOk(uint64_t code) {
  return code == HTTP_STATUS_CODE::HTTP_OK ||
         code == HTTP_STATUS_CODE::HTTP_CREATED;
}

// Fire-and-forget host notification "toast". Safe from the WS worker thread: it
// hops to the UI thread and re-checks the emulator/drawer on the way.
void ShowFriendToast(const std::string& title, const std::string& body) {
  if (!cvars::friend_request_notify || !kernel_state()) {
    return;
  }
  Emulator* emulator = kernel_state()->emulator();
  if (!emulator || !emulator->display_window() || !emulator->imgui_drawer()) {
    return;
  }
  emulator->display_window()->app_context().CallInUIThread(
      [emulator, title, body]() {
        if (!emulator->imgui_drawer()) {
          return;
        }
        new xe::ui::HostNotificationWindow(emulator->imgui_drawer(), title, body,
                                           0);
      });
}

// Parse a FriendPresence object into a FriendEntry.
FriendEntry ParseFriend(const rapidjson::Value& v) {
  FriendEntry f;
  f.xuid = ParseHexU64(GetStr(v, "xuid"));
  f.gamertag = GetStr(v, "gamertag");
  f.state = GetU32(v, "state");
  f.title_id = GetU32(v, "titleId");
  f.session_id = GetStr(v, "sessionId");
  f.rich_presence = GetStr(v, "richPresence");
  return f;
}

// Parse a [{xuid|gamertag}] request array keyed by the given id/tag field names
// (incoming uses from*, outgoing uses to*).
std::vector<FriendRequest> ParseRequests(const rapidjson::Value& arr,
                                         const char* xuid_key,
                                         const char* tag_key) {
  std::vector<FriendRequest> out;
  if (!arr.IsArray()) {
    return out;
  }
  for (const auto& v : arr.GetArray()) {
    if (!v.IsObject()) {
      continue;
    }
    FriendRequest r;
    r.xuid = ParseHexU64(GetStr(v, xuid_key));
    r.gamertag = GetStr(v, tag_key);
    if (r.xuid) {
      out.push_back(std::move(r));
    }
  }
  return out;
}

}  // namespace

FriendsManager* FriendsManager::Get() {
  static FriendsManager instance;
  return &instance;
}

FriendsManager::~FriendsManager() { Stop(); }

void FriendsManager::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  poll_thread_ = std::thread(&FriendsManager::PollThreadMain, this);
  // Subscribe to the live-events WS (primary path). The poll loop backs off while
  // the WS is connected; registering is harmless if live_events is off.
  LiveEventsClient::Get()->AddHandler(
      [](const std::string& msg) { FriendsManager::Get()->OnLiveEvent(msg); });
  XELOGI("[friends] manager started (poll loop up)");
}

void FriendsManager::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }
  XELOGI("[friends] manager stopped");
}

uint64_t FriendsManager::LocalOnlineXuid() { return OnlineXuidNow(); }

void FriendsManager::PollThreadMain() {
  xe::threading::set_name("Friends Poll");
  using namespace std::chrono_literals;
  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t me = LocalOnlineXuid();
    const bool ws_active = LiveEventsClient::Get()->connected();
    // Poll even while the WS is up, just slowly. The WS delivers instant
    // friend.*/presence deltas, BUT a friend added mid-session via
    // friend.accepted arrives with NO presence (that event only carries
    // xuid+gamertag), and a delta can be missed. A periodic reconciliation
    // GET /friends -- authoritative, presence included -- self-heals both. When
    // the WS is off it's the sole source, so it runs fast.
    if (me) {
      PollOnce(me);
    }
    const auto interval = ws_active ? 30s : 8s;
    for (auto slept = 0ms; slept < interval && running_.load(); slept += 200ms) {
      std::this_thread::sleep_for(200ms);
    }
  }
}

void FriendsManager::PollOnce(uint64_t me) {
  auto* api = Api();
  if (!api) {
    return;
  }
  auto resp = api->Get(XLiveAPI::BuildEndpoint("friends?xuid=" + HexXuid(me)));
  if (!resp || !HttpOk(resp->StatusCode())) {
    return;  // transient; next poll retries
  }
  const auto& raw = resp->RawResponse();
  if (raw.response && raw.size) {
    HandlePollBody(raw.response, raw.size);
  }
}

void FriendsManager::HandlePollBody(const char* body, size_t len) {
  rapidjson::Document d;
  d.Parse(body, len);
  if (d.HasParseError() || !d.IsObject()) {
    return;
  }
  std::vector<FriendEntry> friends;
  if (d.HasMember("friends") && d["friends"].IsArray()) {
    for (const auto& v : d["friends"].GetArray()) {
      if (v.IsObject()) {
        FriendEntry f = ParseFriend(v);
        if (f.xuid) {
          friends.push_back(std::move(f));
        }
      }
    }
  }
  std::vector<FriendRequest> incoming, outgoing;
  if (d.HasMember("incoming")) {
    incoming = ParseRequests(d["incoming"], "fromXuid", "fromGamertag");
  }
  if (d.HasMember("outgoing")) {
    outgoing = ParseRequests(d["outgoing"], "toXuid", "toGamertag");
  }
  ApplyFull(std::move(friends), std::move(incoming), std::move(outgoing));
}

void FriendsManager::ApplyFull(std::vector<FriendEntry> friends,
                               std::vector<FriendRequest> incoming,
                               std::vector<FriendRequest> outgoing) {
  // TEMP diagnostic (server presence/friends investigation): what GET /friends
  // actually returns for us right now -- friend count + each friend's raw state.
  // Remove once the server-side offline-friend behaviour is settled.
  XELOGI("[friends] roster from server: {} friend(s), {} incoming, {} outgoing",
         friends.size(), incoming.size(), outgoing.size());
  for (const auto& f : friends) {
    XELOGI("[friends]   '{}' xuid={:016X} state={:08X} title={:08X}",
           f.gamertag.empty() ? "?" : f.gamertag, f.xuid, f.state, f.title_id);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  friends_ = std::move(friends);
  incoming_ = std::move(incoming);
  outgoing_ = std::move(outgoing);
}

void FriendsManager::OnLiveEvent(const std::string& message) {
  rapidjson::Document d;
  d.Parse(message.c_str(), message.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("type") ||
      !d["type"].IsString()) {
    return;
  }
  const std::string type(d["type"].GetString(), d["type"].GetStringLength());
  const rapidjson::Value* payload =
      d.HasMember("payload") && d["payload"].IsObject() ? &d["payload"] : nullptr;

  if (type == "snapshot") {
    // friends/incoming/outgoing portions (party.* are the PartyManager's).
    std::vector<FriendEntry> friends;
    if (payload && payload->HasMember("friends") &&
        (*payload)["friends"].IsArray()) {
      for (const auto& v : (*payload)["friends"].GetArray()) {
        if (v.IsObject()) {
          FriendEntry f = ParseFriend(v);
          if (f.xuid) {
            friends.push_back(std::move(f));
          }
        }
      }
    }
    std::vector<FriendRequest> incoming, outgoing;
    if (payload && payload->HasMember("incoming")) {
      incoming = ParseRequests((*payload)["incoming"], "fromXuid",
                               "fromGamertag");
    }
    if (payload && payload->HasMember("outgoing")) {
      outgoing = ParseRequests((*payload)["outgoing"], "toXuid", "toGamertag");
    }
    ApplyFull(std::move(friends), std::move(incoming), std::move(outgoing));
  } else if (type == "friend.request") {
    if (!payload) {
      return;
    }
    FriendRequest r;
    r.xuid = ParseHexU64(GetStr(*payload, "fromXuid"));
    r.gamertag = GetStr(*payload, "fromGamertag");
    if (!r.xuid) {
      return;
    }
    bool is_new = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const bool dup = std::any_of(
          incoming_.begin(), incoming_.end(),
          [&](const FriendRequest& x) { return x.xuid == r.xuid; });
      if (!dup) {
        incoming_.push_back(r);
        is_new = true;
      }
    }
    if (is_new) {
      const std::string who =
          r.gamertag.empty() ? HexXuid(r.xuid) : r.gamertag;
      ShowFriendToast("Friend Request", who + " wants to be your friend");
    }
  } else if (type == "friend.accepted") {
    if (!payload) {
      return;
    }
    const uint64_t xuid = ParseHexU64(GetStr(*payload, "xuid"));
    const std::string gamertag = GetStr(*payload, "gamertag");
    if (!xuid) {
      return;
    }
    bool i_requested = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Fires on both sides: drop the pending both ways, add to friends.
      i_requested = std::any_of(
          outgoing_.begin(), outgoing_.end(),
          [&](const FriendRequest& x) { return x.xuid == xuid; });
      outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
                                     [&](const FriendRequest& x) {
                                       return x.xuid == xuid;
                                     }),
                      outgoing_.end());
      incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(),
                                     [&](const FriendRequest& x) {
                                       return x.xuid == xuid;
                                     }),
                      incoming_.end());
      const bool have = std::any_of(
          friends_.begin(), friends_.end(),
          [&](const FriendEntry& f) { return f.xuid == xuid; });
      if (!have) {
        FriendEntry f;
        f.xuid = xuid;
        f.gamertag = gamertag;
        friends_.push_back(std::move(f));
      }
    }
    // This event carries no presence, so the new friend would show offline until
    // the next reconciliation poll. Pull authoritative state now (detached; the
    // WS worker thread must not block on HTTP).
    if (const uint64_t me = LocalOnlineXuid()) {
      std::thread(&FriendsManager::PollOnce, this, me).detach();
    }
    // Only toast the requester ("your request was accepted"); the accepter
    // already knows -- they just clicked Accept.
    if (i_requested) {
      const std::string who = gamertag.empty() ? HexXuid(xuid) : gamertag;
      ShowFriendToast("Friend Added", who + " accepted your friend request");
    }
  } else if (type == "friend.removed") {
    if (!payload) {
      return;
    }
    const uint64_t xuid = ParseHexU64(GetStr(*payload, "xuid"));
    if (!xuid) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    friends_.erase(std::remove_if(friends_.begin(), friends_.end(),
                                  [&](const FriendEntry& f) {
                                    return f.xuid == xuid;
                                  }),
                   friends_.end());
    incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(),
                                   [&](const FriendRequest& x) {
                                     return x.xuid == xuid;
                                   }),
                    incoming_.end());
    outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
                                   [&](const FriendRequest& x) {
                                     return x.xuid == xuid;
                                   }),
                    outgoing_.end());
  } else if (type == "presence") {
    // A friend's presence changed. Best-effort: update the matching friend from
    // whatever FriendPresence fields are present.
    if (!payload) {
      return;
    }
    const uint64_t xuid = ParseHexU64(GetStr(*payload, "xuid"));
    if (!xuid) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& f : friends_) {
      if (f.xuid == xuid) {
        f.state = GetU32(*payload, "state");
        f.title_id = GetU32(*payload, "titleId");
        f.session_id = GetStr(*payload, "sessionId");
        f.rich_presence = GetStr(*payload, "richPresence");
        const std::string g = GetStr(*payload, "gamertag");
        if (!g.empty()) {
          f.gamertag = g;
        }
        break;
      }
    }
  }
}

// --- UI actions ------------------------------------------------------------

bool FriendsManager::RequestByXuid(uint64_t target_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !target_xuid || target_xuid == me) {
    return false;
  }
  std::thread(&FriendsManager::DoRequestXuid, this, me, target_xuid).detach();
  return true;
}

bool FriendsManager::RequestByGamertag(const std::string& gamertag) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || gamertag.empty()) {
    return false;
  }
  std::thread(&FriendsManager::DoRequestGamertag, this, me, gamertag).detach();
  return true;
}

bool FriendsManager::Accept(uint64_t other_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !other_xuid) {
    return false;
  }
  std::thread(&FriendsManager::DoAccept, this, me, other_xuid).detach();
  return true;
}

bool FriendsManager::Decline(uint64_t other_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !other_xuid) {
    return false;
  }
  std::thread(&FriendsManager::DoDecline, this, me, other_xuid).detach();
  return true;
}

bool FriendsManager::Cancel(uint64_t other_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !other_xuid) {
    return false;
  }
  std::thread(&FriendsManager::DoCancel, this, me, other_xuid).detach();
  return true;
}

bool FriendsManager::Remove(uint64_t other_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !other_xuid) {
    return false;
  }
  std::thread(&FriendsManager::DoRemove, this, me, other_xuid).detach();
  return true;
}

void FriendsManager::DoRequestXuid(uint64_t me, uint64_t target) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"fromXuid\":\"" + HexXuid(me) +
                           "\",\"toXuid\":\"" + HexXuid(target) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("friends/request"),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[friends] requested {:016X}", target);
  } else {
    XELOGW("[friends] request failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

void FriendsManager::DoRequestGamertag(uint64_t me, std::string gamertag) {
  auto* api = Api();
  if (!api) {
    return;
  }
  // NOTE: gamertag is user text; it's placed into a JSON string. Gamertags are
  // alnum/space so this is safe, but avoid quotes/backslashes defensively.
  std::string safe;
  safe.reserve(gamertag.size());
  for (char c : gamertag) {
    if (c != '"' && c != '\\') {
      safe.push_back(c);
    }
  }
  const std::string body =
      "{\"fromXuid\":\"" + HexXuid(me) + "\",\"toGamertag\":\"" + safe + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("friends/request"),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[friends] requested '{}'", safe);
  } else {
    XELOGW("[friends] request-by-gamertag failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

// accept/decline/cancel/remove all POST {xuid, otherXuid}; only the path differs.
static void PostPair(const char* path, uint64_t me, uint64_t other) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\",\"otherXuid\":\"" +
                           HexXuid(other) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint(std::string("friends/") + path),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[friends] {} {:016X}", path, other);
  } else {
    XELOGW("[friends] {} failed: {}", path,
           resp ? resp->Message() : std::string("no response"));
  }
}

void FriendsManager::DoAccept(uint64_t me, uint64_t other) {
  PostPair("accept", me, other);
  // Apply locally so the UI updates before the next poll/WS event.
  std::lock_guard<std::mutex> lock(mutex_);
  incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(),
                                 [&](const FriendRequest& x) {
                                   return x.xuid == other;
                                 }),
                  incoming_.end());
}

void FriendsManager::DoDecline(uint64_t me, uint64_t other) {
  PostPair("decline", me, other);
  std::lock_guard<std::mutex> lock(mutex_);
  incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(),
                                 [&](const FriendRequest& x) {
                                   return x.xuid == other;
                                 }),
                  incoming_.end());
}

void FriendsManager::DoCancel(uint64_t me, uint64_t other) {
  PostPair("cancel", me, other);
  std::lock_guard<std::mutex> lock(mutex_);
  outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
                                 [&](const FriendRequest& x) {
                                   return x.xuid == other;
                                 }),
                  outgoing_.end());
}

void FriendsManager::DoRemove(uint64_t me, uint64_t other) {
  PostPair("remove", me, other);
  std::lock_guard<std::mutex> lock(mutex_);
  friends_.erase(std::remove_if(friends_.begin(), friends_.end(),
                                [&](const FriendEntry& f) {
                                  return f.xuid == other;
                                }),
                 friends_.end());
}

// --- UI snapshots ----------------------------------------------------------

std::vector<FriendEntry> FriendsManager::GetFriends() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return friends_;
}

std::vector<FriendRequest> FriendsManager::GetIncoming() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return incoming_;
}

std::vector<FriendRequest> FriendsManager::GetOutgoing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outgoing_;
}

size_t FriendsManager::IncomingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return incoming_.size();
}

}  // namespace kernel
}  // namespace xe
