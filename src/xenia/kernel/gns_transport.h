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

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

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
  // local_peer_key (e.g. the console machine id). If restrict_local_ipv4
  // (host byte order) and/or restrict_local_ipv6 (16-byte network order) are
  // given, ICE host-candidate gathering is restricted to that local adapter's
  // addresses so VPN/virtual NICs (incl. their IPv6, e.g. Tailscale) aren't
  // bound. Idempotent; returns false on failure, leaving the transport disabled.
  bool Initialize(
      uint64_t local_peer_key, uint32_t restrict_local_ipv4 = 0,
      const std::vector<std::array<uint8_t, 16>>& restrict_local_ipv6 = {});
  void Shutdown();
  bool initialized() const { return initialized_.load(); }

  // Override the ICE STUN/TURN servers at runtime (e.g. short-lived creds minted
  // by WebServices GET /turn). Empty strings mean "unset -> fall back to the
  // gns_stun_/gns_turn_ cvars". turn_user/turn_pass are SINGLE credentials that
  // apply to every TURN url (repeated to match the url count, as GNS requires
  // parallel equal-length lists). Applied to GNS's global config immediately if
  // initialized (new connections pick it up; live ones keep their snapshot), or
  // deferred to Initialize otherwise. Safe to call before or after Initialize.
  void SetIceServers(const std::string& stun, const std::string& turn,
                     const std::string& turn_user, const std::string& turn_pass);

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

  // --- Voice side-channel (netplay voice, Option 2) -------------------------
  // A reserved destination port. Datagrams addressed to it are diverted to the
  // voice handler in the receive pump BEFORE the guest ReceiveHandler, so no
  // guest game traffic is affected (no guest socket binds this port). Sent via
  // SendTo(peer, kVoicePort, kVoicePort, ..., reliable=false).
  static constexpr uint16_t kVoicePort = 0xF00D;
  // Reserved port for honest-QoS link probes (ProbePeer). No guest socket binds
  // it, so the empty probe frame is dropped by the peer's guest ReceiveHandler;
  // its only purpose is to open the ISteamNetworkingMessages session.
  static constexpr uint16_t kQosProbePort = 0xF00C;
  using VoiceHandler =
      std::function<void(uint32_t src_ina, const uint8_t* data, size_t len)>;
  void SetVoiceHandler(VoiceHandler handler);
  // Snapshot of the currently-mapped peer guest_inas, for voice broadcast.
  std::vector<uint32_t> GetMappedPeers() const;

  // Like GetMappedPeers, but only actual CONSOLE peers: excludes the dedicated/
  // relay server (its key is tagged in bits 48-63 via ServerPeerKeyFromIna) and
  // our own console (local_peer_key_). A console peer_key is a bare 48-bit MAC,
  // so (key >> 48) == 0 selects them. This is the recipient set for peer-to-peer
  // voice -- the title's session peers are mapped here on join, whereas the
  // server is not a voice participant (voice sent to it is silently dropped).
  std::vector<uint32_t> GetConsolePeers() const;

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

  // Fast-fail a peer whose signaling can't be delivered: the relay tells us (via
  // a dest-unreachable notice) that peer_key isn't connected to it, so any P2P/
  // ICE attempt to it can't complete. Tear down the datagram session + any stream
  // connections to it now (waking blocked stream readers with EOF) instead of
  // letting ICE spin for its full ~60s timeout. Mapping is kept -- a later send
  // simply re-attempts. Safe to call from the signaling worker thread.
  void AbortPeer(uint64_t peer_key);

  // Identity <-> peer_key helpers used by the signaling glue (defined in the
  // .cc, which owns the GNS types). PeerKeyFromIdentityString returns 0 if the
  // string is not one of our "xe:" identities.
  static uint64_t PeerKeyFromIdentityString(const char* generic_string);
  uint32_t InaFromPeerKey(uint64_t peer_key) const;  // 0 if unknown
  uint64_t PeerKeyFromIna(uint32_t guest_ina) const;  // 0 if unknown

  // Deterministic synthetic online IP (network-order s_addr, in 10.0.0.0/8) for a
  // console peer_key (= its 48-bit MAC). Under GNS the title must advertise and
  // route to these instead of its real public IP: the public IP isn't reachable
  // peer-to-peer (NAT) and isn't in the registry, so the peer-mesh handshake
  // can't complete. Because the value is a pure function of the MAC, both ends
  // agree on it without any exchange -- the advertiser derives it from its own
  // MAC, and a receiver re-derives the identical value from the MAC it already
  // has (session member list) or from the inbound GNS identity. All peers land in
  // one private /8 subnet, mirroring the same-subnet reachability Tailscale
  // provides for native netplay. Returns 0 for a null key.
  static uint32_t SyntheticInaFromPeerKey(uint64_t peer_key);

  // Deterministic peer_key for a dedicated title server, derived from its
  // synthetic online IP (XNADDR/TSADDR inaOnline, raw s_addr value). Both the
  // client and the server-side GNS gateway derive the same key from the same
  // address, so no out-of-band identity exchange is needed. Tagged in bits
  // 48-63 to never collide with the 48-bit console MAC keys used for peers.
  static uint64_t ServerPeerKeyFromIna(uint32_t ina);

  // --- Live peer link status (honest QoS) -----------------------------------
  // Real GNS connectivity to a mapped peer, used to answer XNetQosLookup with
  // the title's own P2P link state instead of fabricated "always reachable"
  // metrics. `mapped` is false for a guest_ina not in the registry (native /
  // non-GNS peer -- the QoS path keeps its legacy fabricated result for those).
  // `connected` is true only when a GNS session or stream to the peer is
  // established; `ping_ms` is the measured RTT (>=0) when connected, else -1.
  struct PeerStatus {
    bool mapped = false;
    bool connected = false;
    int ping_ms = -1;
  };
  PeerStatus GetPeerStatus(uint32_t guest_ina);

  // Nudge a GNS session toward a mapped peer to open (idempotent): sends a tiny
  // probe datagram so the ISteamNetworkingMessages session handshakes, letting a
  // subsequent GetPeerStatus observe the link come up. Mirrors what a real QoS
  // probe does (send probes, then measure). No-op if the peer isn't mapped.
  void ProbePeer(uint32_t guest_ina);

  // --- Connection-oriented (TCP) support (Phase 6) --------------------------
  // TCP/stream sockets use GNS's connection-oriented API (reliable, ordered) --
  // a natural fit for TCP -- reusing the same registry + signaling + ICE as the
  // datagram path. Connections are identified by an opaque handle (0 = invalid).
  // P2P only: the caller (XSocket) picks GNS vs native by whether the peer is
  // mapped, exactly as for datagrams.
  enum class StreamState { kConnecting, kConnected, kClosed };

  // Client: initiate a connection to a mapped peer on virtual port dst_vport.
  // Returns a connection handle, or 0 if the peer isn't mapped / not up.
  uint32_t StreamConnect(uint32_t guest_ina, uint16_t dst_vport);

  // Server: start / stop listening for inbound connections on virtual port
  // vport (the guest's bound TCP port).
  bool StreamListen(uint16_t vport);
  void StreamStopListen(uint16_t vport);

  // Server: pop the next established inbound connection on vport, returning its
  // handle and the peer's guest_ina. Returns 0 if none pending (wait=false) or
  // if aborted. Blocks on the listener when wait=true.
  uint32_t StreamAccept(uint16_t vport, bool wait, uint32_t* out_peer_ina);

  // Send reliable, ordered bytes on a connection. Returns bytes queued or -1.
  int StreamSend(uint32_t conn, const uint8_t* data, size_t len);

  // Read up to len bytes from the connection's byte-stream reassembly buffer.
  // Returns >0 bytes read; 0 if the peer closed and the buffer is drained; -1 if
  // no data is buffered and the connection is still open (wait=false) / aborted.
  int StreamRecv(uint32_t conn, uint8_t* buf, size_t len, bool wait);

  StreamState StreamGetState(uint32_t conn);

  // True if a StreamRecv would not block: buffered bytes are available, or the
  // connection is closed (recv returns EOF). Used by select() for readability.
  bool StreamReadable(uint32_t conn);

  // Close a connection. linger=true flushes queued reliable data first.
  void StreamClose(uint32_t conn, bool linger);

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
  VoiceHandler voice_handler_;

  std::atomic<GNSSignalingBackend*> signaling_backend_{nullptr};

  // Runtime ICE STUN/TURN override (from WebServices /turn). Empty => use the
  // gns_stun_/gns_turn_ cvars. Guarded by ice_mutex_; applied via ApplyIceConfig.
  mutable std::mutex ice_mutex_;
  std::string ice_stun_;
  std::string ice_turn_;
  std::string ice_turn_user_;  // single credential, repeated to url count
  std::string ice_turn_pass_;
  // Push the effective ICE config (override if set, else cvars) into GNS's global
  // config. Requires SteamNetworkingUtils() (post GameNetworkingSockets_Init).
  void ApplyIceConfig();
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GNS_TRANSPORT_H_
