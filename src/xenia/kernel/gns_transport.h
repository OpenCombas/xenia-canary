/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GNS_TRANSPORT_H_
#define XENIA_KERNEL_GNS_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace xe {
namespace kernel {

// Pluggable transport for GNS custom-signaling blobs. GNS establishes P2P
// connections by exchanging small best-effort datagrams ("signals") out of
// band; this interface carries them. Phase 4 supplies an implementation backed
// by the matchmaking (heroku) server. Until then a null backend is used and no
// connections can be established (SendTo simply reports failure -> native
// fallback).
class GNSSignalingBackend {
 public:
  virtual ~GNSSignalingBackend() = default;
  // Deliver an opaque signaling blob to the peer identified by peer_key.
  // Best-effort: drops/dupes/reordering are tolerated by GNS.
  virtual void SendSignal(uint64_t peer_key, const uint8_t* data,
                          size_t len) = 0;
};

// Routes Xbox 360 guest UDP/VDP datagrams over Valve's GameNetworkingSockets
// using ISteamNetworkingMessages with SDR-less custom signaling + ICE NAT
// traversal. See docs/gns_integration.md.
//
// Singleton. Phase 2 builds the transport in isolation; it is not yet wired
// into XSocket (Phase 3) nor populated/initialized by netplay startup
// (Phases 4-5), so on its own it changes no runtime behavior.
class GNSTransport {
 public:
  // A received datagram, demuxed back into guest terms: source peer (synthetic
  // guest IP + port), destination guest port (selects the bound socket), and
  // the opaque payload (VDP framing, if any, is left untouched).
  using ReceiveHandler =
      std::function<void(uint32_t src_ina, uint16_t src_port, uint16_t dst_port,
                         const uint8_t* data, size_t len)>;

  static GNSTransport* Get();

  // Master enable cvar (`gns`). Other phases consult this before routing.
  static bool IsEnabled();

  // Initialize the GNS library with our local identity, derived from
  // local_peer_key (e.g. the console machine id). Idempotent; returns false on
  // failure, leaving the transport disabled.
  bool Initialize(uint64_t local_peer_key);
  void Shutdown();
  bool initialized() const { return initialized_.load(); }

  // Registry (Phase 4 populates from session join/leave). guest_ina is the
  // synthetic per-peer IP the title sees (XNADDR::inaOnline); peer_key is the
  // peer's stable id, from which its SteamNetworkingIdentity is derived.
  void MapPeer(uint32_t guest_ina, uint64_t peer_key);
  void UnmapPeer(uint32_t guest_ina);
  bool IsMapped(uint32_t guest_ina) const;

  // Send a datagram to a mapped peer. Returns false if the peer is not mapped
  // or the transport is not initialized (caller falls back to native send).
  // reliable selects the GNS reliability flag (UDP/VDP => false).
  bool SendTo(uint32_t guest_ina, uint16_t src_port, uint16_t dst_port,
              const uint8_t* data, size_t len, bool reliable);

  // Phase 3 installs this to route received datagrams into the bound XSocket.
  void SetReceiveHandler(ReceiveHandler handler);

  // Server/gateway mode (Phase 7): when enabled, an inbound datagram from a
  // peer that isn't in the registry auto-registers that peer against a
  // synthetic guest_ina, so the receive handler still fires and replies can be
  // routed back via SendTo. The client leaves this off (peers are pre-mapped at
  // session join), so its behavior is unchanged.
  void set_auto_register_inbound(bool enabled);

  // Signaling (Phase 4). The backend ships our outbound blobs; inbound blobs
  // received by the backend are pushed back in via DeliverInboundSignal.
  void SetSignalingBackend(GNSSignalingBackend* backend);
  GNSSignalingBackend* signaling_backend() const { return signaling_backend_; }
  void DeliverInboundSignal(const uint8_t* data, size_t len);

  // Identity <-> peer_key helpers used by the signaling glue (defined in the
  // .cc, which owns the GNS types). PeerKeyFromIdentityString returns 0 if the
  // string is not one of our "xe:" identities.
  static uint64_t PeerKeyFromIdentityString(const char* generic_string);
  uint32_t InaFromPeerKey(uint64_t peer_key) const;  // 0 if unknown

  // Deterministic peer_key for a dedicated title server, derived from its
  // synthetic online IP (XNADDR/TSADDR inaOnline, raw s_addr value). Both the
  // client and the server-side GNS gateway derive the same key from the same
  // address, so no out-of-band identity exchange is needed. Tagged in bits
  // 48-63 to never collide with the 48-bit console MAC keys used for peers.
  static uint64_t ServerPeerKeyFromIna(uint32_t ina);

 private:
  GNSTransport() = default;

  void PumpThread();
  void Service();
  // Look up the guest_ina for an inbound peer_key; in auto-register mode,
  // synthesizes and records one for an unknown peer. Returns 0 if unknown and
  // auto-register is off.
  uint32_t ResolveOrRegisterInbound(uint64_t peer_key);

  std::atomic<bool> initialized_{false};
  std::atomic<bool> pump_running_{false};
  std::thread pump_thread_;

  uint64_t local_peer_key_ = 0;

  // Bidirectional registry: guest_ina <-> peer_key.
  mutable std::shared_mutex registry_mutex_;
  std::map<uint32_t, uint64_t> ina_to_key_;
  std::map<uint64_t, uint32_t> key_to_ina_;

  // Server/gateway mode: synthesize guest_ina values for unknown inbound peers.
  std::atomic<bool> auto_register_inbound_{false};
  uint32_t next_synthetic_ina_ = 0x0A000001;  // 10.0.0.1+, registry-internal

  mutable std::mutex handler_mutex_;
  ReceiveHandler receive_handler_;

  std::atomic<GNSSignalingBackend*> signaling_backend_{nullptr};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GNS_TRANSPORT_H_
