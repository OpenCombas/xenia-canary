/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/gns_transport.h"

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#ifdef XE_GNS_ENABLED
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

#include "xenia/base/threading.h"

#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

// Defined in our patched GameNetworkingSockets
// (steamnetworkingsockets_ice_client.cpp): restrict ICE host-candidate
// gathering to an allowlist of local addresses. Passing nCount==0 removes any
// restriction.
extern "C" void Xenia_GNS_SetLocalAddrAllowlist(
    const SteamNetworkingIPAddr* pAddrs, int nCount);
// Hard-block (false) or allow (true) IPv6 local interfaces for ICE gathering,
// independent of the allowlist (which is per-family-permissive and can't
// express "no IPv6" on a host that has IPv6). Driven by the gns_ice_ipv6 cvar.
extern "C" void Xenia_GNS_SetIPv6Allowed(bool bAllowed);
#endif  // XE_GNS_ENABLED

DEFINE_bool(gns, false,
            "Route netplay peer traffic over GameNetworkingSockets for NAT "
            "traversal (experimental).",
            "Live");

DEFINE_string(gns_stun_server_list, "stun:stun.l.google.com:19302",
              "Comma-separated STUN server list for GNS ICE NAT traversal.",
              "Live");

DEFINE_string(gns_turn_server_list, "",
              "Comma-separated TURN relay server list for GNS ICE (empty = no "
              "TURN). Used as a fallback when direct P2P fails.",
              "Live");

DEFINE_string(gns_turn_username, "",
              "Username for the GNS TURN relay servers (gns_turn_server_list).",
              "Live");

DEFINE_string(gns_turn_password, "",
              "Password for the GNS TURN relay servers (gns_turn_server_list).",
              "Live");

DEFINE_bool(gns_enable_relay, true,
            "Allow ICE relayed (TURN) candidates as a fallback when a direct "
            "peer-to-peer path can't be established.",
            "Live");

DEFINE_bool(
    gns_ice_allow_public, true,
    "Allow ICE public candidates (STUN-reflexive and public host "
    "addresses) for GNS NAT traversal. Set false to gather only private "
    "host candidates -- useful for LAN-only testing and to avoid "
    "probing public / IPv6 / Teredo paths on multi-homed hosts.",
    "Live");

DEFINE_bool(
    gns_ice_ipv6, false,
    "Gather IPv6 host ICE candidates from the selected adapter. Off by "
    "default: "
    "an ISP-assigned global IPv6 frequently can't actually route to the STUN "
    "servers or peers, so it only adds failed candidate pairs, extra NAT "
    "mappings (UDP flows), and STUN-failure churn. IPv4 host/reflexive/relay "
    "candidates are unaffected. Enable only on a network with working IPv6.",
    "Live");

DEFINE_bool(
    gns_stream, true,
    "Carry TCP-over-GNS streams as reliable messages on a second channel of "
    "the "
    "peer's existing datagram session instead of opening a separate "
    "connection-oriented (ConnectP2P) link per virtual port. This collapses "
    "the "
    "two ICE sessions a peer otherwise needs (one datagram, one stream) into a "
    "single connection, halving that peer's ICE candidate gathering and NAT "
    "mappings -- a likely contributor to the ~4-peer session ceiling. Off by "
    "default: the stream wire format differs, so both console peers of a TCP "
    "stream must run a build with this enabled (a mismatched pair simply falls "
    "back to native for that stream). The datagram channel is unchanged, so "
    "the "
    "server-side gns-gateway is unaffected either way.",
    "Live");

DEFINE_bool(gns_log_packets, false,
            "Log every GNS packet routed through the transport (direction, "
            "peer online IP, destination port, size) at debug level. GNS is "
            "encrypted on the wire so a packet capture is opaque; enable this "
            "to see what netplay is actually sending/receiving. Verbose.",
            "Live");

DEFINE_bool(
    gns_log_verbose, false,
    "Capture GNS's own internal chatter (routine connection lifecycle, "
    "ICE keepalives/timeouts, per-connection perf notes) in addition to "
    "genuine problems. Off by default: only Bug/Error/Important/Warning "
    "GNS output is logged, which keeps the network service thread quiet "
    "during play while still surfacing real issues (e.g. the 'service "
    "thread waited Nms for lock' latency warnings). Turn on to diagnose "
    "connection instability. Either way GNS output uses a non-blocking "
    "log path, so it can never stall the transport thread.",
    "Live");

namespace xe {
namespace kernel {

// Our peers are identified to GNS as generic-string identities of the form
// "xe:" followed by 16 lowercase hex digits of the 64-bit peer key.
namespace {
constexpr char kIdentityPrefix[] = "xe:";
constexpr size_t kIdentityLen = 3 + 16;  // "xe:" + 16 hex
}  // namespace

// ---------------------------------------------------------------------------
// Platform-independent members (no GNS dependency).
// ---------------------------------------------------------------------------
GNSTransport* GNSTransport::Get() {
  static GNSTransport instance;
  return &instance;
}

bool GNSTransport::IsEnabled() { return cvars::gns; }

void GNSTransport::MapPeer(uint32_t guest_ina, uint64_t peer_key) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  ina_to_key_[guest_ina] = peer_key;
  key_to_ina_[peer_key] = guest_ina;
}

void GNSTransport::UnmapPeer(uint32_t guest_ina) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  auto it = ina_to_key_.find(guest_ina);
  if (it != ina_to_key_.end()) {
    key_to_ina_.erase(it->second);
    ina_to_key_.erase(it);
  }
}

bool GNSTransport::IsMapped(uint32_t guest_ina) const {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  return ina_to_key_.find(guest_ina) != ina_to_key_.end();
}

uint32_t GNSTransport::InaFromPeerKey(uint64_t peer_key) const {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  auto it = key_to_ina_.find(peer_key);
  return it != key_to_ina_.end() ? it->second : 0;
}

uint64_t GNSTransport::PeerKeyFromIna(uint32_t guest_ina) const {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  auto it = ina_to_key_.find(guest_ina);
  return it != ina_to_key_.end() ? it->second : 0;
}

uint32_t GNSTransport::SyntheticInaFromPeerKey(uint64_t peer_key) {
  if (!peer_key) {
    return 0;
  }
  // Avalanche the 48-bit MAC into 24 host bits of a 10.0.0.0/8 address.
  uint64_t h = peer_key * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 29;
  uint32_t host24 = static_cast<uint32_t>(h) & 0x00FFFFFFu;
  if (host24 == 0) {
    host24 = 1;  // avoid the 10.0.0.0 network address
  } else if (host24 == 0x00FFFFFFu) {
    host24 = 0x00FFFFFEu;  // avoid the 10.255.255.255 broadcast address
  }
  const uint32_t a = 10;
  const uint32_t b = (host24 >> 16) & 0xFF;
  const uint32_t c = (host24 >> 8) & 0xFF;
  const uint32_t d = host24 & 0xFF;
  // Network byte order: first octet in the low memory byte, matching the value
  // carried in XNADDR::inaOnline.s_addr and used as the registry/SendTo
  // guest_ina.
  return a | (b << 8) | (c << 16) | (d << 24);
}

void GNSTransport::set_auto_register_inbound(bool enabled) {
  auto_register_inbound_.store(enabled);
}

uint32_t GNSTransport::ResolveOrRegisterInbound(uint64_t peer_key) {
  {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    auto it = key_to_ina_.find(peer_key);
    if (it != key_to_ina_.end()) {
      return it->second;
    }
    if (!auto_register_inbound_.load()) {
      return 0;
    }
  }
  // Unknown peer in server mode: assign it a synthetic guest_ina.
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  auto it = key_to_ina_.find(peer_key);  // re-check under the exclusive lock
  if (it != key_to_ina_.end()) {
    return it->second;
  }
  // Deterministic per-MAC address (not a running counter): the console that
  // owns this peer_key advertises the identical value as its online IP, so
  // learning it on inbound here makes host->peer routing agree with what the
  // peer published.
  uint32_t ina = SyntheticInaFromPeerKey(peer_key);
  if (!ina) {
    ina = next_synthetic_ina_++;  // fallback for a degenerate (0) key
  }
  ina_to_key_[ina] = peer_key;
  key_to_ina_[peer_key] = ina;
  return ina;
}

void GNSTransport::SetReceiveHandler(ReceiveHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  receive_handler_ = std::move(handler);
}

void GNSTransport::SetVoiceHandler(VoiceHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  voice_handler_ = std::move(handler);
}

std::vector<uint32_t> GNSTransport::GetMappedPeers() const {
  std::vector<uint32_t> peers;
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  peers.reserve(ina_to_key_.size());
  for (const auto& kv : ina_to_key_) {
    peers.push_back(kv.first);
  }
  return peers;
}

std::vector<uint32_t> GNSTransport::GetConsolePeers() const {
  std::vector<uint32_t> peers;
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  peers.reserve(ina_to_key_.size());
  for (const auto& kv : ina_to_key_) {
    const uint64_t key = kv.second;
    // Console peers carry a bare 48-bit MAC (high 16 bits zero); the server key
    // is tagged in bits 48-63. Skip tagged keys and our own console.
    if (key && (key >> 48) == 0 && key != local_peer_key_) {
      peers.push_back(kv.first);
    }
  }
  return peers;
}

void GNSTransport::SetSignalingBackend(GNSSignalingBackend* backend) {
  signaling_backend_.store(backend);
}

uint64_t GNSTransport::PeerKeyFromIdentityString(const char* generic_string) {
  if (!generic_string) {
    return 0;
  }
  if (std::strncmp(generic_string, kIdentityPrefix, 3) != 0) {
    return 0;
  }
  if (std::strlen(generic_string) != kIdentityLen) {
    return 0;
  }
  uint64_t key = 0;
  for (const char* c = generic_string + 3; *c; ++c) {
    uint64_t digit;
    if (*c >= '0' && *c <= '9') {
      digit = static_cast<uint64_t>(*c - '0');
    } else if (*c >= 'a' && *c <= 'f') {
      digit = static_cast<uint64_t>(*c - 'a' + 10);
    } else {
      return 0;
    }
    key = (key << 4) | digit;
  }
  return key;
}

uint64_t GNSTransport::ServerPeerKeyFromIna(uint32_t ina) {
  // 'SV' marker in bits 48-63 keeps server keys clear of the 48-bit MAC keys
  // used for console peers; the online IP occupies the low 32 bits.
  constexpr uint64_t kServerKeyTag = 0x5356ULL << 48;
  return kServerKeyTag | static_cast<uint64_t>(ina);
}

#ifdef XE_GNS_ENABLED

// ---------------------------------------------------------------------------
// GNS-backed implementation.
// ---------------------------------------------------------------------------

namespace {

using namespace std::chrono_literals;

// Throughput counters (diagnostic). tx is incremented from guest threads in
// SendTo; rx from the pump thread in Service. PumpThread logs the per-interval
// rate so we can see whether GNS is amplifying the guest's datagram traffic.
std::atomic<uint64_t> g_tx_packets{0};
std::atomic<uint64_t> g_tx_bytes{0};
std::atomic<uint64_t> g_rx_packets{0};
std::atomic<uint64_t> g_rx_bytes{0};

// All GNS datagrams travel on a single channel; the guest source/destination
// ports ride in a small framing header so the receiver can route to the
// correct bound socket. (Using the GNS channel for the port would require
// draining each possible channel separately.)
constexpr int kDatagramChannel = 0;
constexpr size_t kFrameHeaderSize = 4;  // be16 src_port + be16 dst_port

// Unified-stream mode (gns_stream): TCP-over-GNS rides this second
// channel of the same per-peer ISteamNetworkingMessages session as reliable,
// ordered messages, instead of a separate ConnectP2P connection. Frames:
//   [op:1][vport:be16]            for OPEN / OPEN_ACK / CLOSE
//   [op:1][vport:be16][bytes...]  for DATA
// The vport is the server's listen port; a logical stream is keyed
// (peer,vport), matching the legacy per-(peer,vport) connection granularity.
constexpr int kStreamChannel = 1;
constexpr size_t kStreamCtrlSize = 3;  // op + be16 vport
enum StreamOp : uint8_t {
  kStreamOpen = 0x01,     // client -> server: open logical stream to vport
  kStreamOpenAck = 0x02,  // server -> client: accepted
  kStreamData = 0x03,     // either way: reliable ordered stream bytes
  kStreamCloseOp = 0x04,  // either way: FIN
};

SteamNetworkingIdentity MakeIdentity(uint64_t peer_key) {
  SteamNetworkingIdentity id;
  id.Clear();
  char buf[kIdentityLen + 1];
  std::snprintf(buf, sizeof(buf), "%s%016" PRIx64, kIdentityPrefix, peer_key);
  id.SetGenericString(buf);
  return id;
}

// Outbound signaling object handed to GNS for a single connection. SendSignal
// is invoked with an opaque blob to deliver to the peer; we forward it to the
// pluggable backend (Phase 4). GNS owns the lifetime and calls Release().
class XeConnectionSignaling : public ISteamNetworkingConnectionSignaling {
 public:
  explicit XeConnectionSignaling(uint64_t peer_key) : peer_key_(peer_key) {}

  bool SendSignal(HSteamNetConnection hConn,
                  const SteamNetConnectionInfo_t& info, const void* pMsg,
                  int cbMsg) override {
    XELOGI("[GNS] SendSignal invoked -> key xe:{:016x} ({} bytes)", peer_key_,
           cbMsg);
    auto* backend = GNSTransport::Get()->signaling_backend();
    if (!backend) {
      XELOGW("[GNS] SendSignal dropped: no signaling backend installed");
      return false;
    }
    uint64_t key = peer_key_;
    if (!key) {
      key = GNSTransport::PeerKeyFromIdentityString(
          info.m_identityRemote.GetGenericString());
    }
    if (!key) {
      return false;
    }
    backend->SendSignal(key, reinterpret_cast<const uint8_t*>(pMsg),
                        static_cast<size_t>(cbMsg));
    return true;
  }

  void Release() override { delete this; }

 private:
  uint64_t peer_key_;
};

// Context used when feeding inbound signals to GNS. OnConnectRequest supplies a
// signaling object for the reply leg; we accept only peers we recognize.
class XeSignalingRecvContext : public ISteamNetworkingSignalingRecvContext {
 public:
  ISteamNetworkingConnectionSignaling* OnConnectRequest(
      HSteamNetConnection hConn, const SteamNetworkingIdentity& identityPeer,
      int nLocalVirtualPort) override {
    uint64_t key = GNSTransport::PeerKeyFromIdentityString(
        identityPeer.GetGenericString());
    if (!key) {
      return nullptr;  // Not one of ours -> reject.
    }
    return new XeConnectionSignaling(key);
  }

  void SendRejectionSignal(const SteamNetworkingIdentity& identityPeer,
                           const void* pMsg, int cbMsg) override {
    auto* backend = GNSTransport::Get()->signaling_backend();
    if (!backend) {
      return;
    }
    uint64_t key = GNSTransport::PeerKeyFromIdentityString(
        identityPeer.GetGenericString());
    if (key) {
      backend->SendSignal(key, reinterpret_cast<const uint8_t*>(pMsg),
                          static_cast<size_t>(cbMsg));
    }
  }
};

XeSignalingRecvContext g_recv_context;

// Global callback GNS uses to obtain a signaling object for locally-initiated
// connections (ConnectP2P and ISteamNetworkingMessages alike).
ISteamNetworkingConnectionSignaling* CreateConnectionSignalingFn(
    ISteamNetworkingSockets* pLocalInterface,
    const SteamNetworkingIdentity& identityPeer, int nLocalVirtualPort,
    int nRemoteVirtualPort) {
  uint64_t key =
      GNSTransport::PeerKeyFromIdentityString(identityPeer.GetGenericString());
  XELOGI(
      "[GNS] CreateConnectionSignaling requested -> key xe:{:016x} (lvport={}, "
      "rvport={})",
      key, nLocalVirtualPort, nRemoteVirtualPort);
  if (!key) {
    XELOGW(
        "[GNS] CreateConnectionSignaling: unrecognized/non-xe identity; "
        "rejecting connection");
    return nullptr;
  }
  return new XeConnectionSignaling(key);
}

// Fired when a peer first sends to us. Accept only recognized peers.
void MessagesSessionRequestFn(
    SteamNetworkingMessagesSessionRequest_t* request) {
  uint64_t key = GNSTransport::PeerKeyFromIdentityString(
      request->m_identityRemote.GetGenericString());
  if (!key) {
    return;
  }
  SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
}

void DebugOutputFn(ESteamNetworkingSocketsDebugOutputType type,
                   const char* msg) {
  // This runs on GNS's internal service thread, which drives all netplay I/O.
  // Route it through the NON-BLOCKING log path: if another thread is flooding
  // the log ring buffer (e.g. debug-level spam with flush_log on), the normal
  // logger would spin-wait for buffer space right here, stalling the service
  // thread -- which GNS itself reports as "service thread waited Nms for lock,
  // this directly adds to network latency". Dropping a diagnostic line is
  // always preferable to adding latency for every peer, so we drop rather than
  // wait. Bug(1)/Error(2) are genuine problems -> Error; anything else -> Info.
  const bool is_error = static_cast<int>(type) <= 2;
  char line[512];
  const int n = std::snprintf(line, sizeof(line), "[GNS] %s", msg ? msg : "");
  if (n <= 0) {
    return;
  }
  const size_t len = static_cast<size_t>(n) < sizeof(line)
                         ? static_cast<size_t>(n)
                         : sizeof(line) - 1;
  xe::logging::AppendLogLineNoBlock(
      is_error ? xe::LogLevel::Error : xe::LogLevel::Info,
      is_error ? xe::logging::kPrefixCharError : xe::logging::kPrefixCharInfo,
      std::string_view(line, len));
}

// --- Connection-oriented (TCP) state (Phase 6) ------------------------------
// GNSTransport is a singleton, so the per-connection stream state lives here as
// file-static rather than bloating the (GNS-free) header with GNS handle types.

enum { kStreamConnecting = 0, kStreamConnected = 1, kStreamClosed = 2 };

// One TCP-over-GNS connection: the GNS handle, the peer, and a byte-stream
// reassembly buffer that turns GNS's discrete reliable messages back into the
// boundary-less stream the guest expects.
struct StreamConn {
  HSteamNetConnection h = k_HSteamNetConnection_Invalid;
  uint64_t peer_key = 0;
  uint16_t listen_vport = 0;  // 0 => client-initiated (outbound)
  std::atomic<int> state{kStreamConnecting};
  std::mutex mutex;            // guards rx / rx_read
  std::condition_variable cv;  // woken on data or state change
  std::vector<uint8_t> rx;     // reassembly buffer
  size_t rx_read = 0;          // consumed prefix of rx
  // Unified-stream mode only (gns_stream): there is no GNS connection
  // handle, so the opaque id we hand callers is a synthetic `handle` and the
  // logical stream is addressed by (peer_key, vport) on the shared Messages
  // session. `h` stays Invalid in this mode.
  uint32_t handle = 0;
  uint16_t vport = 0;
  bool unified = false;
};

// A listen socket bound to a guest TCP port, plus a queue of established
// inbound connections awaiting the guest's Accept().
struct StreamListener {
  HSteamListenSocket h = k_HSteamListenSocket_Invalid;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<HSteamNetConnection> pending;
  bool stopping = false;
};

std::mutex g_streams_mutex;  // guards the three maps below
std::map<HSteamNetConnection, std::shared_ptr<StreamConn>> g_streams;
std::map<uint16_t, std::shared_ptr<StreamListener>> g_listeners;
std::map<HSteamListenSocket, uint16_t> g_listen_vport;

std::shared_ptr<StreamConn> FindStream(HSteamNetConnection h) {
  std::lock_guard<std::mutex> lock(g_streams_mutex);
  auto it = g_streams.find(h);
  return it != g_streams.end() ? it->second : nullptr;
}

// --- Unified-stream helpers (gns_stream) ----------------------------
// Synthetic connection handles for unified streams. Based high to stay clear of
// any real HSteamNetConnection value (the two modes never coexist in one run,
// but a distinct range keeps logs unambiguous). Never returns 0 (= invalid).
std::atomic<uint32_t> g_next_unified_handle{0x40000000u};
uint32_t AllocUnifiedHandle() {
  uint32_t h = g_next_unified_handle.fetch_add(1, std::memory_order_relaxed);
  return h ? h : g_next_unified_handle.fetch_add(1, std::memory_order_relaxed);
}

// Find the logical unified stream to a peer on a vport. Caller holds
// g_streams_mutex.
//
// A remote close only marks a stream kStreamClosed (it isn't erased until the
// local side also closes), so after a fast reconnect a stale closed stream and
// the fresh live stream can briefly coexist for the same (peer, vport). Prefer
// a live stream so inbound data/close frames route to the reconnect and not the
// abandoned stale stream (which would shadow it and stall the peer's
// handshake).
std::shared_ptr<StreamConn> FindUnifiedStreamLocked(uint64_t peer_key,
                                                    uint16_t vport) {
  std::shared_ptr<StreamConn> closed_match;
  for (auto& [handle, c] : g_streams) {
    if (c->unified && c->peer_key == peer_key && c->vport == vport) {
      if (c->state.load() != kStreamClosed) {
        return c;  // live stream wins
      }
      if (!closed_match) {
        closed_match = c;  // remember, but keep looking for a live one
      }
    }
  }
  return closed_match;
}

// Send a stream control/data frame on the reliable stream channel of the peer's
// Messages session. `payload` is appended after the [op][vport] header (DATA);
// null/0 for the control ops. Returns true on EResultOK.
bool SendStreamFrame(uint64_t peer_key, uint8_t op, uint16_t vport,
                     const uint8_t* payload, size_t len) {
  std::vector<uint8_t> frame(kStreamCtrlSize + len);
  frame[0] = op;
  frame[1] = static_cast<uint8_t>(vport >> 8);
  frame[2] = static_cast<uint8_t>(vport & 0xFF);
  if (len) {
    std::memcpy(frame.data() + kStreamCtrlSize, payload, len);
  }
  EResult r = SteamNetworkingMessages()->SendMessageToUser(
      MakeIdentity(peer_key), frame.data(), static_cast<uint32_t>(frame.size()),
      k_nSteamNetworkingSend_Reliable, kStreamChannel);
  return r == k_EResultOK;
}

// Global callback GNS invokes (on the pump thread, during RunCallbacks) for
// every connection-oriented state change: inbound accept, connect completion,
// and close/failure.
void ConnectionStatusChangedFn(
    SteamNetConnectionStatusChangedCallback_t* info) {
  const HSteamNetConnection h = info->m_hConn;
  XELOGI("[GNS] conn status h={} state={} listen={} peer=xe:{:016x}", h,
         static_cast<int>(info->m_info.m_eState), info->m_info.m_hListenSocket,
         GNSTransport::PeerKeyFromIdentityString(
             info->m_info.m_identityRemote.GetGenericString()));
  switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
      const HSteamListenSocket ls = info->m_info.m_hListenSocket;
      if (ls == k_HSteamListenSocket_Invalid) {
        break;  // outbound; StreamConnect already created the StreamConn
      }
      uint16_t vport = 0;
      {
        std::lock_guard<std::mutex> lock(g_streams_mutex);
        auto it = g_listen_vport.find(ls);
        if (it == g_listen_vport.end()) {
          XELOGW("[GNS] inbound conn {} on unknown listen socket {}", h, ls);
          break;  // not one of ours
        }
        vport = it->second;
      }
      uint64_t key = GNSTransport::PeerKeyFromIdentityString(
          info->m_info.m_identityRemote.GetGenericString());
      if (!key ||
          SteamNetworkingSockets()->AcceptConnection(h) != k_EResultOK) {
        XELOGW("[GNS] AcceptConnection failed for conn {} (key {})", h, key);
        SteamNetworkingSockets()->CloseConnection(h, 0, nullptr, false);
        break;
      }
      auto conn = std::make_shared<StreamConn>();
      conn->h = h;
      conn->peer_key = key;
      conn->listen_vport = vport;
      {
        std::lock_guard<std::mutex> lock(g_streams_mutex);
        g_streams[h] = conn;
      }
      XELOGI("[GNS] inbound accepted conn {} vport {}", h, vport);
      break;
    }
    case k_ESteamNetworkingConnectionState_Connected: {
      auto conn = FindStream(h);
      if (!conn) {
        XELOGW("[GNS] connected conn {} not tracked (dropped)", h);
        break;
      }
      conn->state.store(kStreamConnected);
      conn->cv.notify_all();  // wake a StreamConnect waiter
      {
        // Log the ICE route actually selected (direct vs relay, addresses,
        // ping) so we can tell a NAT-traversal issue from a stalled pump.
        char addrbuf[64] = {};
        info->m_info.m_addrRemote.ToString(addrbuf, sizeof(addrbuf), true);
        char detail[2048] = {};
        SteamNetworkingSockets()->GetDetailedConnectionStatus(h, detail,
                                                              sizeof(detail));
        XELOGI("[GNS] conn {} connected, remote={} route:\n{}", h, addrbuf,
               detail);
      }
      if (conn->listen_vport) {
        std::shared_ptr<StreamListener> ln;
        {
          std::lock_guard<std::mutex> lock(g_streams_mutex);
          auto it = g_listeners.find(conn->listen_vport);
          if (it != g_listeners.end()) {
            ln = it->second;
          }
        }
        if (ln) {
          std::lock_guard<std::mutex> lk(ln->mutex);
          ln->pending.push_back(h);
          ln->cv.notify_all();
          XELOGI("[GNS] inbound conn {} queued for accept on vport {}", h,
                 conn->listen_vport);
        } else {
          XELOGW("[GNS] inbound conn {} connected but listener vport {} gone",
                 h, conn->listen_vport);
        }
      }
      break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
      if (auto conn = FindStream(h)) {
        conn->state.store(kStreamClosed);
        conn->cv.notify_all();  // wake blocked StreamRecv -> EOF
      }
      // Required to release the handle; the StreamConn is kept so the guest can
      // still drain buffered bytes and observe the close (StreamClose erases
      // it).
      SteamNetworkingSockets()->CloseConnection(h, 0, nullptr, false);
      break;
    }
    default:
      break;
  }
}

}  // namespace

bool GNSTransport::Initialize(
    uint64_t local_peer_key, uint32_t restrict_local_ipv4,
    const std::vector<std::array<uint8_t, 16>>& restrict_local_ipv6) {
  if (initialized_.load()) {
    return true;
  }

  local_peer_key_ = local_peer_key;
  SteamNetworkingIdentity local_id = MakeIdentity(local_peer_key);

  SteamNetworkingErrMsg err = {};
  if (!GameNetworkingSockets_Init(&local_id, err)) {
    XELOGE("[GNS] GameNetworkingSockets_Init failed: {}", err);
    return false;
  }

  ISteamNetworkingUtils* utils = SteamNetworkingUtils();
  // Quiet by default: only Bug/Error/Important/Warning (keeps the "waited Nms
  // for lock" latency warnings and real errors, drops routine Msg/Verbose
  // connection chatter). gns_log_verbose opts into the full firehose for
  // diagnosing connection instability. DebugOutputFn drops rather than blocks
  // regardless, so the level only affects volume, not transport-thread safety.
  utils->SetDebugOutputFunction(
      cvars::gns_log_verbose ? k_ESteamNetworkingSocketsDebugOutputType_Verbose
                             : k_ESteamNetworkingSocketsDebugOutputType_Warning,
      &DebugOutputFn);
  // SDR-less custom signaling: GNS asks this callback for a signaling object
  // for every locally-initiated connection (incl. ISteamNetworkingMessages).
  utils->SetGlobalConfigValuePtr(
      k_ESteamNetworkingConfig_Callback_CreateConnectionSignaling,
      reinterpret_cast<void*>(&CreateConnectionSignalingFn));
  utils->SetGlobalCallback_MessagesSessionRequest(&MessagesSessionRequestFn);
  // Connection-oriented (TCP) state changes: inbound accept, connect
  // completion, close/failure (Phase 6).
  utils->SetGlobalCallback_SteamNetConnectionStatusChanged(
      &ConnectionStatusChangedFn);

  // NAT traversal via the built-in ICE client. The effective STUN/TURN config
  // comes from WebServices GET /turn (short-lived Cloudflare creds) when set,
  // else the gns_stun_/gns_turn_ cvars. Applied here at init; re-applied by
  // SetIceServers on each credential refresh.
  ApplyIceConfig();

  // Allowed ICE candidate types, assembled from cvars. Private host candidates
  // are always allowed; public (STUN-reflexive / public host) is gated by
  // gns_ice_allow_public, and relay (TURN) by gns_enable_relay. For LAN-only
  // testing, disabling public keeps ICE off public / IPv6 / Teredo paths.
  int ice_enable = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private;
  if (cvars::gns_ice_allow_public) {
    ice_enable |= k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public;
  }
  if (cvars::gns_enable_relay) {
    ice_enable |= k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay;
  }
  utils->SetGlobalConfigValueInt32(
      k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, ice_enable);
  XELOGI("[GNS] ICE candidate types: private{}{} (mask 0x{:x})",
         cvars::gns_ice_allow_public ? " public" : "",
         cvars::gns_enable_relay ? " relay" : "", ice_enable);

  // Kill IPv6 host-candidate gathering entirely unless gns_ice_ipv6 is set. The
  // adapter allowlist below is per-family-permissive (an IPv4-only list leaves
  // IPv6 unconstrained), so on a host that actually has IPv6 addresses it would
  // still bind them and spam failed candidate pairs / WSASendTo errors. This
  // flag is the real switch; it also makes remote IPv6 candidates un-pairable
  // (ICE only pairs same-family), so we never send to an unreachable peer IPv6.
  Xenia_GNS_SetIPv6Allowed(cvars::gns_ice_ipv6);

  // Restrict ICE host-candidate gathering to the app-selected network adapter,
  // if one was chosen. Prevents VPN/virtual NICs (no public route) from being
  // bound as candidates and hammering STUN with WSAENETUNREACH. Zero = no
  // restriction (default; GNS uses all interfaces).
  if (restrict_local_ipv4 || !restrict_local_ipv6.empty()) {
    std::vector<SteamNetworkingIPAddr> allow;
    allow.reserve(1 + restrict_local_ipv6.size());
    if (restrict_local_ipv4) {
      SteamNetworkingIPAddr a = {};
      a.SetIPv4(restrict_local_ipv4, 0);
      allow.push_back(a);
    }
    for (const auto& v6 : restrict_local_ipv6) {
      SteamNetworkingIPAddr a = {};
      std::memcpy(a.m_ipv6, v6.data(), sizeof(a.m_ipv6));
      allow.push_back(a);
    }
    Xenia_GNS_SetLocalAddrAllowlist(allow.data(),
                                    static_cast<int>(allow.size()));
    XELOGI("[GNS] ICE restricted to selected adapter {}.{}.{}.{} (+{} IPv6)",
           (restrict_local_ipv4 >> 24) & 0xFF,
           (restrict_local_ipv4 >> 16) & 0xFF,
           (restrict_local_ipv4 >> 8) & 0xFF, restrict_local_ipv4 & 0xFF,
           restrict_local_ipv6.size());
  } else {
    Xenia_GNS_SetLocalAddrAllowlist(nullptr, 0);  // no restriction
  }

  initialized_.store(true);
  pump_running_.store(true);
  pump_thread_ = std::thread(&GNSTransport::PumpThread, this);

  XELOGI("[GNS] transport initialized (local identity xe:{:016x})",
         local_peer_key);
  return true;
}

void GNSTransport::Shutdown() {
  if (!initialized_.load()) {
    return;
  }
  pump_running_.store(false);
  if (pump_thread_.joinable()) {
    pump_thread_.join();
  }
  // Wake any blocked StreamRecv/StreamAccept and drop all stream state before
  // tearing down the library.
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    for (auto& [h, c] : g_streams) {
      c->state.store(kStreamClosed);
      c->cv.notify_all();
    }
    for (auto& [vp, ln] : g_listeners) {
      std::lock_guard<std::mutex> lk(ln->mutex);
      ln->stopping = true;
      ln->cv.notify_all();
    }
    g_streams.clear();
    g_listeners.clear();
    g_listen_vport.clear();
  }
  GameNetworkingSockets_Kill();
  initialized_.store(false);
  XELOGI("[GNS] transport shut down");
}

namespace {
// Count comma-separated entries in a GNS server list ("" -> 0).
size_t CountCsvEntries(const std::string& s) {
  if (s.empty()) {
    return 0;
  }
  return static_cast<size_t>(std::count(s.begin(), s.end(), ',')) + 1;
}

// GNS requires the TURN user/pass lists to be parallel and equal-length to the
// server list; if they differ it discards ALL creds (unauthenticated relay).
// WebServices hands us a SINGLE credential for every TURN url, so repeat it to
// `n` entries. A value that already contains commas is assumed to be a caller-
// supplied parallel list and passed through untouched.
std::string RepeatCredentialToCount(const std::string& cred, size_t n) {
  if (cred.empty() || n <= 1 || cred.find(',') != std::string::npos) {
    return cred;
  }
  std::string out;
  out.reserve((cred.size() + 1) * n);
  for (size_t i = 0; i < n; ++i) {
    if (i) {
      out.push_back(',');
    }
    out += cred;
  }
  return out;
}

// GNS's ICE client only understands bare "turn:host:port" / "stun:host:port"
// over UDP. Its parser strips the exact "turn:"/"stun:" prefix, then hands the
// rest to getaddrinfo splitting on ':' -- so it mis-reads "turns:" (TLS) as the
// hostname, and passes a "3478?transport=udp" query suffix straight through as
// the service string, which getaddrinfo rejects (the failure is then logged
// against the hostname, misleadingly). Cloudflare/RFC-7065 ICE URLs carry both.
// Normalize each comma-separated entry to what GNS accepts:
//   - drop "turns:"/"stuns:" (TLS) entries -- GNS has no TLS transport
//   - drop "?transport=tcp" entries -- GNS ICE is UDP-only
//   - strip any "?..." query suffix from the survivors
// Returns the filtered, comma-joined list (possibly empty).
std::string NormalizeIceServerList(const std::string& csv) {
  std::string out;
  size_t pos = 0;
  while (pos <= csv.size()) {
    const size_t comma = csv.find(',', pos);
    const size_t end = (comma == std::string::npos) ? csv.size() : comma;
    std::string entry = csv.substr(pos, end - pos);
    pos = end + 1;

    // Trim surrounding whitespace.
    const size_t b = entry.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
      continue;
    }
    const size_t e = entry.find_last_not_of(" \t\r\n");
    entry = entry.substr(b, e - b + 1);

    std::string lower = entry;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Drop TLS schemes and TCP transports GNS can't use.
    if (lower.rfind("turns:", 0) == 0 || lower.rfind("stuns:", 0) == 0) {
      continue;
    }
    if (lower.find("transport=tcp") != std::string::npos) {
      continue;
    }
    // Strip the query suffix (?transport=udp, etc.).
    const size_t q = entry.find('?');
    if (q != std::string::npos) {
      entry = entry.substr(0, q);
    }
    if (entry.empty()) {
      continue;
    }

    if (!out.empty()) {
      out.push_back(',');
    }
    out += entry;
  }
  return out;
}
}  // namespace

void GNSTransport::ApplyIceConfig() {
  ISteamNetworkingUtils* utils = SteamNetworkingUtils();
  if (!utils) {
    return;  // library not up yet; Initialize() will call us once it is
  }

  std::string stun, turn, turn_user, turn_pass;
  bool turn_from_api = false;
  {
    std::lock_guard<std::mutex> lock(ice_mutex_);
    stun = ice_stun_.empty() ? std::string(cvars::gns_stun_server_list)
                             : ice_stun_;
    // TURN is all-or-nothing per source: an /turn override supplies its own
    // creds; falling back means falling back for creds too.
    if (!ice_turn_.empty()) {
      turn = ice_turn_;
      turn_user = ice_turn_user_;
      turn_pass = ice_turn_pass_;
      turn_from_api = true;
    } else {
      turn = cvars::gns_turn_server_list;
      turn_user = cvars::gns_turn_username;
      turn_pass = cvars::gns_turn_password;
    }
  }

  // Strip URL forms GNS's ICE parser can't consume (turns:/stuns: TLS schemes,
  // ?transport=tcp entries, ?query suffixes). Applied to both sources so the
  // cvars can also carry standard RFC-7065 URLs.
  const size_t turn_before = CountCsvEntries(turn);
  stun = NormalizeIceServerList(stun);
  turn = NormalizeIceServerList(turn);
  const size_t turn_after = CountCsvEntries(turn);
  if (turn_after != turn_before) {
    XELOGI("[GNS] ICE: normalized TURN list {} -> {} UDP relay(s)", turn_before,
           turn_after);
  }

  utils->SetGlobalConfigValueString(
      k_ESteamNetworkingConfig_P2P_STUN_ServerList, stun.c_str());

  if (!turn.empty()) {
    const size_t n = CountCsvEntries(turn);
    utils->SetGlobalConfigValueString(
        k_ESteamNetworkingConfig_P2P_TURN_ServerList, turn.c_str());
    if (!turn_user.empty()) {
      const std::string users = RepeatCredentialToCount(turn_user, n);
      utils->SetGlobalConfigValueString(
          k_ESteamNetworkingConfig_P2P_TURN_UserList, users.c_str());
    }
    if (!turn_pass.empty()) {
      const std::string passes = RepeatCredentialToCount(turn_pass, n);
      utils->SetGlobalConfigValueString(
          k_ESteamNetworkingConfig_P2P_TURN_PassList, passes.c_str());
    }
    XELOGI("[GNS] ICE config applied: {} STUN, {} TURN relay(s) [{}]",
           CountCsvEntries(stun), n, turn_from_api ? "WebServices" : "cvar");
  } else {
    XELOGI("[GNS] ICE config applied: {} STUN, no TURN relay",
           CountCsvEntries(stun));
  }
}

void GNSTransport::SetIceServers(const std::string& stun,
                                 const std::string& turn,
                                 const std::string& turn_user,
                                 const std::string& turn_pass) {
  {
    std::lock_guard<std::mutex> lock(ice_mutex_);
    ice_stun_ = stun;
    ice_turn_ = turn;
    ice_turn_user_ = turn_user;
    ice_turn_pass_ = turn_pass;
  }
  // SetGlobalConfigValueString is global and read per-connection, so applying
  // mid-session is safe: new P2P connections use the fresh creds; existing ones
  // keep the snapshot they were created with. No-op until the library is up.
  if (initialized_.load()) {
    ApplyIceConfig();
  }
}

void GNSTransport::PumpThread() {
  xe::threading::set_name("GNS Pump");
  // ~5s at the 2ms pump interval (approximate; pump work adds to each tick).
  constexpr int kStatsIntervalTicks = 2500;
  int ticks = 0;
  uint64_t last_txp = 0, last_txb = 0, last_rxp = 0, last_rxb = 0;
  while (pump_running_.load()) {
    // Time the callback+service work. If it ever blocks for long, ICE
    // keepalives stall and P2P connections drop -- this pinpoints a blocking op
    // on the pump thread (as opposed to a NAT-traversal failure).
    const auto pump_t0 = std::chrono::steady_clock::now();
    SteamNetworkingSockets()->RunCallbacks();
    Service();
    const auto pump_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - pump_t0)
                             .count();
    if (pump_ms > 250) {
      XELOGW("[GNS] pump iteration took {} ms (blocked?)", pump_ms);
    }
    if (++ticks >= kStatsIntervalTicks) {
      const uint64_t txp = g_tx_packets.load(std::memory_order_relaxed);
      const uint64_t txb = g_tx_bytes.load(std::memory_order_relaxed);
      const uint64_t rxp = g_rx_packets.load(std::memory_order_relaxed);
      const uint64_t rxb = g_rx_bytes.load(std::memory_order_relaxed);
      if (txp != last_txp || rxp != last_rxp) {
        XELOGI("[GNS] throughput ~5s: tx {} pkts / {} KiB, rx {} pkts / {} KiB",
               txp - last_txp, (txb - last_txb) / 1024, rxp - last_rxp,
               (rxb - last_rxb) / 1024);
      }
      last_txp = txp;
      last_txb = txb;
      last_rxp = rxp;
      last_rxb = rxb;
      ticks = 0;
    }
    std::this_thread::sleep_for(2ms);
  }
}

// Render a guest online IP (network-order s_addr) as dotted-decimal for logs.
static std::string FormatIna(uint32_t ina) {
  return fmt::format("{}.{}.{}.{}", ina & 0xFF, (ina >> 8) & 0xFF,
                     (ina >> 16) & 0xFF, (ina >> 24) & 0xFF);
}

// Hex-encode a packet payload for the gns_log_packets diagnostic. Capped so a
// giant config block can't produce a multi-KB log line; the true length is
// logged separately and ".." marks truncation. Bump kPktLogMaxBytes if a full
// dump of large frames is needed.
static constexpr size_t kPktLogMaxBytes = 2048;
static std::string HexDump(const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  const size_t n = std::min(len, kPktLogMaxBytes);
  std::string s;
  s.reserve(n * 2 + 2);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(kHex[data[i] >> 4]);
    s.push_back(kHex[data[i] & 0x0F]);
  }
  if (len > kPktLogMaxBytes) {
    s += "..";
  }
  return s;
}

void GNSTransport::Service() {
  ISteamNetworkingMessages* messages = SteamNetworkingMessages();

  // Snapshot the handlers so we don't hold the lock across delivery.
  ReceiveHandler handler;
  VoiceHandler voice_handler;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = receive_handler_;
    voice_handler = voice_handler_;
  }

  SteamNetworkingMessage_t* in_messages[32];
  int received =
      messages->ReceiveMessagesOnChannel(kDatagramChannel, in_messages, 32);
  for (int i = 0; i < received; ++i) {
    SteamNetworkingMessage_t* msg = in_messages[i];

    // Route the frame to the guest if a handler is installed, the frame is big
    // enough to carry the src/dst port header, and the peer identity resolves.
    if ((handler || voice_handler) &&
        msg->m_cbSize >= static_cast<int>(kFrameHeaderSize)) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(msg->m_pData);
      uint16_t src_port = static_cast<uint16_t>((p[0] << 8) | p[1]);
      uint16_t dst_port = static_cast<uint16_t>((p[2] << 8) | p[3]);

      uint64_t key =
          PeerKeyFromIdentityString(msg->m_identityPeer.GetGenericString());
      uint32_t src_ina = ResolveOrRegisterInbound(key);
      if (src_ina) {
        const uint8_t* payload = p + kFrameHeaderSize;
        const size_t plen =
            static_cast<size_t>(msg->m_cbSize) - kFrameHeaderSize;
        if (dst_port == kVoicePort) {
          // Voice side-channel: divert before the guest handler so no game
          // traffic is touched (no guest socket binds kVoicePort).
          if (voice_handler) {
            voice_handler(src_ina, payload, plen);
          }
        } else if (handler) {
          if (cvars::gns_log_packets) {
            XELOGD("[GNS] pkt RX udp <- {}:{} dst:{} {}B {}",
                   FormatIna(src_ina), src_port, dst_port, plen,
                   HexDump(payload, plen));
          }
          handler(src_ina, src_port, dst_port, payload, plen);
        }
      }
    }

    g_rx_packets.fetch_add(1, std::memory_order_relaxed);
    g_rx_bytes.fetch_add(static_cast<uint64_t>(msg->m_cbSize),
                         std::memory_order_relaxed);
    msg->Release();
  }

  // Unified-stream mode: drain the reliable stream channel of the shared
  // Messages sessions and dispatch the OPEN/ACK/DATA/CLOSE mux. This replaces
  // the ConnectP2P-based drain below (that loop finds no connections when the
  // mode is on, since StreamConnect/Listen create no GNS connections).
  if (cvars::gns_stream) {
    SteamNetworkingMessage_t* sm[32];
    int sn = messages->ReceiveMessagesOnChannel(kStreamChannel, sm, 32);
    for (int i = 0; i < sn; ++i) {
      SteamNetworkingMessage_t* msg = sm[i];
      g_rx_packets.fetch_add(1, std::memory_order_relaxed);
      g_rx_bytes.fetch_add(static_cast<uint64_t>(msg->m_cbSize),
                           std::memory_order_relaxed);
      if (msg->m_cbSize < static_cast<int>(kStreamCtrlSize)) {
        msg->Release();
        continue;
      }
      const uint8_t* p = reinterpret_cast<const uint8_t*>(msg->m_pData);
      const uint8_t op = p[0];
      const uint16_t vport = static_cast<uint16_t>((p[1] << 8) | p[2]);
      const uint8_t* body = p + kStreamCtrlSize;
      const size_t blen = static_cast<size_t>(msg->m_cbSize) - kStreamCtrlSize;
      const uint64_t key =
          PeerKeyFromIdentityString(msg->m_identityPeer.GetGenericString());
      if (!key) {
        msg->Release();
        continue;
      }
      switch (op) {
        case kStreamOpen: {
          // Server side: match a listener on vport, create/queue the logical
          // stream, and ACK. Idempotent on a duplicate OPEN.
          std::shared_ptr<StreamListener> ln;
          std::shared_ptr<StreamConn> conn;
          bool created = false;
          {
            std::lock_guard<std::mutex> lock(g_streams_mutex);
            auto lit = g_listeners.find(vport);
            if (lit == g_listeners.end()) {
              break;  // nobody listening -> drop (peer will time out / retry)
            }
            ln = lit->second;
            conn = FindUnifiedStreamLocked(key, vport);
            if (!conn || conn->state.load() == kStreamClosed) {
              conn = std::make_shared<StreamConn>();
              conn->unified = true;
              conn->handle = AllocUnifiedHandle();
              conn->peer_key = key;
              conn->listen_vport = vport;
              conn->vport = vport;
              conn->state.store(kStreamConnected);
              g_streams[conn->handle] = conn;
              created = true;
            }
          }
          if (ln && created) {
            std::lock_guard<std::mutex> lk(ln->mutex);
            ln->pending.push_back(conn->handle);
            ln->cv.notify_all();
            XELOGI(
                "[GNS] unified inbound stream conn={:08X} vport {} xe:{:016x}",
                conn->handle, vport, key);
          }
          SendStreamFrame(key, kStreamOpenAck, vport, nullptr, 0);
          break;
        }
        case kStreamOpenAck: {
          // Client side: our outbound stream is accepted -> Connected.
          std::shared_ptr<StreamConn> conn;
          {
            std::lock_guard<std::mutex> lock(g_streams_mutex);
            conn = FindUnifiedStreamLocked(key, vport);
          }
          if (conn && conn->state.load() == kStreamConnecting) {
            conn->state.store(kStreamConnected);
            conn->cv.notify_all();
          }
          break;
        }
        case kStreamData: {
          std::shared_ptr<StreamConn> conn;
          {
            std::lock_guard<std::mutex> lock(g_streams_mutex);
            conn = FindUnifiedStreamLocked(key, vport);
          }
          if (conn) {
            {
              std::lock_guard<std::mutex> lk(conn->mutex);
              conn->rx.insert(conn->rx.end(), body, body + blen);
            }
            conn->cv.notify_all();
            if (cvars::gns_log_packets) {
              XELOGD("[GNS] pkt RX tcp <- {} conn={:08X} vport {} {}B {}",
                     FormatIna(InaFromPeerKey(key)), conn->handle, vport, blen,
                     HexDump(body, blen));
            }
          }
          break;
        }
        case kStreamCloseOp: {
          std::shared_ptr<StreamConn> conn;
          {
            std::lock_guard<std::mutex> lock(g_streams_mutex);
            conn = FindUnifiedStreamLocked(key, vport);
          }
          if (conn) {
            conn->state.store(kStreamClosed);
            conn->cv.notify_all();
          }
          break;
        }
        default:
          break;
      }
      msg->Release();
    }
  }

  // Drain reliable bytes on active stream (TCP) connections into their
  // per-connection reassembly buffers.
  std::vector<std::shared_ptr<StreamConn>> conns;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    conns.reserve(g_streams.size());
    for (auto& [h, c] : g_streams) {
      conns.push_back(c);
    }
  }
  ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
  for (auto& conn : conns) {
    if (conn->unified) {
      continue;  // unified streams are serviced via the channel drain above
    }
    if (conn->state.load() != kStreamConnected) {
      continue;
    }
    SteamNetworkingMessage_t* stream_msgs[16];
    int n = sockets->ReceiveMessagesOnConnection(conn->h, stream_msgs, 16);
    if (n <= 0) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(conn->mutex);
      for (int i = 0; i < n; ++i) {
        const uint8_t* p =
            reinterpret_cast<const uint8_t*>(stream_msgs[i]->m_pData);
        conn->rx.insert(conn->rx.end(), p, p + stream_msgs[i]->m_cbSize);
        if (cvars::gns_log_packets) {
          XELOGD("[GNS] pkt RX tcp <- {} conn={:08X} {}B {}",
                 FormatIna(InaFromPeerKey(conn->peer_key)), conn->h,
                 stream_msgs[i]->m_cbSize,
                 HexDump(p, stream_msgs[i]->m_cbSize));
        }
        g_rx_bytes.fetch_add(static_cast<uint64_t>(stream_msgs[i]->m_cbSize),
                             std::memory_order_relaxed);
        stream_msgs[i]->Release();
      }
    }
    conn->cv.notify_all();
    g_rx_packets.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
  }
}

bool GNSTransport::SendTo(uint32_t guest_ina, uint16_t src_port,
                          uint16_t dst_port, const uint8_t* data, size_t len,
                          bool reliable) {
  if (!initialized_.load()) {
    return false;
  }

  uint64_t key;
  {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    auto it = ina_to_key_.find(guest_ina);
    if (it == ina_to_key_.end()) {
      XELOGD("[GNS] SendTo: no peer mapped for ina {:08X}", guest_ina);
      return false;
    }
    key = it->second;
  }

  // Defensive: never open a GNS session to ourselves. CacheRemotePeerMac
  // already avoids mapping self, but any other stray self-mapping would
  // otherwise spam SendMessageToUser ConnectFailed for the console's own online
  // IP.
  if (local_peer_key_ && key == local_peer_key_) {
    return false;
  }

  // Prepend the framing header (src/dst guest port, big-endian).
  std::vector<uint8_t> framed(kFrameHeaderSize + len);
  framed[0] = static_cast<uint8_t>(src_port >> 8);
  framed[1] = static_cast<uint8_t>(src_port & 0xFF);
  framed[2] = static_cast<uint8_t>(dst_port >> 8);
  framed[3] = static_cast<uint8_t>(dst_port & 0xFF);
  if (len) {
    std::memcpy(framed.data() + kFrameHeaderSize, data, len);
  }

  SteamNetworkingIdentity id = MakeIdentity(key);
  int flags = reliable ? k_nSteamNetworkingSend_Reliable
                       : k_nSteamNetworkingSend_Unreliable;
  EResult result = SteamNetworkingMessages()->SendMessageToUser(
      id, framed.data(), static_cast<uint32_t>(framed.size()), flags,
      kDatagramChannel);
  if (result != k_EResultOK) {
    XELOGW("[GNS] SendMessageToUser(xe:{:016x}) failed: EResult {}", key,
           static_cast<int>(result));
  } else {
    g_tx_packets.fetch_add(1, std::memory_order_relaxed);
    g_tx_bytes.fetch_add(framed.size(), std::memory_order_relaxed);
    if (cvars::gns_log_packets) {
      XELOGD("[GNS] pkt TX udp -> {}:{} src:{} {}B{} {}", FormatIna(guest_ina),
             dst_port, src_port, len, reliable ? " reliable" : "",
             HexDump(data, len));
    }
  }
  return result == k_EResultOK;
}

void GNSTransport::ProbePeer(uint32_t guest_ina) {
  if (!initialized_.load() || !IsMapped(guest_ina)) {
    return;
  }
  // Empty frame on the reserved QoS port: opens/keeps-alive the Messages
  // session so GetPeerStatus can observe the link, and is dropped by the peer's
  // guest handler (no socket binds kQosProbePort).
  SendTo(guest_ina, kQosProbePort, kQosProbePort, nullptr, 0, false);
}

GNSTransport::PeerStatus GNSTransport::GetPeerStatus(uint32_t guest_ina) {
  PeerStatus st;
  const uint64_t key = PeerKeyFromIna(guest_ina);
  if (!key) {
    return st;  // not in the registry -> native/non-GNS peer (mapped=false)
  }
  st.mapped = true;
  if (!initialized_.load()) {
    return st;
  }
  // Prefer a live stream (TCP-over-GNS) connection: it carries the gameplay
  // link and exposes a queryable real-time ping.
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    for (const auto& kv : g_streams) {
      const auto& conn = kv.second;
      if (conn->unified) {
        continue;  // no GNS handle; the Messages-session status below applies
      }
      if (conn->peer_key == key && conn->state.load() == kStreamConnected) {
        SteamNetConnectionRealTimeStatus_t rt = {};
        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(
                conn->h, &rt, 0, nullptr) == k_EResultOK) {
          st.ping_ms = rt.m_nPing;
        }
        st.connected = true;
        return st;
      }
    }
  }
  // Otherwise the datagram (ISteamNetworkingMessages) session, if established.
  SteamNetConnectionRealTimeStatus_t rt = {};
  const ESteamNetworkingConnectionState s =
      SteamNetworkingMessages()->GetSessionConnectionInfo(MakeIdentity(key),
                                                          nullptr, &rt);
  if (s == k_ESteamNetworkingConnectionState_Connected) {
    st.connected = true;
    st.ping_ms = rt.m_nPing;
  }
  return st;
}

void GNSTransport::DeliverInboundSignal(const uint8_t* data, size_t len) {
  if (!initialized_.load()) {
    return;
  }
  SteamNetworkingSockets()->ReceivedP2PCustomSignal(data, static_cast<int>(len),
                                                    &g_recv_context);
}

void GNSTransport::AbortPeer(uint64_t peer_key) {
  if (!initialized_.load() || !peer_key || peer_key == local_peer_key_) {
    return;
  }
  // Tear down the datagram (ISteamNetworkingMessages) session to this peer so
  // ICE stops spinning on the undeliverable signal.
  SteamNetworkingMessages()->CloseSessionWithUser(MakeIdentity(peer_key));

  // Close + drop any stream (TCP-over-GNS) connections to this peer. Collect
  // under the lock, close outside it -- CloseConnection's status callback also
  // takes g_streams_mutex (on the pump thread), so we must not hold it here.
  std::vector<std::shared_ptr<StreamConn>> conns;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    for (auto it = g_streams.begin(); it != g_streams.end();) {
      if (it->second->peer_key == peer_key) {
        conns.push_back(it->second);
        it = g_streams.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& c : conns) {
    c->state.store(kStreamClosed);
    c->cv.notify_all();  // wake blocked StreamRecv/StreamConnect -> EOF / fail
    if (!c->unified && c->h != k_HSteamNetConnection_Invalid) {
      SteamNetworkingSockets()->CloseConnection(
          c->h, 0, "relay: dest unreachable", false);
    }
  }
  XELOGI(
      "[GNS] abort peer xe:{:016x}: relay reports it unreachable ({} stream(s) "
      "dropped)",
      peer_key, conns.size());
}

// --- Connection-oriented (TCP) API (Phase 6) --------------------------------

uint32_t GNSTransport::StreamConnect(uint32_t guest_ina, uint16_t dst_vport) {
  if (!initialized_.load()) {
    return 0;
  }
  uint64_t key;
  {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    auto it = ina_to_key_.find(guest_ina);
    if (it == ina_to_key_.end()) {
      return 0;
    }
    key = it->second;
  }

  if (cvars::gns_stream) {
    // Reuse the peer's datagram session: create a logical stream and send OPEN
    // on the reliable stream channel. Connected is reached when OPEN_ACK
    // arrives (Service). Idempotent: an existing live stream to (peer,vport) is
    // reused.
    std::shared_ptr<StreamConn> conn;
    {
      std::lock_guard<std::mutex> lock(g_streams_mutex);
      conn = FindUnifiedStreamLocked(key, dst_vport);
      if (conn && conn->state.load() != kStreamClosed) {
        return conn->handle;
      }
      conn = std::make_shared<StreamConn>();
      conn->unified = true;
      conn->handle = AllocUnifiedHandle();
      conn->peer_key = key;
      conn->vport = dst_vport;  // listen_vport stays 0 (outbound)
      conn->state.store(kStreamConnecting);
      g_streams[conn->handle] = conn;
    }
    if (!SendStreamFrame(key, kStreamOpen, dst_vport, nullptr, 0)) {
      std::lock_guard<std::mutex> lock(g_streams_mutex);
      g_streams.erase(conn->handle);
      return 0;
    }
    XELOGI("[GNS] StreamConnect(unified) xe:{:016x} vport {} -> conn {:08X}",
           key, dst_vport, conn->handle);
    return conn->handle;
  }

  SteamNetworkingIdentity id = MakeIdentity(key);
  HSteamNetConnection h = SteamNetworkingSockets()->ConnectP2P(
      id, static_cast<int>(dst_vport), 0, nullptr);
  if (h == k_HSteamNetConnection_Invalid) {
    return 0;
  }

  auto conn = std::make_shared<StreamConn>();
  conn->h = h;
  conn->handle = h;
  conn->peer_key = key;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    g_streams[h] = conn;
  }
  XELOGI("[GNS] StreamConnect xe:{:016x} vport {} -> conn {}", key, dst_vport,
         h);
  return h;
}

bool GNSTransport::StreamListen(uint16_t vport) {
  if (!initialized_.load()) {
    return false;
  }
  if (cvars::gns_stream) {
    // No GNS listen socket: inbound OPEN frames on the stream channel are
    // matched to this listener (by vport) in Service. Just register the accept
    // queue.
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    if (!g_listeners.count(vport)) {
      auto ln = std::make_shared<StreamListener>();
      g_listeners[vport] = ln;  // ln->h stays Invalid
      XELOGI("[GNS] StreamListen(unified) vport {}", vport);
    }
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    if (g_listeners.count(vport)) {
      return true;  // already listening on this port
    }
  }
  // Create the listen socket WITHOUT holding g_streams_mutex.
  // CreateListenSocketP2P takes the GNS global lock, which the pump thread
  // holds while invoking our ConnectionStatusChanged callback (which takes
  // g_streams_mutex). Holding both in the opposite order here would be a
  // lock-order inversion that stalls the pump (and hence ICE keepalives).
  HSteamListenSocket ls = SteamNetworkingSockets()->CreateListenSocketP2P(
      static_cast<int>(vport), 0, nullptr);
  if (ls == k_HSteamListenSocket_Invalid) {
    return false;
  }
  bool lost = false;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    if (g_listeners.count(vport)) {
      lost = true;  // another thread created one meanwhile
    } else {
      auto ln = std::make_shared<StreamListener>();
      ln->h = ls;
      g_listeners[vport] = ln;
      g_listen_vport[ls] = vport;
    }
  }
  if (lost) {
    SteamNetworkingSockets()->CloseListenSocket(ls);  // outside the lock
  } else {
    XELOGI("[GNS] StreamListen vport {} -> listen {}", vport, ls);
  }
  return true;
}

void GNSTransport::StreamStopListen(uint16_t vport) {
  std::shared_ptr<StreamListener> ln;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    auto it = g_listeners.find(vport);
    if (it == g_listeners.end()) {
      return;
    }
    ln = it->second;
    g_listen_vport.erase(ln->h);
    g_listeners.erase(it);
  }
  {
    std::lock_guard<std::mutex> lk(ln->mutex);
    ln->stopping = true;
  }
  ln->cv.notify_all();
  if (ln->h != k_HSteamListenSocket_Invalid) {
    SteamNetworkingSockets()->CloseListenSocket(ln->h);  // legacy mode only
  }
}

uint32_t GNSTransport::StreamAccept(uint16_t vport, bool wait,
                                    uint32_t* out_peer_ina) {
  std::shared_ptr<StreamListener> ln;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    auto it = g_listeners.find(vport);
    if (it == g_listeners.end()) {
      return 0;
    }
    ln = it->second;
  }

  HSteamNetConnection h = k_HSteamNetConnection_Invalid;
  {
    std::unique_lock<std::mutex> lk(ln->mutex);
    while (ln->pending.empty()) {
      if (ln->stopping || !initialized_.load()) {
        return 0;
      }
      if (!wait) {
        return 0;
      }
      ln->cv.wait_for(lk, std::chrono::milliseconds(500));
    }
    h = ln->pending.front();
    ln->pending.pop_front();
  }

  if (out_peer_ina) {
    auto conn = FindStream(h);
    *out_peer_ina = conn ? ResolveOrRegisterInbound(conn->peer_key) : 0;
  }
  return h;
}

int GNSTransport::StreamSend(uint32_t conn, const uint8_t* data, size_t len) {
  auto c = FindStream(conn);
  if (!c || c->state.load() == kStreamClosed) {
    return -1;
  }
  if (c->unified) {
    if (!SendStreamFrame(c->peer_key, kStreamData, c->vport, data, len)) {
      return -1;
    }
  } else {
    EResult r = SteamNetworkingSockets()->SendMessageToConnection(
        conn, data, static_cast<uint32_t>(len), k_nSteamNetworkingSend_Reliable,
        nullptr);
    if (r != k_EResultOK) {
      return -1;
    }
  }
  g_tx_packets.fetch_add(1, std::memory_order_relaxed);
  g_tx_bytes.fetch_add(len, std::memory_order_relaxed);
  if (cvars::gns_log_packets) {
    XELOGD("[GNS] pkt TX tcp -> {} conn={:08X} {}B {}",
           FormatIna(InaFromPeerKey(c->peer_key)), conn, len,
           HexDump(data, len));
  }
  return static_cast<int>(len);
}

int GNSTransport::StreamRecv(uint32_t conn, uint8_t* buf, size_t len,
                             bool wait) {
  auto c = FindStream(conn);
  if (!c) {
    return -1;
  }
  std::unique_lock<std::mutex> lk(c->mutex);
  for (;;) {
    size_t avail = c->rx.size() - c->rx_read;
    if (avail > 0) {
      size_t n = std::min(avail, len);
      std::memcpy(buf, c->rx.data() + c->rx_read, n);
      c->rx_read += n;
      if (c->rx_read == c->rx.size()) {  // fully drained -> reclaim
        c->rx.clear();
        c->rx_read = 0;
      }
      return static_cast<int>(n);
    }
    if (c->state.load() == kStreamClosed) {
      return 0;  // closed and drained -> EOF
    }
    if (!wait || !initialized_.load()) {
      return -1;  // would block
    }
    c->cv.wait_for(lk, std::chrono::milliseconds(500));
  }
}

GNSTransport::StreamState GNSTransport::StreamGetState(uint32_t conn) {
  auto c = FindStream(conn);
  if (!c) {
    return StreamState::kClosed;
  }
  switch (c->state.load()) {
    case kStreamConnected:
      return StreamState::kConnected;
    case kStreamClosed:
      return StreamState::kClosed;
    default:
      return StreamState::kConnecting;
  }
}

bool GNSTransport::StreamReadable(uint32_t conn) {
  auto c = FindStream(conn);
  if (!c) {
    return true;  // unknown -> recv returns immediately (don't block select)
  }
  if (c->state.load() == kStreamClosed) {
    return true;  // EOF is readable
  }
  std::lock_guard<std::mutex> lk(c->mutex);
  return c->rx.size() > c->rx_read;
}

void GNSTransport::StreamClose(uint32_t conn, bool linger) {
  std::shared_ptr<StreamConn> c;
  {
    std::lock_guard<std::mutex> lock(g_streams_mutex);
    auto it = g_streams.find(conn);
    if (it == g_streams.end()) {
      return;
    }
    c = it->second;
    g_streams.erase(it);
  }
  c->state.store(kStreamClosed);
  c->cv.notify_all();
  if (c->unified) {
    // Best-effort FIN on the stream channel; the session itself (datagrams) may
    // stay up for other traffic, so we don't tear anything else down.
    SendStreamFrame(c->peer_key, kStreamCloseOp, c->vport, nullptr, 0);
  } else {
    SteamNetworkingSockets()->CloseConnection(conn, 0, nullptr, linger);
  }
}

#else  // !XE_GNS_ENABLED

// Inert stubs for platforms where GNS is not built (non-Windows / non-x64).
// The transport can still be referenced; it just never does anything.

bool GNSTransport::Initialize(uint64_t) { return false; }
void GNSTransport::Shutdown() {}
void GNSTransport::PumpThread() {}
void GNSTransport::Service() {}
bool GNSTransport::SendTo(uint32_t, uint16_t, uint16_t, const uint8_t*, size_t,
                          bool) {
  return false;
}
void GNSTransport::DeliverInboundSignal(const uint8_t*, size_t) {}
void GNSTransport::AbortPeer(uint64_t) {}
void GNSTransport::ProbePeer(uint32_t) {}
GNSTransport::PeerStatus GNSTransport::GetPeerStatus(uint32_t) { return {}; }
uint32_t GNSTransport::StreamConnect(uint32_t, uint16_t) { return 0; }
bool GNSTransport::StreamListen(uint16_t) { return false; }
void GNSTransport::StreamStopListen(uint16_t) {}
uint32_t GNSTransport::StreamAccept(uint16_t, bool, uint32_t*) { return 0; }
int GNSTransport::StreamSend(uint32_t, const uint8_t*, size_t) { return -1; }
int GNSTransport::StreamRecv(uint32_t, uint8_t*, size_t, bool) { return -1; }
GNSTransport::StreamState GNSTransport::StreamGetState(uint32_t) {
  return StreamState::kClosed;
}
bool GNSTransport::StreamReadable(uint32_t) { return true; }
void GNSTransport::StreamClose(uint32_t, bool) {}
void GNSTransport::SetIceServers(const std::string&, const std::string&,
                                 const std::string&, const std::string&) {}
void GNSTransport::ApplyIceConfig() {}

#endif  // XE_GNS_ENABLED

}  // namespace kernel
}  // namespace xe
