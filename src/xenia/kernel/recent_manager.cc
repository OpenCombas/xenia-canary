/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/recent_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>

#include "xenia/base/logging.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"  // kernel_state()
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xnet.h"  // HTTP_STATUS_CODE

namespace xe {
namespace kernel {

namespace {

// Refetch at most this often while the Recent view is open.
constexpr int64_t kRefreshCooldownMs = 15000;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string HexXuid(uint64_t xuid) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(xuid));
  return std::string(buf);
}

uint64_t ParseHexU64(const std::string& s) {
  return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 16);
}

std::string GetStr(const rapidjson::Value& obj, const char* key) {
  if (obj.HasMember(key) && obj[key].IsString()) {
    return std::string(obj[key].GetString(), obj[key].GetStringLength());
  }
  return std::string();
}

uint32_t GetU32(const rapidjson::Value& obj, const char* key) {
  if (obj.HasMember(key) && obj[key].IsUint()) {
    return obj[key].GetUint();
  }
  return 0;
}

// Accept online as a JSON bool, number (0/1), or string ("true"/"1").
bool GetBool(const rapidjson::Value& obj, const char* key) {
  if (!obj.HasMember(key)) {
    return false;
  }
  const rapidjson::Value& v = obj[key];
  if (v.IsBool()) {
    return v.GetBool();
  }
  if (v.IsNumber()) {
    return v.GetDouble() != 0.0;
  }
  if (v.IsString()) {
    const std::string s(v.GetString(), v.GetStringLength());
    return s == "true" || s == "1";
  }
  return false;
}

XLiveAPI* Api() {
  return kernel_state() ? kernel_state()->GetXboxLiveAPI() : nullptr;
}

bool HttpOk(uint64_t code) {
  return code == HTTP_STATUS_CODE::HTTP_OK ||
         code == HTTP_STATUS_CODE::HTTP_CREATED;
}

}  // namespace

RecentManager* RecentManager::Get() {
  static RecentManager instance;
  return &instance;
}

uint64_t RecentManager::LocalOnlineXuid() {
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
    if (p && p->GetOnlineXUID()) {
      return p->GetOnlineXUID();
    }
  }
  return 0;
}

void RecentManager::Refresh() {
  const uint64_t me = LocalOnlineXuid();
  if (!me) {
    return;
  }
  if (NowMs() - last_fetch_ms_.load() < kRefreshCooldownMs) {
    return;  // still fresh
  }
  bool expected = false;
  if (!fetching_.compare_exchange_strong(expected, true)) {
    return;  // a fetch is already in flight
  }
  last_fetch_ms_.store(NowMs());
  std::thread(&RecentManager::DoFetch, this, me).detach();
}

void RecentManager::DoFetch(uint64_t me) {
  auto* api = Api();
  if (api) {
    auto resp = api->Get(
        XLiveAPI::BuildEndpoint("recent?xuid=" + HexXuid(me) + "&limit=25"));
    if (resp && HttpOk(resp->StatusCode())) {
      const auto& raw = resp->RawResponse();
      if (raw.response && raw.size) {
        HandleBody(raw.response, raw.size);
      }
    }
  }
  fetching_.store(false);
}

void RecentManager::HandleBody(const char* body, size_t len) {
  rapidjson::Document d;
  d.Parse(body, len);
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("recent") ||
      !d["recent"].IsArray()) {
    return;
  }
  std::vector<RecentPlayer> list;
  for (const auto& v : d["recent"].GetArray()) {
    if (!v.IsObject()) {
      continue;
    }
    RecentPlayer rp;
    rp.xuid = ParseHexU64(GetStr(v, "xuid"));
    rp.gamertag = GetStr(v, "gamertag");
    rp.last_seen = GetStr(v, "lastSeen");
    rp.encounter_count = GetU32(v, "encounterCount");
    rp.online = GetBool(v, "online");
    if (rp.xuid) {
      list.push_back(std::move(rp));
    }
  }
  // TEMP diagnostic (presence investigation): what /recent reports for online.
  XELOGI("[recent] {} player(s) from server", list.size());
  for (const auto& rp : list) {
    XELOGI("[recent]   '{}' xuid={:016X} online={} enc={}",
           rp.gamertag.empty() ? "?" : rp.gamertag, rp.xuid, rp.online,
           rp.encounter_count);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  recent_ = std::move(list);
}

bool RecentManager::Remove(uint64_t other_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !other_xuid) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const RecentPlayer& r) {
                                   return r.xuid == other_xuid;
                                 }),
                  recent_.end());
  }
  std::thread(&RecentManager::DoRemove, this, me, other_xuid).detach();
  return true;
}

void RecentManager::DoRemove(uint64_t me, uint64_t other) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\",\"otherXuid\":\"" +
                           HexXuid(other) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("recent/remove"),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[recent] removed {:016X}", other);
  } else {
    XELOGW("[recent] remove failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

bool RecentManager::Clear() {
  const uint64_t me = LocalOnlineXuid();
  if (!me) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.clear();
  }
  std::thread(&RecentManager::DoClear, this, me).detach();
  return true;
}

void RecentManager::DoClear(uint64_t me) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("recent/clear"),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[recent] cleared");
  } else {
    XELOGW("[recent] clear failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

std::vector<RecentPlayer> RecentManager::GetRecent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recent_;
}

}  // namespace kernel
}  // namespace xe
