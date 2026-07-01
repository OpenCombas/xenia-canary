/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// GNS gateway sidecar (docs/gns_integration.md Phase 7).
//
// Gives a dedicated title server (e.g. the pure-Go open-combas-server) a GNS
// endpoint without touching its code. It is essentially a NAT: GNS on the
// outside, plain UDP to 127.0.0.1:<sub-port> on the inside, with our framing
// header's dst_port selecting the sub-server.
//
//   client ==GNS==> [gateway] ==UDP==> 127.0.0.1:dst_port (Go sub-server)
//   client <==GNS== [gateway] <==UDP== 127.0.0.1:dst_port
//
// Usage:
//   gns-gateway --server-ip <online-ip> --signaling-url <ws[s]://host/path>
//               [--loopback 127.0.0.1]
//
// --server-ip is the server's online IP as the client resolves it
// (X_TITLE_SERVER / TSADDR inaOnline); both sides derive the same peer_key from
// it via GNSTransport::ServerPeerKeyFromIna. Cross-platform: Windows + Linux.

// clang-format off
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // keep windows.h min/max macros from breaking xenia headers
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif
// clang-format on

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/gns_signaling.h"
#include "xenia/kernel/gns_transport.h"

// Defined in gns_transport.cc / gns_signaling.cc (compiled into this target).
DECLARE_bool(gns);
DECLARE_string(gns_signaling_url);

namespace {

using xe::kernel::GNSTransport;
using xe::kernel::StandaloneSignalingBackend;

// --- Minimal cross-platform socket shim --------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
int CloseSocket(socket_t s) { return closesocket(s); }
int SocketError() { return WSAGetLastError(); }
bool IsRecvTimeout(int err) { return err == WSAETIMEDOUT; }
#else
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
int CloseSocket(socket_t s) { return close(s); }
int SocketError() { return errno; }
bool IsRecvTimeout(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

void SetRecvTimeout(socket_t s, int seconds) {
#ifdef _WIN32
  DWORD ms = static_cast<DWORD>(seconds) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
             sizeof(ms));
#else
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}
// -----------------------------------------------------------------------------

std::string g_loopback_ip = "127.0.0.1";
std::atomic<bool> g_running{true};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// One client<->sub-server UDP flow, keyed by (client, src_port, dst_port).
struct Flow {
  socket_t sock = INVALID_SOCKET;
  std::thread reader;
  std::atomic<bool> running{true};
  std::atomic<int64_t> last_active_ms{0};
};

struct FlowKey {
  uint32_t client_ina;
  uint16_t src_port;
  uint16_t dst_port;
  bool operator<(const FlowKey& o) const {
    return std::tie(client_ina, src_port, dst_port) <
           std::tie(o.client_ina, o.src_port, o.dst_port);
  }
};

std::mutex g_flows_mutex;
std::map<FlowKey, std::unique_ptr<Flow>> g_flows;

// Loopback -> GNS: read sub-server replies and ship them back to the client.
// The ports swap: the sub-server's dst_port is the reply's source, the client's
// src_port is its destination.
void ReaderLoop(Flow* flow, uint32_t client_ina, uint16_t src_port,
                uint16_t dst_port) {
  SetRecvTimeout(flow->sock, 1);

  std::vector<uint8_t> buf(64 * 1024);
  while (flow->running.load() && g_running.load()) {
    int n = static_cast<int>(recv(flow->sock,
                                  reinterpret_cast<char*>(buf.data()),
                                  static_cast<int>(buf.size()), 0));
    if (n > 0) {
      flow->last_active_ms.store(NowMs());
      GNSTransport::Get()->SendTo(client_ina, dst_port, src_port, buf.data(),
                                  static_cast<size_t>(n), /*reliable=*/false);
    } else if (n == 0) {
      break;
    } else {
      if (IsRecvTimeout(SocketError())) {
        continue;
      }
      break;
    }
  }
}

// GNS -> loopback: forward a client datagram to the matching sub-server,
// creating the flow (and its reader thread) on first use. Runs on the GNS pump
// thread.
void OnReceive(uint32_t client_ina, uint16_t src_port, uint16_t dst_port,
               const uint8_t* data, size_t len) {
  Flow* flow = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_flows_mutex);
    FlowKey key{client_ina, src_port, dst_port};
    auto it = g_flows.find(key);
    if (it == g_flows.end()) {
      auto f = std::make_unique<Flow>();
      f->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (f->sock == INVALID_SOCKET) {
        XELOGE("[gateway] socket() failed: {}", SocketError());
        return;
      }
      sockaddr_in dst{};
      dst.sin_family = AF_INET;
      dst.sin_port = htons(dst_port);
      inet_pton(AF_INET, g_loopback_ip.c_str(), &dst.sin_addr);
      if (connect(f->sock, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) !=
          0) {
        XELOGE("[gateway] connect(127.0.0.1:{}) failed: {}", dst_port,
               SocketError());
        CloseSocket(f->sock);
        return;
      }
      f->last_active_ms.store(NowMs());
      Flow* fp = f.get();
      fp->reader = std::thread([fp, client_ina, src_port, dst_port]() {
        ReaderLoop(fp, client_ina, src_port, dst_port);
      });
      it = g_flows.emplace(key, std::move(f)).first;
      XELOGI("[gateway] new flow client {:08x} {}->{}", client_ina, src_port,
             dst_port);
    }
    flow = it->second.get();
    flow->last_active_ms.store(NowMs());
  }

  int sent = static_cast<int>(send(flow->sock,
                                   reinterpret_cast<const char*>(data),
                                   static_cast<int>(len), 0));
  if (sent < 0) {
    XELOGW("[gateway] send to sub-server failed: {}", SocketError());
  }
}

// Reap flows idle past the timeout so per-flow sockets/threads don't accumulate.
void JanitorLoop() {
  constexpr int64_t kIdleTimeoutMs = 30000;
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const int64_t now = NowMs();
    std::vector<std::unique_ptr<Flow>> dead;
    {
      std::lock_guard<std::mutex> lock(g_flows_mutex);
      for (auto it = g_flows.begin(); it != g_flows.end();) {
        if (now - it->second->last_active_ms.load() > kIdleTimeoutMs) {
          it->second->running.store(false);
          dead.push_back(std::move(it->second));
          it = g_flows.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& f : dead) {
      if (f->sock != INVALID_SOCKET) {
        CloseSocket(f->sock);  // unblocks the reader's recv
      }
      if (f->reader.joinable()) {
        f->reader.join();
      }
    }
  }
}

void ShutdownAllFlows() {
  std::vector<std::unique_ptr<Flow>> dead;
  {
    std::lock_guard<std::mutex> lock(g_flows_mutex);
    for (auto& [key, flow] : g_flows) {
      flow->running.store(false);
      dead.push_back(std::move(flow));
    }
    g_flows.clear();
  }
  for (auto& f : dead) {
    if (f->sock != INVALID_SOCKET) {
      CloseSocket(f->sock);
    }
    if (f->reader.joinable()) {
      f->reader.join();
    }
  }
}

void HandleSigint(int) { g_running.store(false); }

bool ParseArgs(int argc, char** argv, std::string* server_ip,
               std::string* signaling_url, std::string* loopback) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](std::string* out) {
      if (i + 1 < argc) {
        *out = argv[++i];
        return true;
      }
      return false;
    };
    if (arg == "--server-ip") {
      if (!next(server_ip)) return false;
    } else if (arg == "--signaling-url") {
      if (!next(signaling_url)) return false;
    } else if (arg == "--loopback") {
      if (!next(loopback)) return false;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
      return false;
    }
  }
  return !server_ip->empty() && !signaling_url->empty();
}

}  // namespace

int main(int argc, char** argv) {
  std::string server_ip;
  std::string signaling_url;
  std::string loopback = "127.0.0.1";
  if (!ParseArgs(argc, argv, &server_ip, &signaling_url, &loopback)) {
    std::fprintf(stderr,
                 "Usage: gns-gateway --server-ip <online-ip> --signaling-url "
                 "<ws[s]://host/path> [--loopback 127.0.0.1]\n");
    return 1;
  }

  xe::InitializeLogging("gns-gateway");

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    XELOGE("[gateway] WSAStartup failed");
    return 1;
  }
#endif

  in_addr server_addr{};
  if (inet_pton(AF_INET, server_ip.c_str(), &server_addr) != 1) {
    XELOGE("[gateway] invalid --server-ip: {}", server_ip);
    return 1;
  }
  g_loopback_ip = loopback;

  // Match the cvars the reused transport/signaling code reads.
  cvars::gns = true;
  cvars::gns_signaling_url = signaling_url;

  const uint32_t server_ina = server_addr.s_addr;
  const uint64_t server_key = GNSTransport::ServerPeerKeyFromIna(server_ina);

  auto* transport = GNSTransport::Get();
  transport->set_auto_register_inbound(true);  // clients are dynamic
  if (!transport->Initialize(server_key)) {
    XELOGE("[gateway] GNS transport init failed");
    return 1;
  }

  StandaloneSignalingBackend backend;
  if (!backend.Start(server_key)) {
    XELOGE("[gateway] signaling backend failed to start (check signaling URL)");
    transport->Shutdown();
    return 1;
  }
  transport->SetSignalingBackend(&backend);
  transport->SetReceiveHandler(&OnReceive);

  std::signal(SIGINT, HandleSigint);
  std::signal(SIGTERM, HandleSigint);

  XELOGI("[gateway] up: server {} (key xe:{:016x}) signaling {} loopback {}",
         server_ip, server_key, signaling_url, loopback);
  std::fprintf(stderr, "gns-gateway running; Ctrl-C to stop\n");

  std::thread janitor(JanitorLoop);
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  XELOGI("[gateway] shutting down");
  if (janitor.joinable()) {
    janitor.join();
  }
  ShutdownAllFlows();
  transport->SetSignalingBackend(nullptr);
  backend.Stop();
  transport->Shutdown();
#ifdef _WIN32
  WSACleanup();
#endif
  xe::ShutdownLogging();
  return 0;
}
