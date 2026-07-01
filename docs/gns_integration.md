# GameNetworkingSockets (GNS) Integration Plan

Refactor the netplay socket layer to route Xbox 360 guest UDP/VDP traffic over
Valve's [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
(GNS) using `ISteamNetworkingMessages`, gaining NAT traversal (ICE/STUN/TURN)
without depending on Steam or the Steam Datagram Relay (SDR).

## Decisions (locked)

| Topic | Decision |
| --- | --- |
| Transport model | **GNS default ON, native socket fallback** when no identity mapping exists (LAN / system-link / unmapped peers). |
| Identity / auth | **SDR-less.** Standalone open-source GNS build with **custom signaling**; **ICE** (`-DENABLE_ICE=ON`) for NAT traversal via STUN/TURN. No Steam app ID, no Steam account. |
| Signaling carrier | **Dedicated standalone WebSocket relay** (a separate Go service, hosted in its **own repository** — not vendored in this tree), reached via a new `gns_signaling_url` cvar. *Not* the matchmaking (heroku) API: that stays REST request/response for session discovery, which can't push unsolicited inbound ICE signals. The relay is a dumb, opaque blob forwarder keyed by `peer_key`. |
| First deliverable | This document. |

## Why this is mostly an *addressing* problem

The title speaks **IP:port** (`XSOCKADDR_IN`). `ISteamNetworkingMessages` is
connectionless like `sendto()` but is keyed by **`SteamNetworkingIdentity`**, not
IP:port ("It is like UDP... you specify the message recipient each time you
send"). Everything else lines up:

- VDP/UDP `SendTo`/`RecvFrom` ↔ `SendMessageToUser`/`ReceiveMessagesOnChannel`.
- The fork already treats `XNADDR::inaOnline` as a **synthetic per-peer token**
  resolved through `XLiveAPI` — a natural primary key for a peer.
- `xsocket.h` already carries a dormant `QueuePacket()` / `incoming_packets_`
  queue + `packet` struct — scaffolding that becomes the landing zone for
  received GNS messages.

So the core deliverable is a **bidirectional registry**:

```
guest XSOCKADDR_IN (inaOnline : port)  <-->  SteamNetworkingIdentity
```

plus a transport singleton that pumps GNS and a branch inside `XSocket` that
chooses GNS or the native handle per send/recv.

## Current state (verified)

- **Build**: CMake (ported from premake). Third-party libs are hand-written
  `add_library(... STATIC ...)` targets in `third_party/CMakeLists.txt`
  (`miniupnp` @ ~L848, `libcurl` @ ~L834 are the networking analogues).
  MSVC links `ws2_32 iphlpapi ...` globally (`CMakeLists.txt:144`).
- **Kernel link**: `src/xenia/kernel/CMakeLists.txt` builds `xenia-kernel`
  (contains `xsocket.cc`) and links `miniupnp libcurl ...`. GNS gets added here.
- **Socket layer**: `XSocket` (`src/xenia/kernel/xsocket.{h,cc}`) is a thin
  wrapper over a native handle (`native_handle_`). Guest calls map ~1:1 to
  syscalls. Endianness handled at the `XSOCKADDR_IN <-> sockaddr` boundary
  (`to_host()`/`to_guest()`). Async path: `WSARecvFrom` → `PollWSARecvFrom`.
- **Addressing**: `XNADDR` (`xnet.h:511`) carries `ina`/`inaOnline`/`wPortOnline`.
  `XLiveAPI` resolves peers to real `sockaddr_in` via the matchmaking server
  (`default_public_server_` heroku) + UPnP.

## Architecture

```
        guest title
            │  XSOCKADDR_IN (IP:port), VDP/UDP
            ▼
       ┌─────────┐   GNS enabled && identity mapped?
       │ XSocket │──────────────┬───────────────┐
       └─────────┘              │ yes           │ no (fallback)
                                ▼               ▼
                     ┌────────────────────┐   native sendto/recvfrom
                     │   GNSTransport     │   (existing code path)
                     │  (singleton)       │
                     │  - SteamNetworking │
                     │    Sockets/Messages│
                     │  - IP<->identity   │
                     │    registry        │
                     │  - callback pump   │
                     │  - custom signaling│──► standalone WS relay (separate Go
                     │    backend (plugin) │    repo) ──► forwards blobs to peer
                     │  - ICE/STUN/TURN   │──► peers (NAT traversal)
                     └────────────────────┘

  matchmaking server (heroku, REST) ──► session discovery / peer inaOnline list
                                        (unchanged; no longer carries signaling)
```

The signaling relay is decoupled from the matchmaking server: matchmaking gives
each client the peers' `inaOnline` (from which `peer_key` is derived), and the WS
relay forwards opaque ICE signal blobs between those `peer_key`s. The relay never
inspects payloads and holds no session state beyond `peer_key → live connection`.

## Work breakdown

### Phase 0 — Spike / de-risk — ✅ DONE (resolved by source analysis)

The single biggest unknown was: **does `ISteamNetworkingMessages` drive
connections through a *custom* signaling backend**, or does the convenience
layer assume a default (SDR) signaling?

**Answer: YES, Messages works with SDR-less custom signaling.** Verified
directly in the GNS source (pinned clone in `scratch/gns`, master @ v1.6.0):

- `ISteamNetworkingMessages::SendMessageToUser` →
  `CSteamNetworkingSockets::InternalConnectP2PDefaultSignaling`
  (`csteamnetworkingmessages.cpp:481`).
- That function does **not** hardcode SDR. It resolves a **global, user-settable
  callback** and fails loudly only if it's unset
  (`steamnetworkingsockets_p2p.cpp:2677`):

  ```cpp
  FnSteamNetworkingSocketsCreateConnectionSignaling fn =
      GlobalConfig::Callback_CreateConnectionSignaling.Get();
  if ( fn == nullptr ) { SpewBug("...CreateConnectionSignaling callback not set"); return nullptr; }
  ISteamNetworkingConnectionSignaling *pSignaling =
      (*fn)( this, identityRemote, nLocalVirtualPort, nRemoteVirtualPort );
  ```

  "Default signaling" means *"use the globally-installed CreateConnectionSignaling
  callback,"* not *"use SDR."* The same callback serves both `ConnectP2P` and
  Messages, so **no need to fall back to `ConnectP2PCustomSignaling`.**

**Confirmed concrete API path (this is what Phase 2 implements):**

1. **Outbound signaling**: install our factory via
   `SteamNetworkingUtils()->SetGlobalConfigValuePtr(`
   `k_ESteamNetworkingConfig_Callback_CreateConnectionSignaling /* =206 */, fn)`.
   `fn` (type `FnSteamNetworkingSocketsCreateConnectionSignaling`,
   `steamnetworkingcustomsignaling.h:105`) returns an
   `ISteamNetworkingConnectionSignaling*` whose `SendSignal()` ships the blob
   over our matchmaking server.
2. **Inbound signaling**: when our transport receives a signal blob, feed it to
   `SteamNetworkingSockets()->ReceivedP2PCustomSignal(pMsg, cbMsg, pContext)`
   (`isteamnetworkingsockets.h:787`), where `pContext` implements
   `ISteamNetworkingSignalingRecvContext` (`OnConnectRequest` /
   `SendRejectionSignal`, `steamnetworkingcustomsignaling.h:60`).
3. **Session accept**: `SteamNetworkingUtils()->SetGlobalCallback_MessagesSessionRequest(fn)`;
   in `fn`, call `SteamNetworkingMessages()->AcceptSessionWithUser(identity)`.
4. **Peer identity**: `SteamNetworkingIdentity::SetGenericString()` (or a raw
   custom type) derived from a stable per-peer key.
5. **NAT traversal**: built-in ICE client
   (`steamnetworkingsockets_ice_client.cpp`, ~2.8k LoC) compiled via
   `ENABLE_ICE` (`-DUSE_STEAMWEBRTC=OFF` — no Google WebRTC/abseil). STUN/TURN
   servers configured via `k_ESteamNetworkingConfig_P2P_STUN_ServerList` /
   `..._TURN_*`.

> Note: the bundled `examples/trivial_signaling_client.cpp` demonstrates the
> *connection-oriented* path (`ReceivedP2PCustomSignal`), not Messages, so there
> is no copy-paste Messages example — but the dispatch trace above proves the
> Messages path uses the identical signaling plumbing.

**Dependency findings from Phase 0 (these refine Phase 1, see Risk #2):**

- **Crypto can be fully in-tree on Windows.** `USE_CRYPTO=BCrypt` (Win-only,
  Xenia already links `bcrypt`) for AES/SHA256 + `USE_CRYPTO25519=Reference`
  (bundled `external/{curve25519,ed25519}-donna`, gated by the always-on
  `-DVALVE_CRYPTO_ENABLE_25519`) ⇒ **no OpenSSL/libsodium needed on Windows.**
  Linux/macOS have no BCrypt → use libsodium or OpenSSL there.
- **protobuf is the only unavoidable external dep.** 3 `.proto` files
  (`src/common/steamnetworkingsockets_messages{,_certs,_udp}.proto`) are codegen'd
  via `protobuf_generate_cpp`. Needs both `protoc` (build-time) and `libprotobuf`
  (link). Xenia does not currently vendor protobuf.
- **GNS bundles the rest**: `tier0/ tier1/ vstdlib/`, `external/vjson` (ICE
  signaling JSON), `external/sha1-wpa`. No system deps for these.
- Local toolchain already has `cmake 4.2`, `ninja 1.13`, `protoc (libprotoc 32)`;
  **no vcpkg**. (protoc 32 = protobuf 6.x — newer than GNS's usual 3.x pin;
  validate version compatibility when vendoring protobuf in Phase 1.)

### Phase 1 — Build integration (no behavior change) — ✅ DONE

**Verified**: `GameNetworkingSockets.lib` builds (all sources incl. native ICE,
P2P, SNP, BCrypt crypto, donna 25519, and the 3 protoc-generated `.pb.cc`), and
the full `xenia_canary_netplay.exe` links cleanly with GNS + libprotobuf — no
missing/duplicate symbols. MSVC x64 Debug. No runtime behavior change yet (GNS is
linked but unreferenced by xenia source).

Implementation notes / deviations discovered during the build:
- The hand-written target must define **`VALVE_CRYPTO_ENABLE_25519`** (GNS's own
  CMake adds it via global `add_definitions`); without it the `CCrypto` 25519 key
  functions are `#ifdef`-compiled out and `csteamnetworkingsockets.cpp` /
  `_connections.cpp` fail to compile.
- Did **not** define `GOOGLE_PROTOBUF_NO_RTTI` and left MSVC default `/EHsc` +
  RTTI on (Xenia defaults); protobuf built with matching settings, so no ODR
  mismatch. Simpler than replicating GNS's `/EHs-c-` + `/GR-`.
- Building from a bare shell fails (`Cannot open include file: 'string'`) — the
  MSVC `INCLUDE`/`LIB` env must be present. Build from a VS Developer shell
  (`Enter-VsDevShell ... -DevCmdArguments "-arch=x64"`).
- protobuf pin `v21.12` = annotated tag → commit `f0dc78d7e` (reports 3.21.12.0).

**protobuf decision (locked):** pin **protobuf v21.12 (3.21.12)** — the last
release before Abseil became a hard dependency — as a submodule. Build its
bundled `protoc` from source so generated code always matches the linked
runtime (no system-protoc version mismatch; the local `protoc` 6.x is
incompatible with a 3.x runtime). Full (not lite) runtime because the `.proto`
files are `proto2` + `optimize_for = SPEED`.

1. Add submodule `third_party/GameNetworkingSockets`.
2. Add submodule `third_party/protobuf` pinned to tag `v21.12`.
3. Protobuf target: protobuf 3.21 is too large to hand-write, so bring it in via
   `add_subdirectory(protobuf EXCLUDE_FROM_ALL)` with cache options
   (`protobuf_BUILD_TESTS=OFF`, `protobuf_BUILD_PROTOC_BINARIES=ON`,
   `protobuf_MSVC_STATIC_RUNTIME=OFF`). This deviates from the hand-written
   third-party pattern by necessity — document it inline. Yields targets
   `libprotobuf` + `protoc`.
4. Codegen: `add_custom_command` runs the freshly-built `protoc` on the 3
   `.proto` files → `.pb.cc/.pb.h` in the build dir.
5. Author a hand-written static target `GameNetworkingSockets` in
   `third_party/CMakeLists.txt` (modeled on `miniupnp`):
   - Sources: `src/steamnetworkingsockets/**` (incl. ICE client), `src/common/**`,
     `src/tier0 tier1 vstdlib`, bundled `src/external/{curve25519-donna,
     ed25519-donna,sha1-wpa,vjson}`, plus the generated `.pb.cc`.
   - Includes: `include/`, `src/`, `src/public/`, `src/common/`.
   - Defines: `STEAMNETWORKINGSOCKETS_STATIC_LINK`, `ENABLE_ICE`,
     `VALVE_CRYPTO_ENABLE_25519`, `STEAMNETWORKINGSOCKETS_CRYPTO_BCRYPT`
     (Win) + `STEAMNETWORKINGSOCKETS_CRYPTO_25519_DONNA`, `GOOGLE_PROTOBUF_NO_RTTI`.
   - `target_link_libraries(... libprotobuf ws2_32 crypt32 bcrypt winmm)` on Win32.
6. Add `GameNetworkingSockets` to `xenia-kernel`'s `target_link_libraries`
   (`src/xenia/kernel/CMakeLists.txt:24`).
7. **Gate**: builds clean on MSVC x64 (Debug/Release/Checked). Linux/clang +
   libsodium deferred. No runtime change yet.

### Phase 2 — Transport singleton — ✅ DONE

Implemented in `src/xenia/kernel/gns_transport.{h,cc}`; compiles and links into
the full `xenia_canary_netplay.exe` (GNS symbols resolve). No runtime behavior
change yet — nothing constructs/initializes the transport (Phases 3–5).

Implementation notes / decisions:
- **Public header is GNS-free.** No `<steam/*>` types leak out: the registry is
  keyed by a `uint64_t peer_key` (peer's stable id) and `uint32_t guest_ina`;
  identities are derived internally as generic strings `"xe:"+16 hex`. Keeps
  `STEAMNETWORKINGSOCKETS_STATIC_LINK` and GNS headers out of consumers.
- **Datagram framing**: chose a 4-byte header (`be16 src_port`,`be16 dst_port`)
  on a single GNS channel (0) over using the GNS channel for the port — a single
  `ReceiveMessagesOnChannel(0,…)` drain loop vs. polling every possible channel.
  Receive handler signature: `(src_ina, src_port, dst_port, data, len)`.
- **Signaling glue** (`CreateConnectionSignaling` cb, `MessagesSessionRequest`
  cb, `ISteamNetworkingConnectionSignaling` / `ISteamNetworkingSignalingRecvContext`)
  delegates actual blob transport to a pluggable `GNSSignalingBackend`
  (Phase 4b supplies the standalone WS-relay-backed impl; null backend until
  then, so no connections form yet). `DeliverInboundSignal()` →
  `ReceivedP2PCustomSignal`.
- **Pump thread**: a `std::thread` calling `RunCallbacks()` + `Service()` every
  ~2 ms; never blocks guest threads. Registry under `std::shared_mutex`.
- **Cross-platform**: GNS-specific code is gated behind `XE_GNS_ENABLED` (set by
  CMake only on Win x64); elsewhere the methods are inert stubs so the kernel
  still builds. `Initialize(local_peer_key)` passes our derived identity to
  `GameNetworkingSockets_Init`; sets the STUN default + ICE.
- New cvar `gns` (default false) gates use; consulted via `IsEnabled()`.

Original sketch follows.

New files `src/xenia/kernel/gns_transport.{h,cc}` (lives in `xenia-kernel`):

```cpp
class GNSTransport {
 public:
  static GNSTransport* Get();              // lazy singleton, cvar-gated
  bool Initialize();                        // GameNetworkingSockets_Init + lib init
  void Shutdown();
  void Pump();                              // RunCallbacks + drain ICE; called on a worker

  // Registry — populated at session join.
  void MapPeer(uint32_t guest_ina, const SteamNetworkingIdentity& id);
  void UnmapPeer(uint32_t guest_ina);
  std::optional<SteamNetworkingIdentity> Resolve(uint32_t guest_ina) const;
  std::optional<uint32_t> ReverseResolve(const SteamNetworkingIdentity&) const;

  // Datagram I/O used by XSocket.
  bool SendTo(uint32_t guest_ina, uint16_t port, int channel,
              const uint8_t* buf, size_t len, bool reliable);
  // Drains received messages, demuxes by (identity->guest_ina, channel/port).
  void Service();
 private:
  ISteamNetworkingSockets* sockets_;
  ISteamNetworkingMessages* messages_;
  std::shared_mutex registry_mutex_;
  // boost::bimap-style: guest_ina <-> identity
};
```

Details:
- **Identity scheme**: `SteamNetworkingIdentity::SetGenericString()` seeded from
  the peer's stable key (machine id / MAC / XUID already known to `XLiveAPI`).
  Deterministic so both ends derive the same identity.
- **Channel/port**: the guest port must survive the trip. Carry it as the GNS
  `nRemoteChannel` (or a small framing header) so the receiver can deliver to the
  correct bound `XSocket`. VDP framing
  (`[cbGameData][GameData][VoiceData]`, `xsocket.h:124`) is opaque payload —
  passed through untouched.
- **Signaling**: implement `ISteamNetworkingSignalingRecvContext` /
  `ISteamNetworkingConnectionSignaling`; transport signaling blobs over the
  pluggable `GNSSignalingBackend` (Phase 4b: a dedicated WS relay in a separate
  repo, reached via `gns_signaling_url` — *not* the matchmaking server).
- **Pump thread**: one background thread calling `RunCallbacks()` + `Service()`;
  do **not** block the guest thread.

### Phase 3 — XSocket seam — ✅ DONE (compiles/links; inert until Phases 4-5)

Implemented in `xsocket.{h,cc}`; builds into the app. Default-inert: with cvar
`gns` off (and no `Initialize`), `use_gns_` stays false and every path is the
original native code — zero behavior change. The GNS data path can't be
runtime-exercised until the registry is populated (Phase 4) and the transport is
initialized (Phase 5).

What landed:
- **`use_gns_`** decided in `Bind()` via `MaybeEnableGNS()` (cvar on + DGRAM +
  transport initialized). Connected-UDP (`Connect`+`Send`/`Recv`) stays native
  this phase — deferred to avoid a half-wired `Recv()`.
- **Port→socket registry**: `static std::map<uint16_t, XSocket*> gns_sockets_`
  (host-order bound port → socket), registered in `MaybeEnableGNS`, unregistered
  in `Close()` under the same mutex that `DeliverGNSPacket` holds during
  delivery (keeps the target alive across the QueuePacket call).
- **Receive dispatch**: `static XSocket::DeliverGNSPacket(...)` installed once as
  the `GNSTransport` receive handler; looks up `dst_port` and calls the existing
  `QueuePacket`, which now also signals a new `incoming_packet_cv_`.
- **`SendTo`**: if `use_gns_` and the destination IP is a mapped peer →
  `GNSTransport::SendTo` (unreliable for UDP/VDP); else native `sendto`
  (broadcast/LAN/unmapped real hosts). Bypasses UPnP mapping on the GNS path.
- **Recv paths**: `RecvFrom` → `RecvFromGNS` (non-blocking; WOULDBLOCK when
  empty). `PollWSARecvFrom` → `PollWSARecvFromGNS`, which waits on
  `incoming_packet_cv_` (respecting the overlapped abort flag) and copies the
  queued packet into the guest `WSABUF`s, reusing the same overlapped
  completion/event/`receive_cv_` bookkeeping as the native poll. `Close()` wakes
  the cv so a pending poll sees the abort promptly.
- **Native socket kept** in parallel (Bind still calls native `bind`) so
  broadcast/LAN discovery and unmapped sends still work.

Byte-order convention (carried through the framing header + registry): IPs are
passed as the raw `in_addr.s_addr` value (opaque); ports as host-order port
numbers. Both round-trip transparently via `xe::be<>` on store/load.

Known limitations to revisit: connected-UDP via `Send`/`Recv`; blocking sync
`RecvFrom` (currently non-blocking under GNS); `SO_REUSEADDR` two-sockets-one-port
(registry keeps the last).

Original sketch follows.

### Phase 3 — XSocket seam

Branch only for connectionless guest sockets (`X_SOCK_DGRAM`, UDP/VDP). TCP and
all the `WSARecvFrom`/setsockopt/ioctl machinery stay native for now.

- Add `bool use_gns_` to `XSocket`, decided at `Bind`/first-send time:
  `use_gns_ = cvars::gns_enabled && type_==X_SOCK_DGRAM && GNSTransport::Get()`.
- **`SendTo`** (`xsocket.cc:665`): if `use_gns_`, look up
  `GNSTransport::Resolve(to->address_ip)`. Hit → `GNSTransport::SendTo(...)`.
  Miss → existing native `sendto` (fallback, e.g. broadcast/LAN). Keep the
  implicit-bind bookkeeping.
- **`RecvFrom`** (`xsocket.cc:388`): when `use_gns_`, drain from the (now active)
  `incoming_packets_` queue that `GNSTransport::Service()` fills via
  `QueuePacket(src_ina, src_port, ...)`. Reconstruct the `from` `XSOCKADDR_IN`
  from the reverse registry. Native path unchanged otherwise.
- **`WSARecvFrom` / `PollWSARecvFrom`**: the async overlapped path polls the
  same queue instead of `WSAPoll(native_handle_)` when `use_gns_`. The condition
  variable `receive_cv_` is signalled by `QueuePacket`. This reuses the existing
  overlapped completion machinery (`active_overlapped_`, event handles).
- **`Bind`**: when `use_gns_`, binding is logical (record `bound_port_`); no real
  `bind()` needed unless we keep a native socket for fallback. Decision: keep a
  native UDP socket open in parallel so broadcast/LAN discovery still works — GNS
  handles only mapped peers.

### Phase 4 — Registry population + standalone signaling

Split into two independent sub-phases. 4a is pure host-side wiring (no network
service); 4b stands up the signaling carrier. They share only the `peer_key`
notion and can be built/tested in either order — 4a populates the registry that
decides `use_gns_`; 4b makes connections actually form.

#### Phase 4a — Registry population (host-side)

In `XLiveAPI` session join / `SessionJoinRemote` / `XNetXnAddrToInAddr` paths:
when a peer's `inaOnline` is assigned, derive its `peer_key`
(`GNSTransport::PeerKeyFromIdentityString` over the identity string built from
`inaOnline`) and call `GNSTransport::MapPeer(inaOnline, peer_key)`. On
leave → `UnmapPeer`. This is the only place the two address spaces meet, so keep
the mapping logic centralized here.

#### Phase 4b — Standalone WebSocket signaling relay + client backend

**Decision (locked):** signaling is carried by a **dedicated WebSocket relay**,
*not* the matchmaking (heroku) REST API. The REST API is request/response only,
so it can't push unsolicited inbound ICE signals without bolting on a polled
inbox; a persistent WS connection gives real server→client push, which is what
ICE setup wants (small, bursty, bidirectional, latency-sensitive blobs). The
relay is also decoupled from the matchmaking server's release/scaling lifecycle.

**The relay server — separate repository.** The Go service lives in its **own
repo**, not vendored in this tree (it has no build/runtime coupling to Xenia and
is deployed/hosted independently). It is a dumb, opaque blob forwarder:

- Stack: Go single static binary, `nhooyr.io/websocket` or `gorilla/websocket`,
  a `map[peer_key]*conn` guarded by a mutex (or `sync.Map`). ~120 LoC.
- It never inspects or decodes signal payloads and holds no session state beyond
  `peer_key → live connection`. No Xbox/Steam/session awareness.
- Wire protocol — **binary** WebSocket frames (no JSON/base64; the client has no
  base64 helper and the payloads are binary ICE blobs):

  ```
  HELLO  (client -> relay):  [0x01][u64 LE local_peer_key]      // register on connect
  SIGNAL (client -> relay):  [0x02][u64 LE dest_peer_key][blob] // forward a blob
  SIGNAL (relay  -> client): [0x02][u64 LE src_peer_key ][blob] // delivered to target
  ```

  The relay tracks `peer_key → conn` from HELLO; on a client→relay SIGNAL it
  looks up `dest_peer_key`, and forwards `[0x02][src_peer_key][blob]` to that
  conn (rewriting the key field to the sender's). If the target has no live
  connection, drop it — GNS/ICE re-sends signals, so transient gaps self-heal.
  The `blob` is GNS's rendezvous payload and is self-describing (carries the
  sender identity), so the client ignores the inbound `src_peer_key`.

**The client backend — in this tree.** A concrete
`StandaloneSignalingBackend : GNSSignalingBackend`, registered via
`GNSTransport::SetSignalingBackend()` (the Phase 2 seam — transport code is
unchanged):

- Reuse **in-tree libcurl's WebSocket API** (`curl_ws_send`/`curl_ws_recv`,
  `third_party/libcurl/lib/ws.c`) — no new dependency. **Caveat:** the `libcurl`
  target is built with `HTTP_ONLY` (`third_party/CMakeLists.txt:842`); verify
  WebSockets isn't compiled out (`CURL_DISABLE_WEBSOCKETS`) and flip the one
  define if needed. (Fallback if WS is impractical via this curl build: a small
  header-only WS client — but try libcurl first.)
- Opens a WS connection to `cvars::gns_signaling_url` on
  `GNSTransport::Initialize`; sends `hello` with `local_peer_key` on
  connect/reconnect; reconnect-with-backoff.
- `SendSignal(peer_key, data, len)` → enqueue a binary SIGNAL frame; a single
  worker thread owns the curl handle and drains the queue via `curl_ws_send`
  (curl handles aren't thread-safe, so all curl calls stay on that thread).
- Read loop (the worker thread): inbound SIGNAL → strip the 9-byte header →
  `GNSTransport::DeliverInboundSignal(blob, len)` → `ReceivedP2PCustomSignal`.
- Reconnect-with-backoff (250 ms → 5 s); the worker reassembles fragmented WS
  frames before dispatch.

**Status:** libcurl WS enabled (verified: `curl_ws_*` symbols present), and
`src/xenia/kernel/gns_signaling.{h,cc}` + the `gns_signaling_url` cvar compile
and link into the app. Inert until Phase 5 constructs the backend, calls
`Start(local_peer_key)`, and installs it via `SetSignalingBackend()` alongside
`GNSTransport::Initialize`.

**No chicken-and-egg:** the routing key (`peer_key`) is derived from the peer's
`inaOnline`, which the matchmaking server already provided at session join
(Phase 4a). The relay needs no prior knowledge of who's who.

**New cvar:** `gns_signaling_url` (e.g. `wss://relay.example/ws`). The
matchmaking `api_address` is untouched.

### Phase 5 — Config, lifecycle, polish — ✅ DONE (lifecycle + cvars)

This is the first phase where the `gns` cvar actually does something end-to-end:
with `gns` on and `gns_signaling_url` set, the transport + signaling come up at
netplay startup. Compiles and links into `xenia_canary_netplay.exe` (the
`curl_ws_*` symbols are now referenced and resolve).

What landed:
- **Lifecycle in `XLiveAPI`.** `StartGNS()` runs from `XLiveAPI::Init()` once
  online: derives `local_peer_key` from `GetConsoleMacAddress()`, calls
  `GNSTransport::Initialize(local_peer_key)`, constructs a
  `StandaloneSignalingBackend`, `Start()`s it, and installs it via
  `SetSignalingBackend()`. `StopGNS()` (from `~XLiveAPI`) detaches the backend
  first (so no in-flight GNS callback dereferences it), stops the worker, then
  `Shutdown()`s the transport. No-op unless `gns` is set. If no
  `gns_signaling_url` is configured the transport still comes up but signaling
  doesn't start (logged), so no P2P forms — matching the native-fallback intent.
- **ICE cvars** (defined in `gns_transport.cc`, applied in `Initialize`):
  `gns_stun_server_list` (replaces the hardcoded STUN default),
  `gns_turn_server_list` + `gns_turn_username` + `gns_turn_password` (optional
  TURN relay), and `gns_enable_relay` (bool) which sets
  `k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable` to `All` vs.
  `Private|Public` (direct-only).
- **Logging** via `XELOGI/W/E` at each lifecycle step; the existing GNS debug
  output hook (`DebugOutputFn`) already surfaces ICE/connection spew.

Deferred polish (not blocking; revisit when runtime-testing against a relay):
- Surfacing ICE state (direct vs. relayed) as a user-visible netplay status.
- Mapping GNS connection errors to `X_WSAError`. Today the `XSocket` GNS path
  already falls back to native on send failure, so the guest still gets a
  sensible Winsock result without an explicit mapping; add one if a title needs
  to observe GNS-specific failures.

### Phase 6 — TCP connectivity over GNS (after UDP/VDP proves out)

The UDP/VDP path (Phases 2–5) uses `ISteamNetworkingMessages` (connectionless).
TCP sockets (`X_SOCK_STREAM`/`X_IPPROTO_TCP`) instead use GNS's
**connection-oriented** API, which is a reliable, ordered stream — a natural fit
for TCP — reusing the *same* signaling, `guest-IP ⇄ SteamNetworkingIdentity`
registry, and ICE/NAT traversal. This is a high-value addition: native P2P TCP
NAT traversal is nearly impossible (TCP hole-punching is far less reliable than
UDP), so games doing peer-to-peer TCP simply work where they don't today.

The two GNS modes coexist cleanly: Messages uses its reserved virtual port; each
TCP socket uses its own per-guest-port virtual port to the same peer.

**Hard boundary — peer-to-peer TCP only.** GNS P2P requires a GNS peer with an
identity on the other end. TCP to a *real* server — XLSP title servers
(`X_TITLE_SERVER` / `GetServers`), HTTP, Xbox Live endpoints — has no GNS peer
and **must stay native**. Same registry-lookup decision as UDP: destination IP
resolves to a mapped peer → GNS; otherwise native fallback. The bind-time
`use_gns_` decision (Phase 3) extends to TCP via this same lookup.

**API mapping** (connection-oriented `ISteamNetworkingSockets`):

| Xbox TCP call | GNS |
| --- | --- |
| `Connect` | `ConnectP2P(identity, vport=dest port)` |
| `Listen` | `CreateListenSocketP2P(vport=bound port)` |
| `Accept` | `AcceptConnection` from the `ConnectionStatusChanged` callback → wrap the new handle in a fresh `XSocket` (mirrors the existing native `Accept` that builds a child `XSocket`) |
| `Send` | `SendMessageToConnection(..., k_nSteamNetworkingSend_Reliable)` |
| `Recv` | drain reliable messages from the connection |
| `Shutdown` / `Close` | `LingerClose` (flush pending reliable) / `CloseConnection` |

**Work items / semantic gaps:**
1. **Byte-stream reassembly (the one substantial new piece).** TCP is a
   boundary-less byte stream; GNS delivers discrete messages. Add a per-connection
   receive buffer: append each received (ordered, reliable) message, and let
   `Recv` return arbitrary N-byte chunks from it. Send side just packages the
   bytes into a reliable message.
2. **Listen/accept wiring.** Map the listen virtual port to the guest bound port;
   on inbound `ConnectionStatusChanged → Connecting`, accept and queue the new
   connection for the guest's next `Accept`, reconstructing the peer
   `XSOCKADDR_IN` from the reverse registry.
3. **Blocking recv.** Blocking TCP `Recv` becomes a cv-wait on the reassembly
   buffer fed by the `GNSTransport` pump thread — same shape as the UDP async
   path (`receive_cv_`).
4. **Half-close gap.** TCP `shutdown(SD_SEND)` (FIN, keep reading) has no exact
   GNS equivalent (`LingerClose` is bidirectional). Most titles don't rely on
   half-close; document the difference and revisit if a title needs it.
5. **Error/state mapping.** Map GNS connection-state changes
   (`ProblemDetectedLocally`, `ClosedByPeer`) to the Winsock errors the guest
   expects (connection reset/refused/closed).

> Note: `ISteamNetworkingMessages` is itself implemented on top of P2P
> connections internally, so an alternative is to drop Messages and run
> *everything* over connection-oriented `ConnectP2P` with per-socket reliability
> flags (unreliable for UDP/VDP, reliable for TCP). We keep Messages for UDP for
> the connectionless convenience that motivated this project; this note records
> the unification option if the two code paths ever feel redundant.

### Phase 7 — Dedicated server (`open-combas-server`) over GNS

The Chromehounds dedicated server (`build/open-combas-server`, pure Go) runs an
array of UDP sub-servers, each bound to its own port on a single listening
address. Clients find it via the matchmaking server (`XOnlineQuerySearch` →
`GetServers`) and address it via `XNetTsAddrToInAddr` / `XNetServerToInAddr`,
which resolve the whole server to **one** `inaOnline`. We route those
connections over GNS too, gaining the same NAT traversal.

**Key insight — the server is ONE GNS peer; sub-server ports demux underneath.**
The title talks to a single server `inaOnline` with many destination ports, so a
single GNS session carries everything and the existing `[src_port][dst_port]`
framing header selects the sub-server. No per-port peers, no new framing.

**Decisions (locked):**
- **Server endpoint = a C++ gateway sidecar**, *not* cgo in the Go server. GNS is
  C++; the gateway reuses our `GNSTransport` + `StandaloneSignalingBackend`
  verbatim and bridges GNS ⇄ loopback UDP, so the Go server stays pure Go and
  unmodified. (cgo would drag a C++ toolchain + GNS + protobuf into the Go build
  and need hand-written wrappers — rejected.)
- **Server `peer_key` is derived from its `inaOnline`**, not advertised. Client
  and gateway both compute `GNSTransport::ServerPeerKeyFromIna(ina)` (the online
  IP in the low 32 bits, an `'SV'` tag in bits 48–63 to avoid colliding with the
  48-bit MAC keys used for console peers). No heroku/protocol change.

**Client side — DONE (builds):** `ServerPeerKeyFromIna` added to `GNSTransport`;
`NetDll_XNetServerToInAddr` and `NetDll_XNetTsAddrToInAddr` now `MapPeer(server
ina, ServerPeerKeyFromIna(ina))` when `gns` is enabled. Once mapped, the Phase 3
`XSocket` seam routes sends to the server over GNS automatically — no further
client work.

**Gateway side — DONE (builds + runs).** New executable target `gns-gateway`
(`src/xenia/tools/gns_gateway/`, Win x64 only) that compiles
`gns_transport.cc` + `gns_signaling.cc` + `gns_gateway_main.cc` and links
`xenia-base` + `GameNetworkingSockets` + `libcurl` (+ `libprotobuf` transitively).
It is a NAT: GNS outside, plain UDP to localhost inside, framing header as the
port-demux key.
1. Args `--server-ip <online-ip> --signaling-url <ws[s]://…> [--loopback
   127.0.0.1]`; sets `cvars::gns`/`gns_signaling_url`, then
   `GNSTransport::Initialize(ServerPeerKeyFromIna(server_ina))` + a
   `StandaloneSignalingBackend` + `SetSignalingBackend`. Registers with the relay
   under the server's `peer_key`, so client signals reach it.
2. `SetReceiveHandler`: each inbound `(client_ina, src_port, dst_port, data)`
   forwards `data` to `127.0.0.1:dst_port` on a per-flow loopback socket keyed by
   `(client_ina, src_port, dst_port)`, spawning a reader thread.
3. Reader: loopback reply → `GNSTransport::SendTo(client_ina, dst_port, src_port,
   …)` back to the client (ports swapped). Janitor reaps flows idle > 30 s.

**Transport change for server mode:** `GNSTransport::set_auto_register_inbound`
+ `ResolveOrRegisterInbound`. Clients are dynamic, so an inbound datagram from an
unregistered peer auto-assigns it a synthetic `guest_ina` (so the receive handler
fires and `SendTo` can route the reply). The client leaves this off, so its
behavior is unchanged.

**Build note:** `gns_gateway_main.cc` defines `NOMINMAX` before `<windows.h>` so
the Windows `min`/`max` macros don't break the xenia headers it also includes.

**Still runtime-untested** end-to-end (needs the signaling relay + the Go server
running). The gateway binary builds and starts; the GNS path is exercised once a
relay is live.

#### Cross-platform gateway (Linux via OpenSSL)

The server runs primarily on Linux, so the GNS stack used by the gateway is
cross-platform. Crypto backend is **per-platform** (Windows keeps BCrypt — the
client build is unchanged and gains no OpenSSL dependency; Linux uses OpenSSL):

- `GameNetworkingSockets` target (`third_party/CMakeLists.txt`) now builds on
  `XE_TARGET_X86_64 AND (WIN32 OR Linux)`. Crypto sources/defines/links are
  chosen by platform: Win → `crypto_bcrypt.cpp` + donna 25519 +
  `VALVE_CRYPTO_BCRYPT/_25519_DONNA/ED25519_HASH_BCRYPT` + `bcrypt`; Linux →
  `find_package(OpenSSL)` + `crypto_openssl.cpp` / `crypto_25519_openssl.cpp` /
  `crypto_{digest,symmetric}_opensslevp.cpp` / `opensslwrapper.cpp` +
  `VALVE_CRYPTO_OPENSSL/_25519_OPENSSLEVP` + `OpenSSL::Crypto`. Platform macro
  `_WINDOWS` vs `POSIX LINUX`; Linux also links `Threads::Threads`.
- `gns-gateway` target moved out of the Windows-only block; builds on Win+Linux
  x64. Links the vendored Schannel `libcurl` on Windows, **system libcurl**
  (`find_package(CURL)`) on Linux.
- `gns_gateway_main.cc` uses a small socket shim (`#ifdef _WIN32` winsock vs
  POSIX `sys/socket.h`); `gns_signaling.cc` includes `<curl/...>` on non-Windows.

**Linux build prerequisites** (the gateway is wired but compile-verified only on
Windows — Linux is verified by you on a Linux box / WSL / CI):
- OpenSSL ≥ 1.1.1 dev headers (`libssl-dev`) — for raw 25519 EVP keys.
- libcurl dev headers built **with the WebSocket API** (`curl_ws_*`, libcurl
  ≥ 7.86 with websockets enabled). Older distro packages may omit it.
- A C++20 toolchain (the gateway target uses `-Werror`; new GCC/Clang warnings in
  the reused TUs may need fixing on first Linux compile).
- Build just the gateway: `cmake --preset … && cmake --build … --target
  gns-gateway`.

## Risks & open questions

1. ~~**Messages-vs-connection signaling** (Phase 0 exit criterion).~~
   **RESOLVED in Phase 0** — Messages uses the global `CreateConnectionSignaling`
   callback; SDR-less custom signaling is fully supported. No fallback needed.
2. **GNS dependency tree.** **protobuf is the only external dep** (Windows:
   BCrypt + bundled Reference 25519 cover all crypto; bundled `tier0/tier1/vstdlib/vjson`
   cover the rest). Phase 1's main effort is vendoring protobuf as a `third_party`
   static target + wiring the `protoc` codegen step for the 3 `.proto` files, and
   replicating GNS's source list in a hand-written CMake target (matching Xenia's
   model — we do **not** use GNS's own `CMakeLists.txt`). **Validate protobuf
   version compatibility** (local `protoc` is 6.x; GNS historically targets 3.x).
   On Linux/macOS, add libsodium **or** OpenSSL for crypto (no BCrypt there).
3. **Port preservation.** Guest port must round-trip through GNS
   (channel or framing header). Needs a concrete wire decision in Phase 2.
4. **Broadcast / system-link.** GNS is point-to-point; `0xFFFFFFFF` broadcast and
   LAN discovery stay on the native fallback socket. Confirm no title relies on
   broadcast reaching mapped (internet) peers.
5. **Ordering/reliability semantics.** Guest UDP is unreliable/unordered. Send
   GNS messages unreliable (`k_nSteamNetworkingSend_Unreliable`) to match; do not
   silently upgrade to reliable.
6. **MTU / fragmentation.** GNS fragments/reassembles, but the guest may assume
   ~1.4KB UDP MTU. Verify large VDP payloads behave.

## Sources

- [GameNetworkingSockets — README](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/README.md)
- [README_P2P.md (custom signaling, ICE/STUN/TURN)](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/README_P2P.md)
- [steamnetworkingcustomsignaling.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingcustomsignaling.h)
- [ISteamNetworkingMessages / Steamworks docs](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets)
