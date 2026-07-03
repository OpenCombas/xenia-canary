/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <ranges>
#include <set>
#include <thread>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xsession.h"
#include "xenia/ui/imgui_host_notification.h"

DECLARE_bool(upnp);

DEFINE_bool(
    session_diag, true,
    "Netplay diagnostics: on each session create/join/leave/delete/migrate, log "
    "a census of every live XSession object (id, flags, host, members) and WARN "
    "when more than one peer-network session is live, or any player is a member "
    "of more than one live session -- the 'ended up in multiple sessions' "
    "failure mode (e.g. a search that couldn't match the real lobby and "
    "self-hosted a parallel one). Default on while we chase multi-session "
    "disconnects; low volume (fires only on session lifecycle events).",
    "Live");

DEFINE_int32(
    session_search_populate_timeout_ms, 5000,
    "Netplay: when a session search finds candidate lobbies that exist but "
    "haven't populated their properties yet (a squad-mate's just-created lobby "
    "mid-setup), wait up to this many milliseconds -- re-polling those "
    "candidates -- for them to become identifiable before failing the search. "
    "Failing forces the title to self-host a separate (parallel) lobby, so this "
    "lets a joiner converge on the real lobby instead. Only property-less "
    "candidates are waited on; an identified non-match is never joined. 0 "
    "disables the wait (legacy behavior).",
    "Live");

DEFINE_bool(
    session_search_log_criteria, true,
    "Netplay diagnostics: for the no-XLAST session-search filter, log every "
    "per-criterion comparison (each searched context/property vs the session's "
    "stored value, the operator used, and match/no-match) for each candidate "
    "session. This is the ground truth for mapping which operator each "
    "(proc_index, property/context id) uses. Default on (paired with "
    "session_diag) while we chase the session-filter / multi-session "
    "disconnects; verbose but only on searches. Set false for quiet logs.",
    "Live");

namespace xe {
namespace kernel {

XSession::XSession(KernelState* kernel_state)
    : XObject(kernel_state, Type::Session) {
  session_id_ = -1;
  owner_xuid_ = 0;
}

X_STATUS XSession::Initialize() {
  auto native_object = CreateNative(sizeof(X_KSESSION));
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }

  auto guest_object = reinterpret_cast<X_KSESSION*>(native_object);
  guest_object->handle = handle();
  // Based on what is in XAM it seems like size of this object is only 4 bytes.
  return X_STATUS_SUCCESS;
}

void XSession::LogSessionCensus(KernelState* kernel_state, const char* event) {
  if (!cvars::session_diag || !kernel_state) {
    return;
  }
  const auto sessions =
      kernel_state->object_table()->GetObjectsByType<XSession>(
          XObject::Type::Session);

  std::map<uint64_t, uint32_t> member_session_count;  // xuid -> # live sessions
  uint32_t live = 0, live_peer_net = 0;
  for (const auto& s : sessions) {
    if (!s || s->IsDeleted()) {
      continue;
    }
    ++live;
    if (s->local_details_.Flags.get() & SessionFlags::PEER_NETWORK) {
      ++live_peer_net;
    }
    std::set<uint64_t> in_this;  // dedupe a xuid within one session
    for (const auto& [xuid, m] : s->local_members_) {
      in_this.insert(xuid);
    }
    for (const auto& [xuid, m] : s->remote_members_) {
      in_this.insert(xuid);
    }
    for (uint64_t x : in_this) {
      member_session_count[x]++;
    }
  }

  XELOGI("[sessdiag] {}: {} live session(s), {} peer-network", event, live,
         live_peer_net);
  for (const auto& s : sessions) {
    if (!s) {
      continue;
    }
    std::string members;
    for (const auto& [xuid, m] : s->local_members_) {
      members += fmt::format("L:{:016X} ", xuid);
    }
    for (const auto& [xuid, m] : s->remote_members_) {
      members += fmt::format("R:{:016X} ", xuid);
    }
    XELOGI(
        "[sessdiag]   h={:08X} id={:016X} flags={:08X} [{}{}{}{}] owner={:016X} "
        "members={{ {}}}",
        s->handle(), s->GetSessionID(), s->local_details_.Flags.get(),
        s->IsCreated() ? "C" : "-", s->IsHost() ? "H" : "-",
        s->IsMigrted() ? "M" : "-", s->IsDeleted() ? "D" : "-",
        s->GetOwnerXUID(), members);
  }

  // The smoking guns: the local client (or any peer) present in >1 live session,
  // and/or more than one concurrent peer-network session.
  for (const auto& [xuid, n] : member_session_count) {
    if (n > 1) {
      XELOGW("[sessdiag] WARN: player {:016X} is a member of {} live sessions "
             "at '{}'",
             xuid, n, event);
    }
  }
  if (live_peer_net > 1) {
    XELOGW(
        "[sessdiag] WARN: {} concurrent peer-network sessions at '{}' -- client "
        "is in multiple sessions",
        live_peer_net, event);
  }
}

X_RESULT XSession::CreateSession(uint32_t user_index, uint8_t public_slots,
                                 uint8_t private_slots, uint32_t flags,
                                 uint32_t session_info_ptr,
                                 uint32_t nonce_ptr) {
  if (IsCreated()) {
    // Todo: Find proper code!
    return X_ERROR_FUNCTION_FAILED;
  }

  const xam::UserProfile* user_profile =
      kernel_state_->xam_state()->GetUserProfile(user_index);
  if (!user_profile) {
    return X_ERROR_FUNCTION_FAILED;
  }

  owner_xuid_ = user_profile->xuid();

  const auto user_tracker = kernel_state()->xam_state()->user_tracker();

  user_tracker->AddOwnedSession(user_profile->xuid(), handle());

  // Mutually exclusive
  if (flags & JOIN_VIA_PRESENCE_DISABLED &&
      flags & JOIN_VIA_PRESENCE_FRIENDS_ONLY) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // ARBITRATION requires stats and peer network flags to be set.
  if (flags & ARBITRATION && !(flags & STATS || flags & PEER_NETWORK)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Session type is ranked but ARBITRATION flag isn't set
  if (user_tracker->GetGameTypeValue(user_profile->xuid()) ==
          X_CONTEXT_GAME_TYPE_RANKED &&
      !(flags & ARBITRATION)) {
    return X_ONLINE_E_SESSION_REQUIRES_ARBITRATION;
  }

  // Set early so utility functions can check flags
  local_details_.Flags = flags;

  // Check we have privileges to create sessions.
  // XPRIVILEGE_MULTIPLAYER_SESSIONS = 254
  // XPRIVILEGE_SESSIONS = 189

  // 58410889
  // If a session requires online features but we're offline then we must fail.
  // e.g. Trying to create a SINGLEPLAYER_WITH_STATS session while not connected
  // to live.
  if (IsXboxLiveSession() && user_profile->signin_state() !=
                                 xam::X_USER_SIGNIN_STATE::SignedInToLive) {
    return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
  }

  XSESSION_INFO* SessionInfo_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(
          session_info_ptr);

  GenerateIdentityExchangeKey(&SessionInfo_ptr->keyExchangeKey);
  PrintSessionType((SessionFlags)flags);

  uint64_t* Nonce_ptr =
      kernel_state_->memory()->TranslateVirtual<uint64_t*>(nonce_ptr);

  local_details_.UserIndexHost = XUserIndexNone;

  // CSGO only uses STATS flag to create a session to POST stats pre round.
  // Minecraft and Portal 2 use flags HOST + STATS.
  //
  // Hexic creates a session with SINGLEPLAYER_WITH_STATS (without HOST bit)
  // with contexts
  //
  // Create presence sessions?
  // - Create when joining a session
  // - Explicitly create a presence session (Frogger & TRON without HOST bit)
  // Based on Presence flag set?

  // 584107FB expects offline session creation by specifying 0 (a session
  // without Xbox Live features) to succeed while offline for local multiplayer.
  //
  // 58410889 expects SINGLEPLAYER_WITH_STATS session creation failure while
  // offline.

  if (flags == STATS) {
    CreateStatsSession(SessionInfo_ptr, Nonce_ptr, user_index, public_slots,
                       private_slots, flags);
  } else if (HasSessionFlag((SessionFlags)flags, HOST) ||
             flags == SINGLEPLAYER_WITH_STATS || IsOfflineSession()) {
    CreateHostSession(SessionInfo_ptr, Nonce_ptr, user_index, public_slots,
                      private_slots, flags);
  } else {
    JoinExistingSession(SessionInfo_ptr);
  }

  local_details_.GameType =
      user_tracker->GetGameTypeValue(user_profile->xuid());
  local_details_.GameMode =
      user_tracker->GetGameModeValue(user_profile->xuid());
  local_details_.MaxPublicSlots = public_slots;
  local_details_.MaxPrivateSlots = private_slots;
  local_details_.AvailablePublicSlots = public_slots;
  local_details_.AvailablePrivateSlots = private_slots;
  local_details_.ActualMemberCount = 0;
  local_details_.ReturnedMemberCount = 0;
  local_details_.eState = XSESSION_STATE::LOBBY;
  local_details_.Nonce = *Nonce_ptr;
  local_details_.sessionInfo = *SessionInfo_ptr;
  local_details_.xnkidArbitration = XNKID{};
  local_details_.SessionMembers_ptr = 0;

  state_ |= STATE_FLAGS_CREATED;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::CreateHostSession(XSESSION_INFO* session_info,
                                     uint64_t* nonce_ptr, uint8_t user_index,
                                     uint8_t public_slots,
                                     uint8_t private_slots, uint32_t flags) {
  state_ |= STATE_FLAGS_HOST;

  local_details_.UserIndexHost = user_index;

  if (!cvars::upnp) {
    XELOGI("Hosting while UPnP is disabled!");
  }

  std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist(0, -1);
  *nonce_ptr = dist(rd);

  XGI_SESSION_CREATE session_data = {};
  session_data.user_index = user_index;
  session_data.num_slots_public = public_slots;
  session_data.num_slots_private = private_slots;
  session_data.flags = flags;

  const uint64_t systemlink_id =
      kernel_state()->GetXboxLiveAPI()->GetSystemlinkID();

  if (IsOfflineSession()) {
    XELOGI("Creating an offline session");

    // what session ID mask should be used here?
    session_id_ = GenerateSessionId(XNKID_SYSTEM_LINK);

  } else if (IsSystemlinkSession()) {
    XELOGI("Creating systemlink session");

    // If XNetRegisterKey did not register key then we must register it here
    if (systemlink_id) {
      session_id_ = systemlink_id;
    } else {
      session_id_ = GenerateSessionId(XNKID_SYSTEM_LINK);
      kernel_state()->GetXboxLiveAPI()->SetSystemlinkID(session_id_);
    }
  } else if (IsXboxLiveSession()) {
    XELOGI("Creating xbox live session");
    session_id_ = GenerateSessionId(XNKID_ONLINE);

    NotifySessionCreationWarning(user_index);

    kernel_state()->GetXboxLiveAPI()->XSessionCreate(session_id_,
                                                     &session_data);
  } else {
    assert_always();
  }

  XELOGI("Created session {:016X}", session_id_);

  IsValidXNKID(session_id_);

  Uint64toXNKID(session_id_, &session_info->sessionID);
  XLiveAPI::IpGetConsoleXnAddr(&session_info->hostAddress);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::CreateStatsSession(XSESSION_INFO* session_info,
                                      uint64_t* nonce_ptr, uint8_t user_index,
                                      uint8_t public_slots,
                                      uint8_t private_slots, uint32_t flags) {
  return CreateHostSession(session_info, nonce_ptr, user_index, public_slots,
                           private_slots, flags);
}

X_RESULT XSession::JoinExistingSession(XSESSION_INFO* session_info) {
  session_id_ = XNKIDtoUint64(&session_info->sessionID);
  XELOGI("Joining session {:016X}", session_id_);

  IsValidXNKID(session_id_);

  if (kernel::IsSystemlink(session_id_)) {
    XELOGI("Joining systemlink session");
    return X_ERROR_SUCCESS;
  } else if (kernel::IsOnlinePeer(session_id_)) {
    XELOGI("Joining xbox live session");
  } else {
    XELOGI("Joining unknown session type!");
    assert_always();
  }

  const auto session =
      kernel_state()->GetXboxLiveAPI()->XSessionGet(session_id_);

  // Begin XNetRegisterKey?

  if (!session.HostAddress().empty()) {
    XLiveAPI::GetXnAddrFromSessionObject(session, &session_info->hostAddress);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::DeleteSession(XGI_SESSION_STATE* state) {
  // Begin XNetUnregisterKey?

  if (IsDeleted()) {
    return X_ERROR_SUCCESS;
  }

  state_ |= STATE_FLAGS_DELETED;

  if (IsHost() && IsXboxLiveSession()) {
    kernel_state()->GetXboxLiveAPI()->DeleteSession(session_id_);
  }

  kernel_state()->xam_state()->user_tracker()->RemoveOwnedSession(
      GetOwnerXUID(), handle());

  session_id_ = -1;

  // Multiple sessions cause issues
  // XLiveAPI::systemlink_id = session_id_;

  local_details_.eState = XSESSION_STATE::DELETED;
  // local_details_.sessionInfo.sessionID = XNKID{};
  return X_ERROR_SUCCESS;
}

// A member can be added by either local or remote, typically local members
// are joined via local but are often joined via remote - they're equivalent.
//
// If there are no private slots available then the member will occupy a
// public slot instead.
//
// TODO(Adrian):
// Add player to recent player list.
// Joining a offline session uses which XUID offline or online (flags = 0)
// Return correct error codes
X_RESULT XSession::JoinSession(XGI_SESSION_MANAGE* data) {
  const bool join_local = data->xuid_array_ptr == 0;

  std::string join_type =
      join_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote";

  XELOGI("{}({:08X}, {}, {:08X}, {:08X}, {:08X})", join_type,
         data->obj_ptr.get(), data->array_count.get(),
         data->xuid_array_ptr.get(), data->indices_array_ptr.get(),
         data->private_slots_array_ptr.get());

  std::unordered_map<uint64_t, bool> members{};

  const auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  const auto indices_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->indices_array_ptr);

  const auto private_slots_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->private_slots_array_ptr);

  for (uint32_t i = 0; i < data->array_count; i++) {
    XSESSION_MEMBER* member = new XSESSION_MEMBER();

    if (join_local) {
      const uint32_t user_index = static_cast<uint32_t>(indices_array[i]);

      if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
        return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
      }

      const auto user_profile =
          kernel_state()->xam_state()->GetUserProfile(user_index);
      const xe::be<uint64_t> xuid_online = user_profile->GetLogonXUID();

      assert_true(IsValidXUID(xuid_online));

      if (local_members_.count(xuid_online) ||
          remote_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member->OnlineXUID = xuid_online;
      member->UserIndex = user_index;

      local_details_.ActualMemberCount = std::min<int32_t>(
          XUserMaxUserCount, local_details_.ActualMemberCount + 1);
    } else {
      const xe::be<uint64_t> xuid_online = xuid_array[i];
      uint8_t user_index =
          kernel_state()->xam_state()->GetUserIndexAssignedToProfileFromXUID(
              xuid_online);

      if (user_index == XUserIndexAny) {
        user_index = XUserIndexNone;
      }

      assert_true(IsValidXUID(xuid_online));

      if (remote_members_.count(xuid_online) ||
          local_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member->OnlineXUID = xuid_online;
      member->UserIndex = user_index;

      const bool is_local_member =
          kernel_state()->xam_state()->IsUserSignedIn(xuid_online);

      if (is_local_member) {
        local_details_.ActualMemberCount = std::min<int32_t>(
            XUserMaxUserCount, local_details_.ActualMemberCount + 1);
      }
    }

    const bool is_private = private_slots_array[i];

    if (is_private && local_details_.AvailablePrivateSlots > 0) {
      member->SetPrivate();

      local_details_.AvailablePrivateSlots =
          std::max<int32_t>(0, local_details_.AvailablePrivateSlots - 1);
    } else {
      local_details_.AvailablePublicSlots =
          std::max<int32_t>(0, local_details_.AvailablePublicSlots - 1);
    }

    XELOGI("XUID: {:016X} - Occupying {} slot", member->OnlineXUID.get(),
           member->IsPrivate() ? "private" : "public");

    members[member->OnlineXUID] = member->IsPrivate();

    if (join_local) {
      local_members_.emplace(member->OnlineXUID, *member);
    } else {
      remote_members_.emplace(member->OnlineXUID, *member);
    }
  }

  local_details_.ReturnedMemberCount = GetMembersCount();

  if (!members.empty() && IsHost() && IsXboxLiveSession()) {
    kernel_state()->GetXboxLiveAPI()->SessionJoinRemote(session_id_, members);
  } else if (!members.empty() && !IsOfflineSession()) {
    // To improve XNetInAddrToXnAddr stability each members session id
    // must match host. This is a workaround and should be fixed properly.
    //
    // 545107D1 will fail to join sessions if session id doesn't match.

    const auto keys = std::views::keys(members);
    std::set<uint64_t> xuids{keys.begin(), keys.end()};

    kernel_state()->GetXboxLiveAPI()->SessionPreJoin(session_id_, xuids);
  }

  // XamUserAddRecentPlayer -> XPresenceSubscribe

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::LeaveSession(XGI_SESSION_MANAGE* data) {
  const bool leave_local = data->xuid_array_ptr == 0;

  std::string leave_type =
      leave_local ? "XGISessionLeaveLocal" : "XGISessionLeaveRemote";

  XELOGI("{}({:08X}, {}, {:08X}, {:08X})", leave_type, data->obj_ptr.get(),
         data->array_count.get(), data->xuid_array_ptr.get(),
         data->indices_array_ptr.get());

  // Server already knows slots types from joining so we only need to send
  // xuids.
  std::vector<xe::be<uint64_t>> xuids{};

  auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  auto indices_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->indices_array_ptr);

  bool is_arbitrated = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), ARBITRATION);

  const auto profile_manager = kernel_state()->xam_state()->profile_manager();

  for (uint32_t i = 0; i < data->array_count; i++) {
    XSESSION_MEMBER* member = new XSESSION_MEMBER();

    if (leave_local) {
      const uint32_t user_index = static_cast<uint32_t>(indices_array[i]);

      if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
        return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
      }

      const auto user_profile =
          kernel_state()->xam_state()->GetUserProfile(user_index);
      const xe::be<uint64_t> xuid_online = user_profile->GetLogonXUID();

      assert_true(IsValidXUID(xuid_online));

      if (!local_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member = &local_members_[xuid_online];
    } else {
      const xe::be<uint64_t> xuid_online = xuid_array[i];

      assert_true(IsValidXUID(xuid_online));

      if (!remote_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member = &remote_members_[xuid_online];
    }

    if (member->IsPrivate()) {
      // Removing a private member but all members are removed
      assert_false(local_details_.AvailablePrivateSlots ==
                   local_details_.MaxPrivateSlots);

      local_details_.AvailablePrivateSlots =
          std::min<int32_t>(local_details_.MaxPrivateSlots,
                            local_details_.AvailablePrivateSlots + 1);
    } else {
      // Removing a public member but all members are removed
      assert_false(local_details_.AvailablePublicSlots ==
                   local_details_.MaxPublicSlots);

      local_details_.AvailablePublicSlots =
          std::min<int32_t>(local_details_.MaxPublicSlots,
                            local_details_.AvailablePublicSlots + 1);
    }

    // Keep arbitrated session members for stats reporting
    if (is_arbitrated) {
      member->SetZombie();
    }

    if (!member->IsZombie()) {
      bool removed = false;

      XELOGI("XUID: {:016X} - Leaving {} slot", member->OnlineXUID.get(),
             member->IsPrivate() ? "private" : "public");

      const xe::be<uint64_t> xuid_online = member->OnlineXUID;

      if (leave_local) {
        removed = local_members_.erase(xuid_online);
      } else {
        removed = remote_members_.erase(xuid_online);
      }

      assert_true(removed);

      if (removed) {
        xuids.push_back(xuid_online);

        const bool is_local_member =
            kernel_state()->xam_state()->IsUserSignedIn(xuid_online);

        if (is_local_member) {
          local_details_.ActualMemberCount =
              std::max<int32_t>(0, local_details_.ActualMemberCount - 1);
        }
      }
    }
  }

  local_details_.ReturnedMemberCount = GetMembersCount();

  if (!xuids.empty() && IsHost() && IsXboxLiveSession()) {
    kernel_state()->GetXboxLiveAPI()->SessionLeaveRemote(session_id_, xuids);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ModifySession(XGI_SESSION_MODIFY* data) {
  XELOGI("Modifying session {:016X}", session_id_);

  XGI_SESSION_MODIFY modify = *data;

  // Mutually exclusive
  if (data->flags & JOIN_VIA_PRESENCE_DISABLED &&
      data->flags & JOIN_VIA_PRESENCE_FRIENDS_ONLY) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint32_t modifiable = X_SESSION_CREATE_MODIFIERS_MASK | ARBITRATION;
  uint32_t modifiers = data->flags & modifiable;

  // If RegisterArbitration is already completed then arbitration flag cannot be
  // removed.
  bool is_arbitration_registered =
      static_cast<uint32_t>(local_details_.eState) &
      static_cast<uint32_t>(XSESSION_STATE::REGISTRATION);

  // If session is ranked then modify cannot remove arbitration flag, otherwise
  // standard/unranked sessions can modify this flag before RegisterArbitration.
  if (!(modifiers & ARBITRATION) &&
      (!local_details_.GameType || is_arbitration_registered)) {
    modifiers |= ARBITRATION;
  }

  local_details_.Flags &= ~modifiable;
  local_details_.Flags |= modifiers;

  modify.flags = local_details_.Flags;

  PrintSessionType(static_cast<SessionFlags>(local_details_.Flags.get()));

  const uint32_t num_private_slots = std::max<int32_t>(
      0, local_details_.MaxPrivateSlots - local_details_.AvailablePrivateSlots);

  const uint32_t num_public_slots = std::max<int32_t>(
      0, local_details_.MaxPublicSlots - local_details_.AvailablePublicSlots);

  data->maxPrivateSlots = std::max<int32_t>(0, data->maxPrivateSlots);
  data->maxPublicSlots = std::max<int32_t>(0, data->maxPublicSlots);

  local_details_.MaxPrivateSlots = data->maxPrivateSlots;
  local_details_.MaxPublicSlots = data->maxPublicSlots;

  local_details_.AvailablePrivateSlots =
      std::max<int32_t>(0, local_details_.MaxPrivateSlots - num_private_slots);
  local_details_.AvailablePublicSlots =
      std::max<int32_t>(0, local_details_.MaxPublicSlots - num_public_slots);

  PrintSessionDetails();

  if (IsHost() && IsXboxLiveSession()) {
    kernel_state()->GetXboxLiveAPI()->SessionModify(session_id_, &modify);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionDetails(XGI_SESSION_DETAILS* data) {
  // 4E4D085C checks ReturnedMemberCount when creating a session

  XSESSION_LOCAL_DETAILS* local_details_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(
          data->session_details_ptr);

  std::memcpy(local_details_ptr, &local_details_,
              sizeof(XSESSION_LOCAL_DETAILS));

  const uint32_t buffer_size =
      *kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->details_buffer_size);

  const uint32_t members_count =
      (buffer_size - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER);

  XSESSION_MEMBER* members_ptr =
      reinterpret_cast<XSESSION_MEMBER*>(local_details_ptr + 1);

  local_details_ptr->SessionMembers_ptr =
      kernel_state()->memory()->HostToGuestVirtual(
          std::to_address(members_ptr));

  std::vector<XSESSION_MEMBER> all_members = {};

  std::ranges::transform(local_members_, std::back_inserter(all_members),
                         &std::pair<const uint64_t, XSESSION_MEMBER>::second);

  std::ranges::transform(remote_members_, std::back_inserter(all_members),
                         &std::pair<const uint64_t, XSESSION_MEMBER>::second);

  const auto members = all_members | std::views::take(members_count);

  for (uint32_t i = 0; const auto& member : members) {
    members_ptr[i] = member;
    i++;
  }

  assert_false(all_members.size() > members_count);

  PrintSessionDetails();

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::MigrateHost(XGI_SESSION_MIGRATE* data) {
  auto SessionInfo_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(
          data->session_info_ptr);

  const auto upnp = kernel_state()->emulator()->GetUPnP();

  if (upnp && !upnp->IsActive()) {
    XELOGI("Migrating without UPnP");
    // return X_E_FAIL;
  }

  const auto result =
      kernel_state()->GetXboxLiveAPI()->XSessionMigration(session_id_, data);

  if (!result->SessionID_UInt()) {
    XELOGI("Session Migration Failed");

    // Returning X_E_FAIL will cause 5454082B to restart
    return X_E_FAIL;
  }

  if (data->user_index == XUserIndexNone) {
    XELOGI("Session migration we are not host.");
  } else {
    XELOGI("Session migration we are new host.");
    state_ |= STATE_FLAGS_HOST;
  }

  memset(SessionInfo_ptr, 0, sizeof(XSESSION_INFO));

  Uint64toXNKID(result->SessionID_UInt(), &SessionInfo_ptr->sessionID);
  XLiveAPI::IpGetConsoleXnAddr(&SessionInfo_ptr->hostAddress);
  GenerateIdentityExchangeKey(&SessionInfo_ptr->keyExchangeKey);

  // Update session id to migrated session id
  session_id_ = result->SessionID_UInt();

  state_ |= STATE_FLAGS_MIGRATED;

  local_details_.UserIndexHost = data->user_index;
  local_details_.sessionInfo = *SessionInfo_ptr;
  local_details_.xnkidArbitration = local_details_.sessionInfo.sessionID;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::RegisterArbitration(XGI_SESSION_ARBITRATION* data) {
  XSESSION_REGISTRATION_RESULTS* results_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_REGISTRATION_RESULTS*>(
          data->results_ptr);

  const auto result =
      kernel_state()->GetXboxLiveAPI()->XSessionArbitration(session_id_);

  const uint32_t registrants_ptr =
      kernel_state_->memory()->SystemHeapAlloc(static_cast<uint32_t>(
          sizeof(XSESSION_REGISTRANT) * result->Machines().size()));

  results_ptr->registrants_count =
      static_cast<uint32_t>(result->Machines().size());
  results_ptr->registrants_ptr = registrants_ptr;

  XSESSION_REGISTRANT* registrants =
      kernel_state_->memory()->TranslateVirtual<XSESSION_REGISTRANT*>(
          registrants_ptr);

  for (uint8_t i = 0; i < result->Machines().size(); i++) {
    registrants[i].trustworthiness = 1;

    registrants[i].machine_id = result->Machines()[i].machine_id;
    registrants[i].num_users = result->Machines()[i].player_count;

    const uint32_t users_ptr = kernel_state_->memory()->SystemHeapAlloc(
        sizeof(uint64_t) * registrants[i].num_users);

    uint64_t* users_xuid_ptr =
        kernel_state_->memory()->TranslateVirtual<uint64_t*>(users_ptr);

    for (uint8_t j = 0; j < registrants[i].num_users; j++) {
      users_xuid_ptr[j] = result->Machines()[i].xuids[j];
    }

    registrants[i].users_ptr = users_ptr;
  }

  Uint64toXNKID(session_id_, &local_details_.xnkidArbitration);

  local_details_.eState = XSESSION_STATE::REGISTRATION;

  // Assert?
  // local_details_.Nonce = data->session_nonce;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ModifySkill(XGI_SESSION_MODIFYSKILL* data) {
  const auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  const bool is_matchmaking_session = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), MATCHMAKING);

  if (!is_matchmaking_session) {
    return X_ONLINE_E_SESSION_INVALID_FLAGS;
  }

  const uint32_t game_mode = local_details_.GameMode;
  const uint32_t game_type = local_details_.GameType;
  const uint32_t skill_view_id =
      xam::GetSkillLeaderboardId(game_type, game_mode);

  X_USER_STATS_SPEC spec = {};

  spec.view_id = skill_view_id;
  spec.num_column_ids = 2;
  spec.column_ids[0] = X_STATS_COLUMN_SKILL_MU;
  spec.column_ids[1] = X_STATS_COLUMN_SKILL_SIGMA;

  // XUserReadStats(0, data->array_count, data->xuid_array_ptr, 1, spec,
  //                results_size, results_ptr, nullptr);

  // TODO: Calculate aggregate skill from skill leaderboard results

  for (uint32_t i = 0; i < data->array_count; i++) {
    const uint64_t xuid = xuid_array[i];

    XELOGI("ModifySkill XUID: {:016X}", xuid);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::WriteStats(XGI_STATS_WRITE* data) {
  if (!HasSessionFlag(static_cast<SessionFlags>((uint32_t)local_details_.Flags),
                      STATS)) {
    XELOGW("Session does not support stats.");
    return X_ERROR_FUNCTION_FAILED;
  }

  if (local_details_.eState != XSESSION_STATE::INGAME) {
    XELOGW("Writing stats outside of gameplay.");
    return X_ERROR_FUNCTION_FAILED;
  }

  if (!data->num_views) {
    XELOGW("No leaderboard stats to write.");
    return X_ERROR_SUCCESS;
  }

  const uint64_t xuid = data->xuid;

  if (!data->xuid) {
    // TODO: How does TrueSkill system overrides work?
    //
    // XPROPERTY_SESSION_SKILL_DRAW_PROBABILITY
    // XPROPERTY_SESSION_SKILL_BETA
    // XPROPERTY_SESSION_SKILL_TAU

    XELOGI("{}: TrueSkill System Overrides", __func__);
    assert_always();
    return X_ERROR_SUCCESS;
  }

  const bool is_arbitrated_session = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), ARBITRATION);

  const XSESSION_VIEW_PROPERTIES* views_properties_ptr =
      kernel_state()->memory()->TranslateVirtual<XSESSION_VIEW_PROPERTIES*>(
          data->views_ptr);

  assert_false(data->num_views > X_STATS_MAX_VIEWS);

  const uint32_t view_properties_count =
      std::min<uint32_t>(data->num_views, X_STATS_MAX_VIEWS);

  const std::vector<XSESSION_VIEW_PROPERTIES> views_properties(
      views_properties_ptr, views_properties_ptr + view_properties_count);

  for (const auto& view : views_properties) {
    const uint32_t view_id = view.view_id;

    const auto spa_stats_view =
        emulator()->game_info_database()->GetStatsView(view_id);

    // TrueSkill leaderboards are not defined in SPA?
    // 41560834 includes invalid leaderboards?
    if (!IsTrueSkillViewID(view_id) && !spa_stats_view.has_value()) {
      XELOGI("{} invalid leaderboard view id {:08X}", __func__, view_id);
      return X_ONLINE_E_STAT_INVALID_TITLE_OR_LEADERBOARD;
    }

    if (spa_stats_view.has_value()) {
      const auto& stats_view = spa_stats_view.value();

      // If session attempts to write arbitrated leaderboards from a
      // non-arbitrated session, then XSessionWriteStats will fail.
      if (stats_view.view.arbitrated && !is_arbitrated_session) {
        XELOGI("{} requires session arbitration", __func__);
        return X_ONLINE_E_SESSION_REQUIRES_ARBITRATION;
      }
    }

    // Only assume a leaderboard is arbitrated if it's skilled and not found
    // in SPA.
    if (IsTrueSkillViewID(view_id) && !is_arbitrated_session &&
        !spa_stats_view.has_value()) {
      XELOGI("{} requires session arbitration", __func__);
      return X_ONLINE_E_SESSION_REQUIRES_ARBITRATION;
    }

    const xam::XUSER_PROPERTY* properties_ptr =
        kernel_state()->memory()->TranslateVirtual<xam::XUSER_PROPERTY*>(
            view.properties_ptr);

    assert_false(view.properties_count > X_STATS_MAX_PROPERTIES_IN_VIEW);

    const uint32_t properties_count = std::min<uint32_t>(
        view.properties_count, X_STATS_MAX_PROPERTIES_IN_VIEW);

    const std::vector<xam::XUSER_PROPERTY> properties(
        properties_ptr, properties_ptr + properties_count);

    for (const auto& property_info : properties) {
      const uint32_t property_id = property_info.property_id;
      const uint8_t* data_ptr =
          reinterpret_cast<const uint8_t*>(&property_info.data.data);

      const uint32_t property_data_size =
          xam::UserData::get_valid_data_size(property_id, 0);

      const xam::Property property = xam::Property(
          property_id, property_data_size, const_cast<uint8_t*>(data_ptr));

      cached_stats_properties_[xuid][view_id][property_id] = property;
    }
  }

  return X_ERROR_SUCCESS;
}

// Flush cached leaderboard stats to the backend
X_RESULT XSession::FlushStats() {
  if (!HasSessionFlag(static_cast<SessionFlags>((uint32_t)local_details_.Flags),
                      STATS)) {
    XELOGW("Session does not support stats.");
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }

  if (local_details_.eState != XSESSION_STATE::INGAME) {
    XELOGW("Flushing stats outside of gameplay.");
    return X_ONLINE_E_SESSION_WRONG_STATE;
  }

  const bool is_arbitrated = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), ARBITRATION);

  view_properties_unordered_map stats_to_flush = {};

  for (const auto& [xuid, views] : cached_stats_properties_) {
    for (const auto& [view_id, view] : views) {
      const auto spa_stats_view =
          emulator()->game_info_database()->GetStatsView(view_id);

      if (is_arbitrated) {
        const bool is_view_arbitrated = spa_stats_view.has_value() &&
                                        spa_stats_view.value().view.arbitrated;

        // Flush only non-arbitrated and non-skilled leaderboards
        if (!IsTrueSkillViewID(view_id) && !is_view_arbitrated) {
          stats_to_flush[xuid][view_id] = view;
        }
      } else {
        // Flush only non-skilled leaderboards
        if (!IsTrueSkillViewID(view_id)) {
          stats_to_flush[xuid][view_id] = view;
        }
      }
    }
  }

  // Previously host-only, which silently dropped every non-host player's
  // career stats: each console writes its OWN player's stat views, but only the
  // host ever uploaded, so the boards ended up ranking hosting frequency rather
  // than skill. Measured -- a pilot's 14-view write was logged locally and no
  // matching row ever reached the backend.
  //
  // Every console now flushes, but only for players it is authoritative for.
  // A non-host is authoritative for its own signed-in profiles and nothing
  // else; the host additionally reports rows it holds on behalf of the session
  // (it writes a one-view arbitration result for every player). Scoping this
  // way is what makes the change safe for the Sum-aggregated columns: those
  // accumulate per-match deltas server-side, so two consoles reporting the same
  // player's row would double-count it. Under this rule exactly one console
  // ever submits a given (xuid, view).
  const bool is_host = IsHost();

  if (!is_host) {
    auto* xam = kernel_state()->xam_state();
    for (auto it = stats_to_flush.begin(); it != stats_to_flush.end();) {
      if (xam && xam->IsUserSignedIn(it->first)) {
        ++it;
      } else {
        it = stats_to_flush.erase(it);
      }
    }
  }

  if (!stats_to_flush.empty()) {
    const bool flushed = kernel_state()->GetXboxLiveAPI()->SessionFlushStats(
        session_id_, stats_to_flush);

    XELOGI("{}: flushed {} player(s) as {} ({} view-set(s) cached)", __func__,
           stats_to_flush.size(), is_host ? "host" : "peer",
           cached_stats_properties_.size());

    // If flush is successful then remove cached stats
    if (flushed) {
      for (const auto& [xuid, views] : stats_to_flush) {
        cached_stats_properties_.erase(xuid);
      }
    }
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::StartSession(XGI_SESSION_STATE* state) {
  local_details_.eState = XSESSION_STATE::INGAME;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::EndSession(XGI_SESSION_STATE* state) {
  local_details_.eState = XSESSION_STATE::REPORTING;

  const bool stats_enabled = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), STATS);

  // The host will report TrueSkill statistics for all players in a session?

  // Post the remaining cached stats. Unlike FlushStats this posts everything
  // still held, INCLUDING the skilled/arbitrated views FlushStats withholds --
  // which is the only path by which the reserved arbitration view reaches the
  // backend at all.
  //
  // Scoping here is per (xuid, view), NOT per xuid, because the two kinds of
  // view have opposite requirements:
  //
  //  - ARBITRATED / skilled views are a CONSENSUS mechanism. Every member
  //    reports the result for every player; the service compares those reports
  //    and only commits the delta if they agree, discarding it otherwise as
  //    evidence of a tampered session. Suppressing a peer's report of another
  //    player therefore destroys the very redundancy arbitration exists for --
  //    so these are submitted by everyone, for everyone.
  //  - ORDINARY stat views have a single authoritative reporter. A player's own
  //    console owns its rows; duplicates from a second console would make the
  //    Sum-aggregated columns accumulate one per-match delta twice.
  //
  // So a non-host drops only NON-arbitrated views belonging to other players.
  //
  // NOTE this makes duplicate arbitrated rows expected by design. The backend
  // must reconcile them (compare across members, apply once on agreement, drop
  // on mismatch) rather than accumulate them -- accumulating would double-count
  // exactly the columns arbitration is meant to protect.
  if (stats_enabled) {
    view_properties_unordered_map stats_to_flush = cached_stats_properties_;

    if (!IsHost()) {
      auto* xam = kernel_state()->xam_state();
      auto* db = emulator()->game_info_database();

      for (auto xuid_it = stats_to_flush.begin();
           xuid_it != stats_to_flush.end();) {
        if (xam && xam->IsUserSignedIn(xuid_it->first)) {
          ++xuid_it;  // our own player: everything stays
          continue;
        }

        auto& views = xuid_it->second;
        for (auto view_it = views.begin(); view_it != views.end();) {
          const uint32_t view_id = view_it->first;
          const auto spa_view = db ? db->GetStatsView(view_id) : std::nullopt;
          const bool arbitrated =
              IsTrueSkillViewID(view_id) ||
              (spa_view.has_value() && spa_view.value().view.arbitrated);

          view_it = arbitrated ? std::next(view_it) : views.erase(view_it);
        }

        xuid_it = views.empty() ? stats_to_flush.erase(xuid_it)
                                : std::next(xuid_it);
      }
    }

    if (!stats_to_flush.empty()) {
      const bool flushed = kernel_state()->GetXboxLiveAPI()->SessionFlushStats(
          session_id_, stats_to_flush);

      if (flushed) {
        for (const auto& [xuid, views] : stats_to_flush) {
          cached_stats_properties_.erase(xuid);
        }
      }
    }
  }

  return X_ERROR_SUCCESS;
}

namespace {

const char* UserDataTypeName(xam::X_USER_DATA_TYPE type) {
  switch (type) {
    case xam::X_USER_DATA_TYPE::CONTEXT:
      return "CONTEXT";
    case xam::X_USER_DATA_TYPE::INT32:
      return "INT32";
    case xam::X_USER_DATA_TYPE::INT64:
      return "INT64";
    case xam::X_USER_DATA_TYPE::DOUBLE:
      return "DOUBLE";
    case xam::X_USER_DATA_TYPE::WSTRING:
      return "WSTRING";
    case xam::X_USER_DATA_TYPE::FLOAT:
      return "FLOAT";
    case xam::X_USER_DATA_TYPE::BINARY:
      return "BINARY";
    case xam::X_USER_DATA_TYPE::DATETIME:
      return "DATETIME";
    default:
      return "UNSET";
  }
}

// Log the guest's search query verbatim. Titles without an XLAST (e.g. the ones
// this fork targets) give us no server-side query schema, but the fields the
// game marshals -- proc_index, each context, and each typed property -- are all
// visible here. Capturing them is the ground truth for reconstructing what a
// title's matchmaking actually asks for (the comparison operators themselves
// live server-side and are not present in the title binary).
void LogSessionSearchQuery(const XGI_SESSION_SEARCH* search_data,
                           uint32_t num_users,
                           const xam::XUSER_CONTEXT* contexts,
                           const xam::XUSER_PROPERTY* properties) {
  XELOGI(
      "XSessionSearch query: proc_index={} num_ctx={} num_props={} "
      "num_results={} num_users={}",
      static_cast<uint32_t>(search_data->proc_index),
      static_cast<uint32_t>(search_data->num_ctx),
      static_cast<uint32_t>(search_data->num_props),
      static_cast<uint32_t>(search_data->num_results), num_users);
  for (uint32_t i = 0; i < search_data->num_ctx; i++) {
    XELOGI("  search ctx[{}]: id=0x{:08X} value=0x{:08X}", i,
           static_cast<uint32_t>(contexts[i].context_id),
           static_cast<uint32_t>(contexts[i].value));
  }
  for (uint32_t i = 0; i < search_data->num_props; i++) {
    const uint32_t id = static_cast<uint32_t>(properties[i].property_id);
    XELOGI(
        "  search prop[{}]: id=0x{:08X} type={} (type-from-id={}) "
        "value=0x{:016X}",
        i, id, UserDataTypeName(properties[i].data.type),
        UserDataTypeName(xam::UserData::get_type(id)),
        static_cast<uint64_t>(properties[i].data.data.filetime));
  }
}

// Comparison operator for a search property. The classic Xbox matchmaking model
// (and every context, which is an enum) is equality; a handful of properties in
// more complex queries (e.g. rank) use an ordering. The real operators are
// server-side in the missing XLAST, so this is a per-property-id override table:
// default equality, with confirmed exceptions added as they're identified from
// the search-query logs above.
enum class SearchCompareOp {
  kEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
};

SearchCompareOp GetPropertySearchOp(uint32_t property_id) {
  switch (property_id) {
    // Squad member-count / capacity, compared as an upper bound: a session
    // matches when its stored value is <= the searched value. The searcher
    // passes the squad size cap (e.g. 20), so any squad with room is returned,
    // while a narrower cap correctly excludes larger squads. This is the only
    // operator consistent with real Chromehounds squad searches (a session
    // storing 1 must be returned for a search of 20); everything else is an
    // enum/boolean and stays equality.
    case 0x1000003C:
      return SearchCompareOp::kLessEqual;
    // Max-rank ceiling: the session stores the highest rank it will admit, and
    // the game auto-sends the searcher's own rank here. A searcher qualifies only
    // when the session's ceiling is at or above them, so the match is session >=
    // search (kGreaterEqual). Confirmed by a live capture (build/logs/
    // session_searches): a Colonel(18) searcher was wrongly KEPT for a
    // recruit(1)-max squad under the old lte (1<=18); >= (1>=18) correctly
    // excludes it while still keeping a colonel(18)-max squad (18>=18).
    case 0x10000042:
      return SearchCompareOp::kGreaterEqual;
    default:
      return SearchCompareOp::kEqual;
  }
}

// Human-readable operator symbol for the per-criterion filter diagnostics.
const char* SearchOpName(SearchCompareOp op) {
  switch (op) {
    case SearchCompareOp::kEqual:
      return "==";
    case SearchCompareOp::kNotEqual:
      return "!=";
    case SearchCompareOp::kLess:
      return "<";
    case SearchCompareOp::kLessEqual:
      return "<=";
    case SearchCompareOp::kGreater:
      return ">";
    case SearchCompareOp::kGreaterEqual:
      return ">=";
    default:
      return "?";
  }
}

// A search value of zero is the title's "unset / don't care" wildcard for a
// property: the parameter was left blank, so it must not constrain the results.
// Confirmed against real sessions -- a Chromehounds squad browse passes property
// 0x20000001=0 while live squad sessions store a non-zero per-session value
// there, and they must still be returned. Properties only; contexts are enums
// where 0 is a legitimate value (GAME_MODE 0 == Free Battle).
bool IsWildcardSearchProperty(const xam::X_USER_DATA& search) {
  using T = xam::X_USER_DATA_TYPE;
  switch (search.type) {
    case T::WSTRING:
    case T::BINARY:
      return static_cast<uint32_t>(search.data.binary.size) == 0;
    case T::INT64:
    case T::DOUBLE:
    case T::DATETIME:
      return static_cast<uint64_t>(search.data.filetime) == 0;
    default:  // INT32 / FLOAT / CONTEXT-as-property (4-byte)
      return static_cast<uint32_t>(search.data.u32) == 0;
  }
}

// The title advertises a session's matchmaking "hopper" id in one of two paired
// INT64 slots (0x20000001 primary / 0x20000002 secondary); a given session
// populates exactly one and leaves the other zero. The conquest browse issues
// one search per slot, both with a wildcard (0) value, so treating wildcard as
// an unconditional skip returns the same session for BOTH searches and it shows
// up twice in the browse list (confirmed: joining either row routes to the same
// session). For these two ids a wildcard search instead requires the session to
// actually populate that slot (stored value non-zero), so each session matches
// only its own slot. Specific-value searches on these ids (the squad rendezvous
// path) are unaffected -- they never reach the wildcard branch.
bool IsPairedHopperSlot(uint32_t property_id) {
  return property_id == 0x20000001 || property_id == 0x20000002;
}

// Type-aware comparison of a stored session property against a search property,
// evaluated as (stored <op> search). Fixes the previous u32-only assumption:
// INT64/DOUBLE/DATETIME compare all 8 bytes with the correct signedness, FLOAT
// as float, WSTRING/BINARY by their bytes. Contexts are handled separately (they
// are genuinely always u32).
bool CompareStoredToSearch(const xam::Property& stored_prop,
                           const xam::X_USER_DATA& search, SearchCompareOp op,
                           Memory* memory) {
  using T = xam::X_USER_DATA_TYPE;
  const xam::X_USER_DATA& stored = *stored_prop.get_data();
  if (stored.type != search.type) {
    return false;
  }

  // Variable-length data: only equality/inequality is meaningful.
  if (search.type == T::WSTRING || search.type == T::BINARY) {
    const auto stored_bytes = stored_prop.get_extended_data();
    const uint32_t size = static_cast<uint32_t>(search.data.binary.size);
    bool equal = size == stored_bytes.size();
    if (equal && size) {
      const uint32_t ptr = static_cast<uint32_t>(search.data.binary.ptr);
      equal = ptr && std::memcmp(memory->TranslateVirtual<const uint8_t*>(ptr),
                                 stored_bytes.data(), size) == 0;
    }
    return op == SearchCompareOp::kNotEqual ? !equal : equal;
  }

  // Fixed-size numeric types: compute a three-way ordering by the real type.
  int cmp;
  switch (search.type) {
    case T::INT64:
    case T::DATETIME: {
      const int64_t a = stored.data.s64, b = search.data.s64;
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      break;
    }
    case T::DOUBLE: {
      const double a = stored.data.f64, b = search.data.f64;
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      break;
    }
    case T::FLOAT: {
      const float a = stored.data.f32, b = search.data.f32;
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      break;
    }
    case T::INT32: {
      const int32_t a = stored.data.s32, b = search.data.s32;
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      break;
    }
    default: {  // CONTEXT / unknown: unsigned 32-bit
      const uint32_t a = stored.data.u32, b = search.data.u32;
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
      break;
    }
  }

  switch (op) {
    case SearchCompareOp::kEqual:
      return cmp == 0;
    case SearchCompareOp::kNotEqual:
      return cmp != 0;
    case SearchCompareOp::kLess:
      return cmp < 0;
    case SearchCompareOp::kLessEqual:
      return cmp <= 0;
    case SearchCompareOp::kGreater:
      return cmp > 0;
    case SearchCompareOp::kGreaterEqual:
      return cmp >= 0;
  }
  return cmp == 0;
}

}  // namespace

X_RESULT XSession::GetSessions(KernelState* kernel_state,
                               XGI_SESSION_SEARCH* search_data,
                               uint32_t num_users) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size =
        sizeof(XSESSION_SEARCHRESULT) * search_data->num_results;
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  xam::XUSER_CONTEXT* search_contexts_ptr =
      kernel_state->memory()->TranslateVirtual<xam::XUSER_CONTEXT*>(
          search_data->ctx_ptr);

  xam::XUSER_PROPERTY* search_properties_ptr =
      kernel_state->memory()->TranslateVirtual<xam::XUSER_PROPERTY*>(
          search_data->props_ptr);

  LogSessionSearchQuery(search_data, num_users, search_contexts_ptr,
                        search_properties_ptr);

  const auto sessions =
      kernel_state->GetXboxLiveAPI()->SessionSearch(search_data, num_users);

  std::vector<uint32_t> filtered_indices;

  // Free-battle vs squad-lobby login guard. Within a proc_index 0 browse the
  // context 0x800B discriminates the two: 0 == free battle, non-zero == the
  // searcher's nation for a squad-lobby browse. A FREE-BATTLE search legitimately
  // leaves the hopper rendezvous property 0x20000001 at its wildcard (0), and
  // that must still match (any free-battle session) -- so it falls through to the
  // normal filter below. A SQUAD-LOBBY search (0x800B != 0) that also leaves
  // 0x20000001 wildcard is under-constrained: it matches any same-mode/same-nation
  // session, so on login the title joins an unrelated squad's lobby. Reject only
  // that case so the title self-hosts; a genuine squad join carries a specific
  // (non-zero) 0x20000001 and is unaffected. Scoped to the no-XLast hand-filtered
  // path; XLast titles use their own matchmaking query.
  bool reject_squad_wildcard = false;
  if (static_cast<uint32_t>(search_data->proc_index) == 0) {
    bool wildcard_hopper = false;
    for (uint32_t p = 0; p < search_data->num_props; p++) {
      const xam::XUSER_PROPERTY& search_prop = search_properties_ptr[p];
      if (static_cast<uint32_t>(search_prop.property_id) == 0x20000001 &&
          IsWildcardSearchProperty(search_prop.data)) {
        wildcard_hopper = true;
        break;
      }
    }
    bool squad_search = false;  // 0x800B != 0 -> squad-lobby browse
    for (uint32_t c = 0; c < search_data->num_ctx; c++) {
      const xam::XUSER_CONTEXT& search_ctx = search_contexts_ptr[c];
      if (static_cast<uint32_t>(search_ctx.context_id) == 0x0000800B &&
          static_cast<uint32_t>(search_ctx.value) != 0) {
        squad_search = true;
        break;
      }
    }
    reject_squad_wildcard = wildcard_hopper && squad_search;
  }

  // NOTE: these filters used to be gated on !HasXLast(), on the assumption that
  // an XLAST-driven path would take over. It would not: the HasXLast() branch
  // below reads the query's parameters/filters/returns and only LOGS them --
  // nothing evaluates them, and the filter operator (`op`) is not parsed at all
  // (there is no GetFiltersOp). So supplying XLAST data did not swap filtering,
  // it REMOVED it, keeping every session the backend returned. The failure
  // signature is *more* results, which reads as success while reintroducing the
  // cross-squad and parallel-lobby matches these filters exist to prevent.
  //
  // The hand filters therefore run unconditionally and stay authoritative.
  // Moving matchmaking to server-supplied XLAST is still the goal, but it needs
  // the XLAST path to actually evaluate filters first; until then this is a
  // strict improvement on upstream, which has the same latent gap.
  if (reject_squad_wildcard) {
    XELOGI(
        "Session search: squad-lobby browse (0x800B!=0) with wildcard "
        "0x20000001=0 -> returning 0 results (avoids joining an unrelated squad "
        "lobby on login; free-battle 0x800B=0 searches are unaffected)");
    for (uint32_t s = 0; s < sessions.size(); s++) {
      filtered_indices.push_back(s);
    }
  } else if (search_data->num_ctx > 0 || search_data->num_props > 0) {
    std::vector<uint32_t> pending_indices;

    // Classify one session against the search criteria: 2 = keep, 1 = reject
    // (identifiable but not a match), 0 = pending -- returned with no properties
    // yet, so there is nothing to filter on but its id (a freshly-created lobby
    // still populating). Reused by the wait-for-identity re-poll below.
    auto classify_session =
        [&](uint64_t session_id,
            const std::vector<xam::Property>& all_properties) -> int {
      if (all_properties.empty()) {
        return 0;  // undecidable yet -> hold for re-poll rather than reject
      }

      // Active-session exclusion (property 0x1000004C, INT32): 1 == joinable,
      // 2 == a mission is under way. The title never SEARCHES on this, so the
      // criteria loop below cannot reject on it -- that loop only tests what the
      // searcher asked for, and an in-progress session otherwise satisfies the
      // browse and shows up as joinable. This is a session-STATE rejection, not
      // a search-criteria one: any returned session storing 2 is dropped
      // regardless of the query. It applies uniformly -- a Neroimus war session
      // stays at 2 for the rest of its life (terminal), while a free-battle
      // session flips back to 1 when its match ends and reappears on its own, so
      // no per-mode gating is needed.
      for (const auto& property : all_properties) {
        if (!property.IsContext() &&
            property.GetPropertyId().value == 0x1000004C &&
            property.get_data()->data.u32 == 2) {
          XELOGI(
              "  filter: session {:016X} excluded -- active (0x1000004C=2)",
              session_id);
          return 1;  // identifiable but not joinable
        }
      }

      // Iterate the search criteria (not the session's properties) so an
      // unmatched criterion excludes the session while a wildcard criterion is
      // simply satisfied. A session passes only if every searched context and
      // property is satisfied by one of the session's stored values.
      uint32_t ctx_matched = 0;
      for (uint32_t c = 0; c < search_data->num_ctx; c++) {
        const xam::XUSER_CONTEXT& search_ctx = search_contexts_ptr[c];
        // Nation (context 0x2): only the OPPOSING-nations conquest browse
        // (proc_index 2) wants sessions whose nation DIFFERS from the searcher's
        // -- it matches 0x2 on inequality. The SQUAD browse (proc_index 1)
        // carries the same 0x2 context but wants the SAME nation (equality), so
        // the inversion must be gated on proc_index, NOT on 0x2's mere presence
        // -- gating on presence wrongly excluded same-nation squads (0x2=1 vs a
        // nation-1 session -> 1!=1 -> dropped). The own-nation conquest browse
        // omits 0x2 entirely. All other contexts stay equality.
        const bool opposing_nation =
            search_ctx.context_id == 0x00000002 &&
            static_cast<uint32_t>(search_data->proc_index) == 2;
        bool ctx_found = false, ctx_ok = false;
        uint32_t stored_ctx = 0;
        for (const auto& property : all_properties) {
          if (property.IsContext() &&
              property.GetPropertyId().value == search_ctx.context_id) {
            ctx_found = true;
            stored_ctx = property.get_data()->data.u32;
            const bool value_equal = stored_ctx == search_ctx.value;
            ctx_ok = opposing_nation ? !value_equal : value_equal;
            break;
          }
        }
        if (ctx_ok) {
          ctx_matched++;
        }
        if (cvars::session_search_log_criteria) {
          XELOGI(
              "  filter[{:016X}] ctx id=0x{:08X} op={} search=0x{:08X} "
              "stored=0x{:08X} present={} -> {}",
              session_id, static_cast<uint32_t>(search_ctx.context_id),
              opposing_nation ? "!=" : "==",
              static_cast<uint32_t>(search_ctx.value), stored_ctx,
              ctx_found ? 1 : 0, ctx_ok ? "match" : "NO");
        }
      }

      uint32_t props_matched = 0;
      for (uint32_t p = 0; p < search_data->num_props; p++) {
        const xam::XUSER_PROPERTY& search_prop = search_properties_ptr[p];
        const uint32_t pid = static_cast<uint32_t>(search_prop.property_id);
        bool prop_found = false, prop_ok = false;
        uint64_t stored_raw = 0;
        const char* op_name = SearchOpName(GetPropertySearchOp(pid));
        // The paired-hopper dedup (a wildcard slot matches only a session that
        // POPULATES it) is specific to the proc_index=2 conquest browse, which
        // issues a separate wildcard search per slot and would otherwise return
        // a one-slot session twice. Other queries (e.g. proc_index=0 free battle)
        // reuse these ids differently -- there a wildcard(0) slot is a plain
        // don't-care. Gating on proc_index==2 (mirrors the 0x2 nation inversion)
        // fixes free-battle sessions that live in the OTHER slot being wrongly
        // excluded (build/logs/session_searches action 4: target stored slot2=1,
        // slot1=0; the slot1 wildcard demanded slot1 populated -> false negative).
        const bool paired_hopper =
            IsPairedHopperSlot(pid) &&
            static_cast<uint32_t>(search_data->proc_index) == 2;
        if (IsWildcardSearchProperty(search_prop.data)) {
          if (paired_hopper) {
            // Wildcard on a hopper slot: match only if the session populates
            // this slot (non-zero), so a session living in one slot isn't
            // returned by both slot searches (which duplicates it).
            op_name = "wild-hopper";
            for (const auto& property : all_properties) {
              if (!property.IsContext() &&
                  property.GetPropertyId().value == pid) {
                prop_found = true;
                stored_raw =
                    static_cast<uint64_t>(property.get_data()->data.filetime);
                if (!IsWildcardSearchProperty(*property.get_data())) {
                  prop_ok = true;
                }
                break;
              }
            }
          } else {
            op_name = "wildcard";
            prop_ok = true;
          }
        } else {
          for (const auto& property : all_properties) {
            if (!property.IsContext() &&
                property.GetPropertyId().value == pid) {
              prop_found = true;
              stored_raw =
                  static_cast<uint64_t>(property.get_data()->data.filetime);
              if (CompareStoredToSearch(property, search_prop.data,
                                        GetPropertySearchOp(pid),
                                        kernel_state->memory())) {
                prop_ok = true;
              }
              break;
            }
          }
        }
        if (prop_ok) {
          props_matched++;
        }
        if (cvars::session_search_log_criteria) {
          XELOGI(
              "  filter[{:016X}] prop id=0x{:08X} op={} search=0x{:016X} "
              "stored=0x{:016X} present={} -> {}",
              session_id, pid, op_name,
              static_cast<uint64_t>(search_prop.data.data.filetime), stored_raw,
              prop_found ? 1 : 0, prop_ok ? "match" : "NO");
        }
      }

      const bool excluded = props_matched < search_data->num_props ||
                            ctx_matched < search_data->num_ctx;
      // TEMP diagnostic: per-session filter verdict, so over-exclusion can be
      // pinpointed without guessing which session/criterion failed. nation and
      // the hopper-slot flags are the discriminators for the opposing browse.
      uint32_t dbg_nation = 0;
      bool dbg_slot1 = false, dbg_slot2 = false;
      for (const auto& property : all_properties) {
        if (property.IsContext()) {
          if (property.GetPropertyId().value == 0x00000002)
            dbg_nation = property.get_data()->data.u32;
          continue;
        }
        const uint32_t pid = property.GetPropertyId().value;
        if (pid == 0x20000001)
          dbg_slot1 = !IsWildcardSearchProperty(*property.get_data());
        else if (pid == 0x20000002)
          dbg_slot2 = !IsWildcardSearchProperty(*property.get_data());
      }
      XELOGI(
          "  filter: session {:016X} nation={} slot1set={} slot2set={} "
          "ctx {}/{} props {}/{} -> {}",
          session_id, dbg_nation, dbg_slot1, dbg_slot2, ctx_matched,
          static_cast<uint32_t>(search_data->num_ctx), props_matched,
          static_cast<uint32_t>(search_data->num_props),
          excluded ? "EXCLUDED" : "kept");
      return excluded ? 1 : 2;
    };

    // First pass: classify every returned session.
    for (uint32_t s = 0; s < sessions.size(); s++) {
      const auto all_properties =
          kernel_state->GetXboxLiveAPI()->SessionPropertiesGet(
              sessions.at(s)->SessionID_UInt());
      switch (
          classify_session(sessions.at(s)->SessionID_UInt(), all_properties)) {
        case 1:
          filtered_indices.push_back(s);
          break;
        case 0:
          pending_indices.push_back(s);
          break;
        default:
          break;  // 2 -> kept (left out of filtered_indices)
      }
    }

    // Wait-for-identity: if nothing was kept but some candidates were returned
    // without properties yet (a squad-mate's lobby that was just created and
    // hasn't populated its context), those are the only joinable hope -- failing
    // now forces the title to self-host a parallel lobby. Re-poll just those
    // candidates until one populates and matches (join it), all populate as
    // non-matches (reject), or the budget expires (reject -> self-host). A
    // property-less session is never joined; it is only ever joined after it
    // becomes identifiable and passes the same filter, so the squad-identity
    // invariant holds (we never join the wrong squad's half-formed lobby).
    const size_t kept_count =
        sessions.size() - filtered_indices.size() - pending_indices.size();
    if (kept_count == 0 && !pending_indices.empty() &&
        cvars::session_search_populate_timeout_ms > 0) {
      XELOGI(
          "Session search: 0 kept, {} candidate(s) not yet populated; waiting "
          "up to {}ms for identity before self-hosting",
          pending_indices.size(),
          cvars::session_search_populate_timeout_ms);
      const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds(
              int64_t(cvars::session_search_populate_timeout_ms));
      constexpr auto kPollInterval = std::chrono::milliseconds(750);
      bool matched = false;
      while (!pending_indices.empty() && !matched &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPollInterval);
        std::vector<uint32_t> still_pending;
        for (uint32_t s : pending_indices) {
          const auto props =
              kernel_state->GetXboxLiveAPI()->SessionPropertiesGet(
                  sessions.at(s)->SessionID_UInt());
          switch (classify_session(sessions.at(s)->SessionID_UInt(), props)) {
            case 2:
              matched = true;  // now identifiable and a match -> keep
              break;
            case 1:
              filtered_indices.push_back(s);  // populated, but not our match
              break;
            default:
              still_pending.push_back(s);  // still empty -> keep waiting
              break;
          }
        }
        pending_indices.swap(still_pending);
      }
      // Anything that never populated in time (or was left pending once another
      // candidate matched) is rejected: we only ever return identified matches.
      for (uint32_t s : pending_indices) {
        filtered_indices.push_back(s);
      }
      XELOGI(matched
                 ? "Session search: a candidate populated and matched -> "
                   "joining (avoided a parallel lobby)"
                 : "Session search: no candidate matched within budget -> "
                   "search fails, title will self-host");
    } else {
      // Not waiting (something kept, nothing pending, or disabled): a still-empty
      // candidate is treated as a non-match, exactly as before.
      for (uint32_t s : pending_indices) {
        filtered_indices.push_back(s);
      }
    }
  }
  const uint32_t session_count = std::min<int32_t>(
      search_data->num_results,
      static_cast<uint32_t>(sessions.size() - filtered_indices.size()));

  XELOGI("Session search: {} returned, {} kept, {} excluded by filter.",
         session_count + filtered_indices.size(), session_count,
         filtered_indices.size());
  const uint32_t session_ids =
      kernel_state->memory()->SystemHeapAlloc(session_count * sizeof(XNKID));

  XNKID* session_ids_ptr =
      kernel_state->memory()->TranslateVirtual<XNKID*>(session_ids);

  // Compact the kept sessions into the session_count-sized id array. The filter
  // check indexes the FULL session list by `i`, but the write must use a
  // SEPARATE output index: otherwise, when an excluded session precedes a kept
  // one, the kept id is written at an out-of-bounds original index while the
  // in-bounds slot stays zero -- so the title later joins session id 0
  // (XSessionGet(0000...) -> not found -> self-host into a separate lobby).
  uint32_t out_index = 0;
  for (uint32_t i = 0; i < sessions.size() && out_index < session_count; i++) {
    if (std::find(filtered_indices.begin(), filtered_indices.end(), i) !=
        filtered_indices.end()) {
      continue;  // excluded by filter
    }
    XNKID id = {};
    Uint64toXNKID(sessions.at(i)->SessionID_UInt(), &id);
    session_ids_ptr[out_index] = id;
    out_index++;
  }
  GetSessionByIDs(kernel_state, session_ids_ptr, session_count,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  SEARCH_RESULTS* search_results_ptr =
      kernel_state->memory()->TranslateVirtual<SEARCH_RESULTS*>(
          search_data->search_results_ptr);

  util::XLastMatchmakingQuery* matchmaking_query = nullptr;

  // DIAGNOSTIC ONLY -- this branch describes the title's declared matchmaking
  // query; it does NOT filter. The parameters/filters/returns read below are
  // logged and discarded, and the comparison operator is never parsed, so
  // nothing here can reproduce the filtering above. Wiring it up means
  // implementing evaluation (and an `op` reader) first -- until then, do not
  // gate the real filters on this.
  if (kernel_state->emulator()->game_info_database()->HasXLast()) {
    matchmaking_query = kernel_state->emulator()
                            ->game_info_database()
                            ->GetXLast()
                            ->GetMatchmakingQuery();

    const auto paramaters =
        matchmaking_query->GetParameters(search_data->proc_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(search_data->proc_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(search_data->proc_index);
    const auto returns = matchmaking_query->GetReturns(search_data->proc_index);

    XELOGI("Matchmaking Query Name: {}",
           matchmaking_query->GetName(search_data->proc_index));

    for (uint32_t i = 0; i < search_data->num_ctx; i++) {
      xam::XUSER_CONTEXT& context = search_contexts_ptr[i];

      auto user =
          kernel_state->xam_state()->GetUserProfile(search_data->user_index);

      std::u16string context_desc =
          kernel_state->xam_state()->user_tracker()->GetContextDescription(
              user->xuid(), context.context_id);

      XELOGD(xe::to_utf8(context_desc));
    }

    for (uint32_t i = 0; i < search_data->num_props; i++) {
      xam::XUSER_PROPERTY& property = search_properties_ptr[i];

      std::u16string property_desc =
          kernel_state->xam_state()->user_tracker()->GetPropertyDescription(
              property.property_id);

      XELOGD(xe::to_utf8(property_desc));
    }
  }

  // Fill each result row's contexts/properties keyed on the session id that
  // GetSessionByIDs actually wrote to that row -- NOT sessions.at(i), which
  // indexes the FULL pre-filter list. GetSessionByIDs compacts the kept
  // sessions (dropping filtered + host-less entries), so results_ptr[i] holds
  // the i-th SURVIVOR's id. Pairing it with sessions.at(i)'s properties
  // mismatched the two whenever an excluded session preceded a kept one: the
  // browser then showed a kept session's id wearing an excluded session's
  // properties (e.g. the wrong squad name, until join re-fetched by real id).
  const uint32_t filled_count = search_results_ptr->header.search_results_count;
  for (uint32_t i = 0; i < filled_count; i++) {
    const uint64_t result_session_id =
        XNKIDtoUint64(&search_results_ptr->results_ptr[i].info.sessionID);

    std::vector<xam::Property> contexts = {};
    std::vector<xam::Property> properties = {};

    const auto all_properties =
        kernel_state->GetXboxLiveAPI()->SessionPropertiesGet(result_session_id);

    for (const auto& property : all_properties) {
      if (property.IsContext()) {
        contexts.push_back(property);
      } else {
        properties.push_back(property);
      }
    }

    FillSessionContext(kernel_state->memory(), search_data->proc_index,
                       matchmaking_query, contexts, search_data->num_ctx,
                       search_contexts_ptr,
                       &search_results_ptr->results_ptr[i]);
    FillSessionProperties(kernel_state->memory(), search_data->proc_index,
                          matchmaking_query, properties, search_data->num_props,
                          search_properties_ptr,
                          &search_results_ptr->results_ptr[i]);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetWeightedSessions(
    KernelState* kernel_state,
    XGI_SESSION_SEARCH_WEIGHTED* weighted_search_data, uint32_t num_users) {
  XGI_SESSION_SEARCH search_data = {};

  search_data.proc_index = weighted_search_data->proc_index;
  search_data.user_index = weighted_search_data->user_index;
  search_data.num_results = weighted_search_data->num_results;
  search_data.num_props = weighted_search_data->num_props;
  search_data.num_ctx = weighted_search_data->num_ctx;
  search_data.props_ptr =
      weighted_search_data->non_weighted_search_properties_ptr;
  search_data.ctx_ptr = weighted_search_data->non_weighted_search_contexts_ptr;
  search_data.results_buffer_size = weighted_search_data->results_buffer_size;
  search_data.search_results_ptr = weighted_search_data->search_results_ptr;

  // TODO:
  // weighted_search_data->weighted_search_contexts_ptr;
  // weighted_search_data->weighted_search_properties_ptr;
  // weighted_search_data->num_weighted_properties;
  // weighted_search_data->num_weighted_contexts;

  return GetSessions(kernel_state, &search_data, num_users);
}

X_RESULT XSession::GetSessionByID(KernelState* kernel_state,
                                  XGI_SESSION_SEARCH_BYID* search_data) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size = sizeof(XSESSION_SEARCHRESULT);
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  if (search_data->user_index < 0 ||
      search_data->user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint32_t session_count = 1;

  GetSessionByIDs(kernel_state, &search_data->session_id, session_count,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionByIDs(KernelState* kernel_state,
                                   XGI_SESSION_SEARCH_BYIDS* search_data) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size =
        search_data->num_session_ids * sizeof(XSESSION_SEARCHRESULT);
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  if (search_data->user_index < 0 ||
      search_data->user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (search_data->num_session_ids <= 0 &&
      search_data->num_session_ids > 0x64) {
    return X_ERROR_INVALID_PARAMETER;
  }

  XNKID* session_ids_ptr = kernel_state->memory()->TranslateVirtual<XNKID*>(
      search_data->session_ids_ptr);

  GetSessionByIDs(kernel_state, session_ids_ptr, search_data->num_session_ids,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionByIDs(KernelState* kernel_state,
                                   XNKID* session_ids_ptr,
                                   uint32_t num_session_ids,
                                   uint32_t search_results_ptr,
                                   uint32_t results_buffer_size) {
  SEARCH_RESULTS* search_results =
      kernel_state->memory()->TranslateVirtual<SEARCH_RESULTS*>(
          search_results_ptr);

  std::memset(search_results, 0, results_buffer_size);

  search_results->results_ptr =
      reinterpret_cast<XSESSION_SEARCHRESULT*>(search_results + 1);

  const uint32_t session_search_result_ptr =
      kernel_state->memory()->HostToGuestVirtual(
          std::to_address(search_results->results_ptr));

  uint32_t result_index = 0;

  for (uint32_t i = 0; i < num_session_ids; i++) {
    const xe::be<uint64_t> session_id = XNKIDtoUint64(&session_ids_ptr[i]);

    IsValidXNKID(session_id);

    const auto session =
        kernel_state->GetXboxLiveAPI()->XSessionGet(session_id);

    // Skip setting contexts and properties in such case.
    if (!session.HostAddress().empty()) {
      FillSessionSearchResult(session,
                              &search_results->results_ptr[result_index]);

      result_index++;
    }
  }

  search_results->header.search_results_count = result_index;
  search_results->header.search_results_ptr = session_search_result_ptr;

  return X_ERROR_SUCCESS;
}

void XSession::FillSessionSearchResult(const SessionObjectJSON session,
                                       XSESSION_SEARCHRESULT* result) {
  result->filled_private_slots = session.FilledPrivateSlotsCount();
  result->filled_public_slots = session.FilledPublicSlotsCount();
  result->open_private_slots = session.OpenPrivateSlotsCount();
  result->open_public_slots = session.OpenPublicSlotsCount();

  Uint64toXNKID(session.SessionID_UInt(), &result->info.sessionID);

  XLiveAPI::GetXnAddrFromSessionObject(session, &result->info.hostAddress);

  GenerateIdentityExchangeKey(&result->info.keyExchangeKey);
}

void XSession::FillSessionContext(
    Memory* memory, uint32_t matchmaking_index,
    util::XLastMatchmakingQuery* matchmaking_query,
    std::vector<xam::Property> contexts, uint32_t filter_contexts_count,
    xam::XUSER_CONTEXT* filter_contexts_ptr, XSESSION_SEARCHRESULT* result) {
  if (matchmaking_query) {
    const auto paramaters = matchmaking_query->GetParameters(matchmaking_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(matchmaking_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(matchmaking_index);
    const auto returns = matchmaking_query->GetReturns(matchmaking_index);
  }

  result->contexts_count = static_cast<uint32_t>(contexts.size());

  const uint32_t context_ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(
      sizeof(xam::XUSER_CONTEXT) * result->contexts_count));

  xam::XUSER_CONTEXT* contexts_to_get =
      memory->TranslateVirtual<xam::XUSER_CONTEXT*>(context_ptr);

  for (uint32_t i = 0; i < filter_contexts_count; i++) {
    xam::XUSER_CONTEXT& filter_context = filter_contexts_ptr[i];
  }

  uint32_t i = 0;
  for (const auto& context : contexts) {
    contexts_to_get[i].context_id = context.GetPropertyId().value;
    contexts_to_get[i].value = context.get_data()->data.u32;
    i++;
  }

  result->contexts_ptr = context_ptr;
}

void XSession::FillSessionProperties(
    Memory* memory, uint32_t matchmaking_index,
    util::XLastMatchmakingQuery* matchmaking_query,
    std::vector<xam::Property> properties, uint32_t filter_properties_count,
    xam::XUSER_PROPERTY* filter_properties_ptr, XSESSION_SEARCHRESULT* result) {
  if (matchmaking_query) {
    const auto paramaters = matchmaking_query->GetParameters(matchmaking_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(matchmaking_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(matchmaking_index);
    const auto returns = matchmaking_query->GetReturns(matchmaking_index);
  }

  result->properties_count = static_cast<uint32_t>(properties.size());

  const uint32_t properties_ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(
      sizeof(xam::XUSER_PROPERTY) * result->properties_count));

  xam::XUSER_PROPERTY* properties_to_set =
      memory->TranslateVirtual<xam::XUSER_PROPERTY*>(properties_ptr);

  for (uint32_t i = 0; i < filter_properties_count; i++) {
    xam::XUSER_PROPERTY& filter_property = filter_properties_ptr[i];
  }

  uint32_t i = 0;
  for (const auto& property : properties) {
    if (property.requires_additional_data()) {
      properties_to_set[i].data.data.unicode.ptr = memory->SystemHeapAlloc(
          static_cast<uint32_t>(property.get_data()->data.unicode.size));
    }

    property.WriteToGuest(&properties_to_set[i]);
    i++;
  }

  result->properties_ptr = properties_ptr;
}

bool XSession::IsPresenceEnabled() const {
  return local_details_.Flags & PRESENCE;
}

bool XSession::IsJoinViaPresenceEnabled() const {
  return !(local_details_.Flags & JOIN_VIA_PRESENCE_DISABLED);
}

bool XSession::IsJoinViaPresenceFriendsOnly() const {
  return local_details_.Flags & JOIN_VIA_PRESENCE_FRIENDS_ONLY;
}

bool XSession::IsJoinInProgressEnabled() const {
  return !(local_details_.Flags & JOIN_IN_PROGRESS_DISABLED);
}

bool XSession::IsInvitesEnabled() const {
  return !(local_details_.Flags & INVITES_DISABLED);
}

bool XSession::IsSessionStarted() const {
  return static_cast<uint32_t>(local_details_.eState) &
         static_cast<uint32_t>(XSESSION_STATE::INGAME);
}

bool XSession::IsSessionEnded() const {
  return static_cast<uint32_t>(local_details_.eState) &
         static_cast<uint32_t>(XSESSION_STATE::REPORTING);
}

void XSession::NotifySessionCreationWarning(uint32_t user_index) const {
  const xam::UserProfile* user_profile =
      kernel_state_->xam_state()->GetUserProfile(user_index);

  if (user_profile) {
    const uint8_t user_slot =
        kernel_state_->xam_state()
            ->profile_manager()
            ->GetUserIndexAssignedToProfile(user_profile->xuid());

    if (user_slot > 0) {
      const auto profile =
          kernel_state()->xam_state()->profile_manager()->GetProfile(
              uint8_t(0));

      std::string warning_msg =
          "Currently only profile slot 1 is allowed to host Xbox Live "
          "sessions!";

      if (profile) {
        warning_msg = fmt::format(
            "Currently only profile slot 1 (signed in as {}) is allowed to "
            "host Xbox Live sessions!",
            profile->name());
      }

      new xe::ui::HostNotificationWindow(
          kernel_state()->emulator()->imgui_drawer(),
          "Session Creation Warning!", warning_msg, 0, 9);
    }
  }
}

void XSession::PrintSessionDetails() {
  XELOGI(
      "\n***************** PrintSessionDetails *****************\n"
      "UserIndex: {}\n"
      "GameType: {}\n"
      "GameMode: {}\n"
      "eState: {}\n"
      "Nonce: {:016X}\n"
      "Flags: {:08X}\n"
      "MaxPrivateSlots: {}\n"
      "MaxPublicSlots: {}\n"
      "AvailablePrivateSlots: {}\n"
      "AvailablePublicSlots: {}\n"
      "ActualMemberCount: {}\n"
      "ReturnedMemberCount: {}\n"
      "xnkidArbitration: {:016X}\n",
      local_details_.UserIndexHost.get(),
      local_details_.GameType ? "Standard" : "Ranked",
      local_details_.GameMode.get(),
      static_cast<uint32_t>(local_details_.eState), local_details_.Nonce.get(),
      local_details_.Flags.get(), local_details_.MaxPrivateSlots.get(),
      local_details_.MaxPublicSlots.get(),
      local_details_.AvailablePrivateSlots.get(),
      local_details_.AvailablePublicSlots.get(),
      local_details_.ActualMemberCount.get(),
      local_details_.ReturnedMemberCount.get(),
      local_details_.xnkidArbitration.as_uintBE64());

  uint32_t index = 0;

  for (const auto& [xuid, mamber] : local_members_) {
    XELOGI(
        "\n***************** LOCAL MEMBER {} *****************\n"
        "Online XUID: {:016X}\n"
        "UserIndex: {}\n"
        "Flags: {:08X}\n"
        "IsPrivate: {}\n",
        index++, mamber.OnlineXUID.get(), mamber.UserIndex.get(),
        mamber.Flags.get(), mamber.IsPrivate() ? "True" : "False");
  }

  index = 0;

  for (const auto& [xuid, mamber] : remote_members_) {
    XELOGI(
        "\n***************** REMOTE MEMBER {} *****************\n"
        "Online XUID: {:016X}\n"
        "UserIndex: {}\n"
        "Flags: {:08X}\n"
        "IsPrivate: {}\n",
        index++, mamber.OnlineXUID.get(), mamber.UserIndex.get(),
        mamber.Flags.get(), mamber.IsPrivate() ? "True" : "False");
  }
}

void XSession::PrintSessionType(SessionFlags flags) {
  std::string session_description = "";

  if (!flags) {
    XELOGI("Session Flags Empty!");
    return;
  }

  const std::map<SessionFlags, std::string> basic = {
      {HOST, "Host"},
      {PRESENCE, "Presence"},
      {STATS, "Stats"},
      {MATCHMAKING, "Matchmaking"},
      {ARBITRATION, "Arbitration"},
      {PEER_NETWORK, "Peer Network"},
      {SOCIAL_MATCHMAKING_ALLOWED, "Social Matchmaking"},
      {INVITES_DISABLED, "No invites"},
      {JOIN_VIA_PRESENCE_DISABLED, "Presence Join Disabled"},
      {JOIN_IN_PROGRESS_DISABLED, "In-Progress Join Disabled"},
      {JOIN_VIA_PRESENCE_FRIENDS_ONLY, "Friends Only"},
      {UNKNOWN, "Unknown Flag 0x1000"}};

  const std::map<SessionFlags, std::string> extended = {
      {SINGLEPLAYER_WITH_STATS, "Singleplayer with Stats"},
      {LIVE_MULTIPLAYER_STANDARD, "LIVE: Multiplayer"},
      {LIVE_MULTIPLAYER_RANKED, "LIVE: Multiplayer Ranked"},
      {GROUP_LOBBY, "Group Lobby"},
      {GROUP_GAME, "Group Game"}};

  for (const auto& entry : basic) {
    if (HasSessionFlag(flags, entry.first)) {
      session_description.append(entry.second + ", ");
    }
  }

  XELOGI("Session Description: {}", session_description);
  session_description.clear();

  for (const auto& entry : extended) {
    if (HasSessionFlag(flags, entry.first)) {
      session_description.append(entry.second + ", ");
    }
  }

  XELOGI("Session Extended Description: {}", session_description);
}

}  // namespace kernel
}  // namespace xe
