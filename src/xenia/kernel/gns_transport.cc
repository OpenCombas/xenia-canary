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
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <vector>

#include "xenia/base/threading.h"

#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>
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
  uint32_t ina = next_synthetic_ina_++;
  ina_to_key_[ina] = peer_key;
  key_to_ina_[peer_key] = ina;
  return ina;
}

void GNSTransport::SetReceiveHandler(ReceiveHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  receive_handler_ = std::move(handler);
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
  // Bug = 1, Error = 2 are genuine problems; everything else is informational.
  if (static_cast<int>(type) <= 2) {
    XELOGE("[GNS] {}", msg);
  } else {
    XELOGI("[GNS] {}", msg);
  }
}

}  // namespace

bool GNSTransport::Initialize(uint64_t local_peer_key) {
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
  utils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg,
                                &DebugOutputFn);
  // SDR-less custom signaling: GNS asks this callback for a signaling object
  // for every locally-initiated connection (incl. ISteamNetworkingMessages).
  utils->SetGlobalConfigValuePtr(
      k_ESteamNetworkingConfig_Callback_CreateConnectionSignaling,
      reinterpret_cast<void*>(&CreateConnectionSignalingFn));
  utils->SetGlobalCallback_MessagesSessionRequest(&MessagesSessionRequestFn);

  // NAT traversal via the built-in ICE client, configured from cvars.
  utils->SetGlobalConfigValueString(
      k_ESteamNetworkingConfig_P2P_STUN_ServerList,
      cvars::gns_stun_server_list.c_str());

  if (!cvars::gns_turn_server_list.empty()) {
    utils->SetGlobalConfigValueString(
        k_ESteamNetworkingConfig_P2P_TURN_ServerList,
        cvars::gns_turn_server_list.c_str());
    if (!cvars::gns_turn_username.empty()) {
      utils->SetGlobalConfigValueString(
          k_ESteamNetworkingConfig_P2P_TURN_UserList,
          cvars::gns_turn_username.c_str());
    }
    if (!cvars::gns_turn_password.empty()) {
      utils->SetGlobalConfigValueString(
          k_ESteamNetworkingConfig_P2P_TURN_PassList,
          cvars::gns_turn_password.c_str());
    }
  }

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
  GameNetworkingSockets_Kill();
  initialized_.store(false);
  XELOGI("[GNS] transport shut down");
}

void GNSTransport::PumpThread() {
  xe::threading::set_name("GNS Pump");
  // ~5s at the 2ms pump interval (approximate; pump work adds to each tick).
  constexpr int kStatsIntervalTicks = 2500;
  int ticks = 0;
  uint64_t last_txp = 0, last_txb = 0, last_rxp = 0, last_rxb = 0;
  while (pump_running_.load()) {
    SteamNetworkingSockets()->RunCallbacks();
    Service();
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

void GNSTransport::Service() {
  ISteamNetworkingMessages* messages = SteamNetworkingMessages();

  // Snapshot the handler so we don't hold the lock across delivery.
  ReceiveHandler handler;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = receive_handler_;
  }

  SteamNetworkingMessage_t* in_messages[32];
  int received =
      messages->ReceiveMessagesOnChannel(kDatagramChannel, in_messages, 32);
  for (int i = 0; i < received; ++i) {
    SteamNetworkingMessage_t* msg = in_messages[i];

    if (handler && msg->m_cbSize >= static_cast<int>(kFrameHeaderSize)) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(msg->m_pData);
      uint16_t src_port = static_cast<uint16_t>((p[0] << 8) | p[1]);
      uint16_t dst_port = static_cast<uint16_t>((p[2] << 8) | p[3]);

      uint64_t key =
          PeerKeyFromIdentityString(msg->m_identityPeer.GetGenericString());
      uint32_t src_ina = ResolveOrRegisterInbound(key);
      if (src_ina) {
        handler(src_ina, src_port, dst_port, p + kFrameHeaderSize,
                static_cast<size_t>(msg->m_cbSize) - kFrameHeaderSize);
      }
    }

    g_rx_packets.fetch_add(1, std::memory_order_relaxed);
    g_rx_bytes.fetch_add(static_cast<uint64_t>(msg->m_cbSize),
                         std::memory_order_relaxed);
    msg->Release();
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
    XELOGD("[GNS] sent {} bytes to xe:{:016x} ({}:{}->:{})", framed.size(), key,
           guest_ina, src_port, dst_port);
  }
  return result == k_EResultOK;
}

void GNSTransport::DeliverInboundSignal(const uint8_t* data, size_t len) {
  if (!initialized_.load()) {
    return;
  }
  SteamNetworkingSockets()->ReceivedP2PCustomSignal(data, static_cast<int>(len),
                                                    &g_recv_context);
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

#endif  // XE_GNS_ENABLED

}  // namespace kernel
}  // namespace xe
