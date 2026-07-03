/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/live_events.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/netplay_auth.h"
#include "xenia/kernel/util/shim_utils.h"  // kernel_state()
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"

#ifdef _WIN32
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/libcurl/include/curl/websockets.h"
#else
#include <curl/curl.h>
#include <curl/websockets.h>
#endif

DEFINE_bool(
    live_events, true,
    "Netplay: connect the WebServices live-events WebSocket (presence / party / "
    "friends push) instead of polling. Takes effect on restart.",
    "Live");

namespace xe {
namespace kernel {

namespace {

using namespace std::chrono_literals;
constexpr auto kClientPingInterval = std::chrono::seconds(15);

// Build wss://<webservices host>/events?xuid=<XUID> from the configured API
// address (e.g. https://test.opencombas.org/). Swaps the scheme to ws/wss and
// drops any path.
std::string BuildEventsUrl(uint64_t xuid) {
  const std::string base = XLiveAPI::GetApiAddress();
  std::string scheme = "wss", host = base;
  const size_t s = base.find("://");
  if (s != std::string::npos) {
    scheme = base.compare(0, s, "http") == 0 ? "ws" : "wss";
    const std::string rest = base.substr(s + 3);
    const size_t slash = rest.find('/');
    host = slash == std::string::npos ? rest : rest.substr(0, slash);
  }
  // ADR-0002 soft rollout: authenticate the socket with ?token= when we hold a
  // bearer token; otherwise fall back to the legacy ?xuid= (server still trusts
  // it during the soft phase). Token is base64url -> URL-safe as-is.
  const std::string token = NetplayAuth::Get()->GetToken(xuid);
  if (!token.empty()) {
    return scheme + "://" + host + "/events?token=" + token;
  }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llX",
                static_cast<unsigned long long>(xuid));
  return scheme + "://" + host + "/events?xuid=" + hex;
}

}  // namespace

LiveEventsClient* LiveEventsClient::Get() {
  static LiveEventsClient instance;
  return &instance;
}

LiveEventsClient::~LiveEventsClient() { Stop(); }

void LiveEventsClient::AddHandler(Handler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  handlers_.push_back(std::move(handler));
}

void LiveEventsClient::Start() {
  if (!cvars::live_events) {
    return;  // opt-in until the server gateway is live
  }
  if (running_.exchange(true)) {
    return;  // already running
  }
  worker_ = std::thread(&LiveEventsClient::WorkerThread, this);
  XELOGI("[events] client started");
}

void LiveEventsClient::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  connected_.store(false);
  XELOGI("[events] client stopped");
}

uint64_t LiveEventsClient::LocalOnlineXuid() {
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

void LiveEventsClient::WorkerThread() {
  xe::threading::set_name("Live Events");
  // Interruptible sleep: 50ms slices so Stop() is prompt.
  auto sleep_ms = [this](int ms) {
    for (int s = 0; s < ms && running_.load(); s += 50) {
      std::this_thread::sleep_for(50ms);
    }
  };

  int backoff_ms = 250;
  constexpr int kMaxBackoffMs = 10000;

  while (running_.load()) {
    const uint64_t xuid = LocalOnlineXuid();
    if (!xuid) {
      sleep_ms(1000);  // not signed in yet; poll for sign-in without backoff
      continue;
    }
    const std::string url = BuildEventsUrl(xuid);
    long http_status = 0;
    CURL* curl = static_cast<CURL*>(Connect(url, &http_status));
    if (!curl) {
      // Token expired / revoked: the socket carried ?token= but the server
      // rejected the handshake. Re-mint from the stored password so the next
      // reconnect uses a fresh token. ReAuth is throttled (30s) and returns ""
      // when it can't get one, so a persistent 401 falls through to backoff
      // instead of tight-looping.
      if (http_status == 401 && !NetplayAuth::Get()->ReAuth(xuid).empty()) {
        XELOGW("[events] handshake rejected (401); re-authed, reconnecting");
        backoff_ms = 250;  // fresh token in hand -- retry promptly
      }
      sleep_ms(backoff_ms);
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }
    backoff_ms = 250;
    connected_.store(true);
    XELOGI("[events] connected: {}", url);
    recv_accum_.clear();

    auto last_ping = std::chrono::steady_clock::now();
    bool ok = true, got_close = false;
    while (running_.load() && ok) {
      if (!PumpReceive(curl, &got_close)) {
        ok = false;
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last_ping >= kClientPingInterval) {
        last_ping = now;
        if (!SendPing(curl)) {
          ok = false;
          break;
        }
      }
      sleep_ms(20);  // brief; keeps inbound recv prompt
    }

    connected_.store(false);
    curl_easy_cleanup(curl);
    if (running_.load()) {
      XELOGW("[events] connection {}; reconnecting",
             got_close ? "closed by server" : "lost");
      sleep_ms(500);  // avoid a hot reconnect loop on immediate close
    }
  }
}

void* LiveEventsClient::Connect(const std::string& url, long* http_status) {
  if (http_status) {
    *http_status = 0;
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    return nullptr;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // CONNECT_ONLY == 2: websocket mode -- the handshake runs during perform, then
  // curl_ws_send/recv are used directly.
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  const CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    // Surface the HTTP status (if the server answered before the upgrade) so the
    // worker can distinguish an auth rejection (401) from a transport failure.
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_status) {
      *http_status = code;
    }
    XELOGW("[events] connect failed: {} (http {})", curl_easy_strerror(res),
           code);
    curl_easy_cleanup(curl);
    return nullptr;
  }
  return curl;
}

bool LiveEventsClient::PumpReceive(void* handle, bool* got_close) {
  CURL* curl = static_cast<CURL*>(handle);
  for (;;) {
    uint8_t buf[8192];
    size_t rlen = 0;
    const struct curl_ws_frame* meta = nullptr;
    const CURLcode res = curl_ws_recv(curl, buf, sizeof(buf), &rlen, &meta);
    if (res == CURLE_AGAIN) {
      return true;  // nothing more pending right now
    }
    if (res != CURLE_OK) {
      XELOGW("[events] recv error: {}", curl_easy_strerror(res));
      return false;
    }
    if (meta && (meta->flags & CURLWS_CLOSE)) {
      *got_close = true;
      return false;
    }
    // Reply to server keepalive PINGs with a PONG; ignore PONGs. Control frames
    // are self-contained and must not enter the JSON reassembly buffer.
    if (meta && (meta->flags & CURLWS_PING)) {
      SendPong(curl);
      continue;
    }
    if (meta && (meta->flags & CURLWS_PONG)) {
      continue;
    }
    recv_accum_.insert(recv_accum_.end(), buf, buf + rlen);
    if (meta && meta->bytesleft == 0) {  // end of the current message
      Dispatch(std::string(recv_accum_.begin(), recv_accum_.end()));
      recv_accum_.clear();
    }
  }
}

bool LiveEventsClient::SendPong(void* handle) {
  CURL* curl = static_cast<CURL*>(handle);
  size_t sent = 0;
  const uint8_t empty = 0;
  const CURLcode res = curl_ws_send(curl, &empty, 0, &sent, 0, CURLWS_PONG);
  return res == CURLE_OK || res == CURLE_AGAIN;
}

bool LiveEventsClient::SendPing(void* handle) {
  CURL* curl = static_cast<CURL*>(handle);
  size_t sent = 0;
  const uint8_t empty = 0;
  const CURLcode res = curl_ws_send(curl, &empty, 0, &sent, 0, CURLWS_PING);
  return res == CURLE_OK || res == CURLE_AGAIN;
}

void LiveEventsClient::Dispatch(const std::string& message) {
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handlers = handlers_;
  }
  if (!handlers.empty()) {
    for (auto& h : handlers) {
      h(message);
    }
    return;
  }
  // No handler registered -> log the event type so the transport is verifiable.
  rapidjson::Document d;
  d.Parse(message.c_str(), message.size());
  std::string type = "?";
  if (!d.HasParseError() && d.IsObject() && d.HasMember("type") &&
      d["type"].IsString()) {
    type.assign(d["type"].GetString(), d["type"].GetStringLength());
  }
  XELOGI("[events] rx type={} ({} bytes) [no handler]", type, message.size());
}

}  // namespace kernel
}  // namespace xe
