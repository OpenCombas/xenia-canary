/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/gns_signaling.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#ifdef _WIN32
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/libcurl/include/curl/websockets.h"
#else
#include <curl/curl.h>
#include <curl/websockets.h>
#endif

DEFINE_string(
    gns_signaling_url, "",
    "WebSocket URL (ws:// or wss://) of the GameNetworkingSockets signaling "
    "relay used for NAT traversal. Empty disables GNS signaling.",
    "Live");

namespace xe {
namespace kernel {

namespace {

constexpr uint8_t kMsgHello = 0x01;
constexpr uint8_t kMsgSignal = 0x02;
constexpr size_t kHeaderLen = 1 + 8;  // type byte + u64 peer_key

void AppendU64LE(std::vector<uint8_t>& buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

}  // namespace

StandaloneSignalingBackend::StandaloneSignalingBackend() = default;

StandaloneSignalingBackend::~StandaloneSignalingBackend() { Stop(); }

bool StandaloneSignalingBackend::Start(uint64_t local_peer_key) {
  if (running_.exchange(true)) {
    return true;  // already running
  }

  if (cvars::gns_signaling_url.empty()) {
    XELOGW(
        "GNS signaling: gns_signaling_url is empty; signaling disabled. GNS "
        "connections cannot be established without a relay.");
    running_ = false;
    return false;
  }

  local_peer_key_ = local_peer_key;
  worker_ = std::thread([this]() { WorkerThread(); });
  return true;
}

void StandaloneSignalingBackend::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  send_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::lock_guard<std::mutex> lock(send_mutex_);
  send_queue_.clear();
}

void StandaloneSignalingBackend::SendSignal(uint64_t peer_key,
                                            const uint8_t* data, size_t len) {
  XELOGI("[GNS] backend enqueue -> key xe:{:016x} ({} bytes)", peer_key, len);
  std::vector<uint8_t> frame;
  frame.reserve(kHeaderLen + len);
  frame.push_back(kMsgSignal);
  AppendU64LE(frame, peer_key);
  frame.insert(frame.end(), data, data + len);

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!running_) {
      return;
    }
    send_queue_.push_back(std::move(frame));
  }
  send_cv_.notify_one();
}

void StandaloneSignalingBackend::WorkerThread() {
  xe::threading::set_name("GNS Signaling");

  int backoff_ms = 250;
  constexpr int kMaxBackoffMs = 5000;

  while (running_) {
    CURL* curl = static_cast<CURL*>(Connect());
    if (!curl) {
      // Wait out the backoff, but wake immediately if we're stopping.
      std::unique_lock<std::mutex> lock(send_mutex_);
      send_cv_.wait_for(lock, std::chrono::milliseconds(backoff_ms),
                        [this]() { return !running_.load(); });
      backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
      continue;
    }
    backoff_ms = 250;

    // Register our identity with the relay.
    std::vector<uint8_t> hello;
    hello.reserve(kHeaderLen);
    hello.push_back(kMsgHello);
    AppendU64LE(hello, local_peer_key_);
    if (!SendFrame(curl, hello)) {
      curl_easy_cleanup(curl);
      continue;
    }
    XELOGI("GNS signaling: connected to {}", cvars::gns_signaling_url);
    recv_accum_.clear();

    bool connection_ok = true;
    while (running_ && connection_ok) {
      DrainSendQueue(curl, &connection_ok);
      if (connection_ok && !PumpReceive(curl)) {
        connection_ok = false;
      }
      if (!connection_ok) {
        break;
      }
      // Wait briefly for outbound work; bounded so inbound recv stays prompt.
      std::unique_lock<std::mutex> lock(send_mutex_);
      if (send_queue_.empty() && running_) {
        send_cv_.wait_for(lock, std::chrono::milliseconds(20));
      }
    }

    curl_easy_cleanup(curl);
    if (running_) {
      XELOGW("GNS signaling: connection lost; reconnecting");
    }
  }
}

void* StandaloneSignalingBackend::Connect() {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return nullptr;
  }

  curl_easy_setopt(curl, CURLOPT_URL, cvars::gns_signaling_url.c_str());
  // CONNECT_ONLY == 2 puts the handle in websocket mode: the handshake runs
  // during curl_easy_perform and afterwards curl_ws_send/recv are used directly.
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    XELOGW("GNS signaling: connect to {} failed: {}", cvars::gns_signaling_url,
           curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    return nullptr;
  }
  return curl;
}

bool StandaloneSignalingBackend::SendFrame(void* handle,
                                           const std::vector<uint8_t>& frame) {
  CURL* curl = static_cast<CURL*>(handle);
  const size_t total = frame.size();
  size_t off = 0;
  int agains = 0;

  while (off < total && running_) {
    size_t sent = 0;
    unsigned int flags = CURLWS_BINARY;
    if (off > 0) {
      flags |= CURLWS_OFFSET;  // continuation of the same binary frame
    }
    CURLcode res = curl_ws_send(curl, frame.data() + off, total - off, &sent,
                                0, flags);
    if (res == CURLE_AGAIN) {
      if (++agains > 2000) {
        return false;  // socket wedged ~seconds -> force reconnect
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (res != CURLE_OK) {
      return false;
    }
    off += sent;
    agains = 0;
  }
  return off >= total;
}

void StandaloneSignalingBackend::DrainSendQueue(void* curl,
                                                bool* connection_ok) {
  for (;;) {
    std::vector<uint8_t> frame;
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      if (send_queue_.empty()) {
        return;
      }
      frame = std::move(send_queue_.front());
      send_queue_.pop_front();
    }
    if (!SendFrame(curl, frame)) {
      *connection_ok = false;
      return;
    }
  }
}

bool StandaloneSignalingBackend::PumpReceive(void* handle) {
  CURL* curl = static_cast<CURL*>(handle);
  for (;;) {
    uint8_t buf[4096];
    size_t rlen = 0;
    const struct curl_ws_frame* meta = nullptr;
    CURLcode res = curl_ws_recv(curl, buf, sizeof(buf), &rlen, &meta);
    if (res == CURLE_AGAIN) {
      return true;  // nothing more pending right now
    }
    if (res != CURLE_OK) {
      XELOGW("GNS signaling: recv error: {}", curl_easy_strerror(res));
      return false;
    }
    if (meta && (meta->flags & CURLWS_CLOSE)) {
      return false;  // relay closed the connection
    }

    recv_accum_.insert(recv_accum_.end(), buf, buf + rlen);
    // bytesleft == 0 marks the end of the current websocket message.
    if (meta && meta->bytesleft == 0) {
      HandleInboundFrame(recv_accum_);
      recv_accum_.clear();
    }
  }
}

void StandaloneSignalingBackend::HandleInboundFrame(
    const std::vector<uint8_t>& frame) {
  if (frame.empty()) {
    return;
  }
  switch (frame[0]) {
    case kMsgSignal: {
      if (frame.size() < kHeaderLen) {
        return;  // malformed
      }
      // frame[1..9) is the source peer_key; GNS recovers the sender identity
      // from the blob itself, so we don't need it here.
      const uint8_t* blob = frame.data() + kHeaderLen;
      const size_t blob_len = frame.size() - kHeaderLen;
      GNSTransport::Get()->DeliverInboundSignal(blob, blob_len);
      break;
    }
    default:
      // Unknown/control frame (e.g. relay error notice) — ignore for now.
      break;
  }
}

}  // namespace kernel
}  // namespace xe
