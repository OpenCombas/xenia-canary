/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_AUTH_H_
#define XENIA_KERNEL_NETPLAY_AUTH_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace xe {
namespace kernel {

// Client side of the WebServices bearer-token auth (ADR-0002). The client is a
// persistent program, so it holds a durable opaque bearer token per account and
// presents it on every request (Authorization: Bearer for HTTP, ?token= for the
// WS). A password exists only to (re)issue tokens; per the operator decision the
// CLIENT auto-generates + stores it (the user never types one) and the recovery
// code -- shown once -- is the only human-facing secret. TOFU claim is folded
// into RegisterPlayer.
//
// SOFT ROLLOUT: everything is "send if present". Until the server ships /auth/*
// (RegisterPlayer returning a token), no token is stored and the client behaves
// exactly as today. Secrets persist per-xuid to netplay_auth.json next to the
// executable (plaintext, matching the existing config model; TLS protects
// transit). Singleton.
class NetplayAuth {
 public:
  static NetplayAuth* Get();

  // The stored bearer token for `xuid`, or "" if none (soft phase / unclaimed).
  std::string GetToken(uint64_t xuid);

  // The stored auto-generated password for `xuid`, generating + persisting one
  // if absent. Sent with the RegisterPlayer claim / re-auth.
  std::string EnsurePassword(uint64_t xuid);

  // Persist a fresh claim (RegisterPlayer returned token+recoveryCode). Queues
  // the recovery code to be shown once, and (if the emulator UI is up) pops the
  // "save your recovery code" dialog.
  void StoreClaim(uint64_t xuid, const std::string& token,
                  const std::string& recovery_code);

  // Persist a re-issued token (POST /auth/token) with no new recovery code.
  void StoreToken(uint64_t xuid, const std::string& token);

  // Re-mint a bearer token from the stored password (POST /auth/token) after a
  // 401 / token loss, and store it. Returns the new token, or "" if we have no
  // stored password or it failed. Throttled (won't hammer /auth/token on a
  // permanently-bad credential). Blocking -- call off the UI thread.
  std::string ReAuth(uint64_t xuid);

  // True while a not-yet-acknowledged recovery code is queued for display.
  bool HasPendingRecoveryCode();
  // The queued recovery code (for the dialog); cleared by ClearPendingRecovery.
  std::string PendingRecoveryCode();
  void ClearPendingRecovery();

 private:
  NetplayAuth() { Load(); }
  ~NetplayAuth() = default;

  struct Entry {
    std::string token;
    std::string password;
    std::string recovery_code;
  };

  void Load();
  void Save();  // caller holds mutex_
  static std::string GenerateSecret(size_t bytes);
  void MaybeShowRecoveryDialog();

  std::mutex mutex_;
  std::map<uint64_t, Entry> entries_;
  std::string pending_recovery_;  // shown-once queue
  int64_t last_reauth_ms_ = 0;    // throttle for ReAuth
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_AUTH_H_
