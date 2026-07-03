/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_PARTY_MANAGER_H_
#define XENIA_KERNEL_PARTY_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace kernel {

// One member of a party, as returned by the WebServices /party roster.
struct PartyMember {
  uint64_t xuid = 0;
  std::string gamertag;
  uint64_t peer_key = 0;  // 48-bit console MAC (from the roster's 12-hex string)
};

// A pending party invite delivered to us via GET /party/poll.
struct PartyInvite {
  std::string party_id;
  uint64_t from_xuid = 0;
  std::string from_gamertag;
};

// Client-side party / voice-call manager. Mirrors Xbox Live party chat on top of
// the friends system, decoupled from game sessions: you invite friends to a
// "call" and full-mesh Opus voice is established with everyone in it.
//
// The WebServices party service is poll-native (no push), so this runs a poll
// loop at the DASHBOARD level (title-agnostic) that both delivers invites/roster
// and heartbeats our liveness. On each roster change it maps every other
// member's console MAC into the GNS registry and hands the VoiceChannel the
// member INA set (MapPeer + SetPartyRoster); voice transport is 100% client-side.
//
// Singleton. Started from XLiveAPI::StartGNS (so it's up whenever we're online).
// HTTP + JSON go through XLiveAPI's static Get/Post helpers.
class PartyManager {
 public:
  static PartyManager* Get();

  // Start / stop the background poll loop. Idempotent.
  void Start();
  void Stop();

  // --- UI actions. Fire the corresponding POST on a detached worker (so the UI
  // thread never blocks on the network); state reconciles on the next poll, and
  // Create/Join also apply their returned party immediately. Return false only
  // on an obvious local precondition failure (e.g. not signed in). ---
  bool CreateParty();
  bool Invite(uint64_t target_xuid);
  bool Join(const std::string& party_id);
  bool Leave();
  bool Decline(const std::string& party_id);

  // --- Thread-safe snapshots for the UI. ---
  bool InParty() const;
  std::string GetPartyId() const;
  uint64_t GetOwnerXuid() const;
  std::vector<PartyMember> GetMembers() const;
  std::vector<PartyInvite> GetInvites() const;
  // True if target_xuid is already in our current party.
  bool IsMember(uint64_t target_xuid) const;

 private:
  PartyManager() = default;
  ~PartyManager();

  void PollThreadMain();
  void PollOnce(uint64_t me);  // one GET /party/poll + apply

  // Worker bodies for the UI actions (run on a detached thread each).
  void DoCreate(uint64_t me);
  void DoInvite(std::string party_id, uint64_t me, uint64_t target);
  void DoJoin(std::string party_id, uint64_t me);
  void DoLeave(std::string party_id, uint64_t me);
  void DoDecline(std::string party_id, uint64_t me);

  // Parse a poll body ({party, invites}) / a create-or-join body (a bare Party).
  void HandlePollBody(const char* body, size_t len);
  void HandlePartyBody(const char* body, size_t len);

  // Live-events WS handler (primary path): route snapshot / party.invite /
  // party.roster / party.dissolved messages to state. Registered with
  // LiveEventsClient in Start(); the poll loop backs off while the WS is up.
  void OnLiveEvent(const std::string& message_json);

  // Apply resolved party state: updates fields under the lock and drives voice.
  // party_present=false means "party":null (not in one).
  void ApplyParty(bool party_present, const std::string& party_id,
                  uint64_t owner_xuid, std::vector<PartyMember> members);
  // Map every non-self member's MAC into GNS and push the INA set to VoiceChannel;
  // starts voice on first populated roster, stops it when the party empties.
  void UpdateVoiceRoster();

  // Our signed-in online XUID, or 0 if not signed in / offline.
  static uint64_t LocalOnlineXuid();

  std::atomic<bool> running_{false};
  std::thread poll_thread_;

  mutable std::mutex mutex_;
  bool in_party_ = false;
  std::string party_id_;
  uint64_t owner_xuid_ = 0;
  std::vector<PartyMember> members_;
  std::vector<PartyInvite> invites_;
  bool voice_started_ = false;
  // Invite() with no current party auto-creates one, then invites these once it
  // exists. `creating_` de-dupes concurrent create requests.
  std::vector<uint64_t> pending_after_create_;
  bool creating_ = false;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_PARTY_MANAGER_H_
