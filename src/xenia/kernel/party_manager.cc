/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/party_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Match the kernel's JSON headers so the rapidjson std::string support macro
// isn't redefined (they set it before including rapidjson).
#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/gns_transport.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/live_events.h"
#include "xenia/kernel/util/shim_utils.h"  // kernel_state()
#include "xenia/kernel/voice_channel.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xnet.h"  // HTTP_STATUS_CODE
#include "third_party/imgui/imgui.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"

DEFINE_bool(
    party_invite_notify, true,
    "Netplay: pop an on-screen notification when a party invite arrives (in "
    "addition to it showing in the Friends party panel).",
    "Live");

namespace xe {
namespace kernel {

namespace {

// XUIDs cross the wire as 16-hex uppercase strings; the console MAC (peer_key)
// as 12-hex uppercase. Both round-trip through these.
std::string HexXuid(uint64_t xuid) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(xuid));
  return std::string(buf);
}

uint64_t ParseHexU64(const std::string& s) {
  if (s.empty()) {
    return 0;
  }
  return std::strtoull(s.c_str(), nullptr, 16);
}

// The signed-in user's online XUID, or 0 if not signed in / offline. Shared by
// PartyManager::LocalOnlineXuid() and the voice overlay (which can't reach the
// private static).
uint64_t OnlineXuidNow() {
  if (!kernel_state()) {
    return 0;
  }
  auto* xam = kernel_state()->xam_state();
  if (!xam) {
    return 0;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!xam->IsUserSignedIn(i)) {
      continue;
    }
    xam::UserProfile* p = xam->GetUserProfile(i);
    if (p) {
      const uint64_t x = p->GetOnlineXUID();
      if (x) {
        return x;
      }
    }
  }
  return 0;
}

// The XLiveAPI instance owns the curl/session plumbing (Get/Post). Null if the
// kernel/live layer isn't up yet -- callers bail.
XLiveAPI* Api() {
  return kernel_state() ? kernel_state()->GetXboxLiveAPI() : nullptr;
}

// The /party endpoints reply 200 (GET poll) or 201 Created (POST actions, the
// NestJS default). Accept either as success.
bool HttpOk(uint64_t code) {
  return code == HTTP_STATUS_CODE::HTTP_OK ||
         code == HTTP_STATUS_CODE::HTTP_CREATED;
}

// Pop a fire-and-forget host notification "toast" for an incoming invite. Safe
// to call from the WS worker thread: it hops to the UI thread and re-checks the
// emulator/window on the way (they can go away between capture and draw). The
// toast is informational only -- accept/decline lives in the Friends party panel.
void ShowInviteToast(const std::string& from_gamertag) {
  if (!cvars::party_invite_notify || !kernel_state()) {
    return;
  }
  Emulator* emulator = kernel_state()->emulator();
  if (!emulator || !emulator->display_window() || !emulator->imgui_drawer()) {
    return;
  }
  const std::string who = from_gamertag.empty() ? "A friend" : from_gamertag;
  emulator->display_window()->app_context().CallInUIThread([emulator, who]() {
    if (!emulator->imgui_drawer()) {
      return;
    }
    new xe::ui::HostNotificationWindow(emulator->imgui_drawer(), "Party Invite",
                                       who + " invited you to a party", 0);
  });
}

std::string GetStr(const rapidjson::Value& obj, const char* key) {
  if (obj.HasMember(key) && obj[key].IsString()) {
    return std::string(obj[key].GetString(), obj[key].GetStringLength());
  }
  return std::string();
}

// Parse a [{partyId,fromXuid,fromGamertag}] array into invites, skipping any
// entry without a partyId. Shared by the poll body and the WS snapshot.
std::vector<PartyInvite> ParseInviteArray(const rapidjson::Value& arr) {
  std::vector<PartyInvite> out;
  if (!arr.IsArray()) {
    return out;
  }
  for (const auto& v : arr.GetArray()) {
    if (!v.IsObject()) {
      continue;
    }
    PartyInvite pi;
    pi.party_id = GetStr(v, "partyId");
    pi.from_xuid = ParseHexU64(GetStr(v, "fromXuid"));
    pi.from_gamertag = GetStr(v, "fromGamertag");
    if (!pi.party_id.empty()) {
      out.push_back(std::move(pi));
    }
  }
  return out;
}

// Parse a Party JSON object: { partyId, ownerXuid, members:[{xuid,gamertag,
// peer_key}] }. Returns false if it doesn't look like a party.
bool ParsePartyObject(const rapidjson::Value& p, std::string& id_out,
                      uint64_t& owner_out, std::vector<PartyMember>& members_out) {
  if (!p.IsObject() || !p.HasMember("partyId") || !p["partyId"].IsString()) {
    return false;
  }
  id_out = GetStr(p, "partyId");
  owner_out = ParseHexU64(GetStr(p, "ownerXuid"));
  members_out.clear();
  if (p.HasMember("members") && p["members"].IsArray()) {
    for (const auto& m : p["members"].GetArray()) {
      if (!m.IsObject()) {
        continue;
      }
      PartyMember pm;
      pm.xuid = ParseHexU64(GetStr(m, "xuid"));
      pm.gamertag = GetStr(m, "gamertag");
      pm.peer_key = ParseHexU64(GetStr(m, "peer_key"));
      if (pm.xuid) {
        members_out.push_back(std::move(pm));
      }
    }
  }
  return true;
}

// --- Floating party-voice overlay ------------------------------------------
// A small always-on HUD (drawn over the game and the dashboard) listing the
// current party members with a live per-member "speaking" dot. Implemented as a
// persistent ImGuiDrawer dialog so the drawer keeps repainting (the dots need to
// animate) and Draw()s it every frame. It is created on the party-start
// transition and self-closes once the party ends, so at idle it costs nothing.
//
// Lifecycle note: everything here runs on the UI thread -- the dialog ctor/dtor
// (drawer-owned) and the create path (posted via CallInUIThread) -- so the
// alive flag needs no locking. A raw pointer is deliberately NOT held by
// PartyManager: the dialog owns its own lifetime (self-Close) and just flips the
// flag, which sidesteps every cross-thread / teardown dangling-pointer hazard.

// Peer output level above which a member is drawn as "speaking".
constexpr float kOverlaySpeakThreshold = 0.02f;
// True while a PartyVoiceOverlay instance is registered. UI-thread only.
std::atomic<bool> g_party_overlay_alive{false};

class PartyVoiceOverlay final : public ui::ImGuiDialog {
 public:
  explicit PartyVoiceOverlay(ui::ImGuiDrawer* drawer) : ui::ImGuiDialog(drawer) {
    g_party_overlay_alive.store(true);
  }
  ~PartyVoiceOverlay() override { g_party_overlay_alive.store(false); }

  void OnDraw(ImGuiIO& io) override {
    auto* pm = PartyManager::Get();
    if (!pm->InParty()) {
      Close();  // party ended -> tear down until the next party starts
      return;
    }
    const std::vector<PartyMember> members = pm->GetMembers();
    const uint64_t me = OnlineXuidNow();
    auto* voice = VoiceChannel::Get();

    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Party Voice", nullptr, flags)) {
      if (members.empty()) {
        ImGui::TextDisabled("(waiting for members)");
      }
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float line_h = ImGui::GetTextLineHeight();
      const float r = line_h * 0.30f;
      for (const auto& m : members) {
        const bool is_self = (m.xuid == me);
        bool speaking;
        if (is_self) {
          speaking = voice->IsSelfActive();
        } else {
          const uint32_t ina =
              GNSTransport::SyntheticInaFromPeerKey(m.peer_key);
          speaking = voice->GetPeerActivity(ina) > kOverlaySpeakThreshold;
        }
        const ImU32 col = speaking ? IM_COL32(60, 220, 90, 255)
                                   : IM_COL32(96, 96, 96, 255);
        // Speaking dot, vertically centred on the text line, then the name.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(pos.x + r, pos.y + line_h * 0.5f), r, col);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + r * 2.0f + 8.0f, pos.y));
        const char* name = m.gamertag.empty() ? "Player" : m.gamertag.c_str();
        if (is_self) {
          ImGui::Text("%s (you)", name);
        } else {
          ImGui::TextUnformatted(name);
        }
      }
    }
    ImGui::End();
  }
};

// Create the overlay if one isn't already up. Hops to the UI thread (dialog
// registration touches the drawer's vector). Safe to call redundantly.
void ShowVoiceOverlay() {
  if (g_party_overlay_alive.load() || !kernel_state()) {
    return;
  }
  Emulator* emulator = kernel_state()->emulator();
  if (!emulator || !emulator->display_window() || !emulator->imgui_drawer()) {
    return;
  }
  emulator->display_window()->app_context().CallInUIThread([emulator]() {
    if (g_party_overlay_alive.load() || !emulator->imgui_drawer()) {
      return;  // raced another create, or the drawer went away
    }
    new PartyVoiceOverlay(emulator->imgui_drawer());  // self-owned
  });
}

}  // namespace

PartyManager* PartyManager::Get() {
  static PartyManager instance;
  return &instance;
}

PartyManager::~PartyManager() { Stop(); }

void PartyManager::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  poll_thread_ = std::thread(&PartyManager::PollThreadMain, this);
  // Subscribe to the live-events WS (primary path). The poll loop backs off while
  // the WS is connected; registering is harmless if live_events is off.
  LiveEventsClient::Get()->AddHandler(
      [](const std::string& msg) { PartyManager::Get()->OnLiveEvent(msg); });
  XELOGI("[party] manager started (poll loop up)");
}

void PartyManager::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }
  // Drop out of any party locally: clear the voice roster + stop voice.
  ApplyParty(false, std::string(), 0, {});
  XELOGI("[party] manager stopped");
}

uint64_t PartyManager::LocalOnlineXuid() { return OnlineXuidNow(); }

void PartyManager::PollThreadMain() {
  xe::threading::set_name("Party Poll");
  using namespace std::chrono_literals;
  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t me = LocalOnlineXuid();
    // The live-events WS is the primary source when connected: it delivers state
    // AND is the liveness signal, so the poll (fallback) backs off to a slow
    // connectivity re-check. connected() is false when live_events is off/down,
    // in which case we poll normally.
    const bool ws_active = LiveEventsClient::Get()->connected();
    if (me && !ws_active) {
      PollOnce(me);
    }
    // Adaptive cadence: fast while a party/invite is live (also the liveness
    // heartbeat, well under the server's 30s reap), slow while idle or WS-backed.
    bool busy;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy = in_party_ || !invites_.empty();
    }
    const auto interval = (ws_active || !busy) ? 5s : 2s;
    // Sleep in short slices so Stop() is responsive.
    for (auto slept = 0ms; slept < interval && running_.load(); slept += 200ms) {
      std::this_thread::sleep_for(200ms);
    }
  }
}

void PartyManager::PollOnce(uint64_t me) {
  auto* api = Api();
  if (!api) {
    return;
  }
  auto resp = api->Get(XLiveAPI::BuildEndpoint("party/poll?xuid=" +
                                                    HexXuid(me)));
  if (!resp) {
    return;
  }
  if (!HttpOk(resp->StatusCode())) {
    return;  // 409 relogin_required etc. -- transient; next poll retries
  }
  const auto& raw = resp->RawResponse();
  if (raw.response && raw.size) {
    HandlePollBody(raw.response, raw.size);
  }
}

void PartyManager::HandlePollBody(const char* body, size_t len) {
  rapidjson::Document d;
  d.Parse(body, len);
  if (d.HasParseError() || !d.IsObject()) {
    return;
  }

  // party: Party | null
  bool present = false;
  std::string id;
  uint64_t owner = 0;
  std::vector<PartyMember> members;
  if (d.HasMember("party") && d["party"].IsObject()) {
    present = ParsePartyObject(d["party"], id, owner, members);
  }
  ApplyParty(present, id, owner, std::move(members));

  // invites: [ { partyId, fromXuid, fromGamertag } ]
  std::vector<PartyInvite> invites;
  if (d.HasMember("invites")) {
    invites = ParseInviteArray(d["invites"]);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    invites_ = std::move(invites);
  }
}

void PartyManager::HandlePartyBody(const char* body, size_t len) {
  rapidjson::Document d;
  d.Parse(body, len);
  if (d.HasParseError()) {
    return;
  }
  std::string id;
  uint64_t owner = 0;
  std::vector<PartyMember> members;
  if (ParsePartyObject(d, id, owner, members)) {
    ApplyParty(true, id, owner, std::move(members));
  }
}

void PartyManager::OnLiveEvent(const std::string& message) {
  rapidjson::Document d;
  d.Parse(message.c_str(), message.size());
  if (d.HasParseError() || !d.IsObject() || !d.HasMember("type") ||
      !d["type"].IsString()) {
    return;
  }
  const std::string type(d["type"].GetString(), d["type"].GetStringLength());
  const rapidjson::Value* payload =
      d.HasMember("payload") && d["payload"].IsObject() ? &d["payload"] : nullptr;

  if (type == "snapshot") {
    // party portion (friends/incoming/outgoing are the FriendsManager's).
    if (payload && payload->HasMember("party") &&
        (*payload)["party"].IsObject()) {
      std::string id;
      uint64_t owner = 0;
      std::vector<PartyMember> members;
      if (ParsePartyObject((*payload)["party"], id, owner, members)) {
        ApplyParty(true, id, owner, std::move(members));
      } else {
        ApplyParty(false, std::string(), 0, {});
      }
    } else {
      ApplyParty(false, std::string(), 0, {});  // "party": null -> not in one
    }
    // partyInvites: [...] -- pending invites present at connect. Full replace so
    // a reconnect re-syncs (the snapshot is authoritative, no replay).
    std::vector<PartyInvite> invites;
    if (payload && payload->HasMember("partyInvites")) {
      invites = ParseInviteArray((*payload)["partyInvites"]);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invites_ = std::move(invites);
    }
  } else if (type == "party.roster") {
    std::string id;
    uint64_t owner = 0;
    std::vector<PartyMember> members;
    if (payload && ParsePartyObject(*payload, id, owner, members)) {
      ApplyParty(true, id, owner, std::move(members));
    }
  } else if (type == "party.invite") {
    if (payload) {
      PartyInvite pi;
      pi.party_id = GetStr(*payload, "partyId");
      pi.from_xuid = ParseHexU64(GetStr(*payload, "fromXuid"));
      pi.from_gamertag = GetStr(*payload, "fromGamertag");
      if (!pi.party_id.empty()) {
        std::string from_gamertag = pi.from_gamertag;
        bool is_new = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          const bool dup = std::any_of(
              invites_.begin(), invites_.end(),
              [&](const PartyInvite& x) { return x.party_id == pi.party_id; });
          if (!dup) {
            invites_.push_back(std::move(pi));
            is_new = true;
          }
        }
        if (is_new) {  // toast only a genuinely new invite (deltas can repeat)
          ShowInviteToast(from_gamertag);
        }
      }
    }
  } else if (type == "party.dissolved") {
    const std::string pid = payload ? GetStr(*payload, "partyId") : std::string();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invites_.erase(
          std::remove_if(invites_.begin(), invites_.end(),
                         [&](const PartyInvite& i) { return i.party_id == pid; }),
          invites_.end());
    }
    if (!pid.empty() && pid == GetPartyId()) {
      ApplyParty(false, std::string(), 0, {});  // my party dissolved
    }
  }
}

void PartyManager::ApplyParty(bool present, const std::string& party_id,
                              uint64_t owner_xuid,
                              std::vector<PartyMember> members) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_party_ = present;
    if (present) {
      party_id_ = party_id;
      owner_xuid_ = owner_xuid;
      members_ = std::move(members);
    } else {
      party_id_.clear();
      owner_xuid_ = 0;
      members_.clear();
    }
  }
  UpdateVoiceRoster();
}

void PartyManager::UpdateVoiceRoster() {
  const uint64_t me = LocalOnlineXuid();
  std::vector<PartyMember> snapshot;
  bool in_party;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = members_;
    in_party = in_party_;
  }

  // Map every other member's console MAC into the GNS registry (so the relay can
  // route to it) and collect its synthetic INA for the voice broadcast set.
  std::vector<uint32_t> inas;
  inas.reserve(snapshot.size());
  auto* gns = GNSTransport::Get();
  for (const auto& m : snapshot) {
    if (m.xuid == me || !m.peer_key) {
      continue;  // skip self and any member the server couldn't resolve a MAC for
    }
    const uint32_t ina = GNSTransport::SyntheticInaFromPeerKey(m.peer_key);
    gns->MapPeer(ina, m.peer_key);
    inas.push_back(ina);
  }

  VoiceChannel::Get()->SetPartyRoster(inas);

  const bool want_voice = in_party && !inas.empty();
  bool started_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (want_voice && !voice_started_) {
      VoiceChannel::Get()->Start();
      voice_started_ = true;
      started_now = true;
      XELOGI("[party] voice started ({} member(s))", inas.size());
    } else if (!want_voice && voice_started_) {
      VoiceChannel::Get()->Stop();
      voice_started_ = false;
      XELOGI("[party] voice stopped");
    }
  }
  // Bring up the floating voice overlay on the start edge (it self-closes when
  // the party ends). Done outside the lock -- it posts to the UI thread.
  if (started_now) {
    ShowVoiceOverlay();
  }
}

// --- UI actions ------------------------------------------------------------

bool PartyManager::CreateParty() {
  const uint64_t me = LocalOnlineXuid();
  if (!me) {
    return false;
  }
  std::thread(&PartyManager::DoCreate, this, me).detach();
  return true;
}

bool PartyManager::Invite(uint64_t target_xuid) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || !target_xuid) {
    return false;
  }
  std::string id;
  bool start_create = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (in_party_) {
      id = party_id_;
    } else {
      // No party yet: queue the invite and create one (once).
      pending_after_create_.push_back(target_xuid);
      if (!creating_) {
        creating_ = true;
        start_create = true;
      }
    }
  }
  if (!id.empty()) {
    std::thread(&PartyManager::DoInvite, this, id, me, target_xuid).detach();
  } else if (start_create) {
    std::thread(&PartyManager::DoCreate, this, me).detach();
  }
  return true;
}

bool PartyManager::Join(const std::string& party_id) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || party_id.empty()) {
    return false;
  }
  std::thread(&PartyManager::DoJoin, this, party_id, me).detach();
  return true;
}

bool PartyManager::Leave() {
  const uint64_t me = LocalOnlineXuid();
  const std::string id = GetPartyId();
  if (!me || id.empty()) {
    return false;
  }
  std::thread(&PartyManager::DoLeave, this, id, me).detach();
  return true;
}

bool PartyManager::Decline(const std::string& party_id) {
  const uint64_t me = LocalOnlineXuid();
  if (!me || party_id.empty()) {
    return false;
  }
  std::thread(&PartyManager::DoDecline, this, party_id, me).detach();
  return true;
}

void PartyManager::DoCreate(uint64_t me) {
  auto* api = Api();
  if (api) {
    const std::string body = "{\"ownerXuid\":\"" + HexXuid(me) + "\"}";
    auto resp = api->Post(XLiveAPI::BuildEndpoint("party"),
                          reinterpret_cast<const uint8_t*>(body.data()));
    if (resp && HttpOk(resp->StatusCode())) {
      const auto& raw = resp->RawResponse();
      if (raw.response && raw.size) {
        HandlePartyBody(raw.response, raw.size);  // sets in_party_ + party_id_
      }
      XELOGI("[party] created");
    } else {
      XELOGW("[party] create failed: {}",
             resp ? resp->Message() : std::string("no response"));
    }
  }
  // Send any invites queued while the party was being created; clear the flag
  // either way so a failed create doesn't wedge future invites.
  std::vector<uint64_t> pend;
  std::string id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    creating_ = false;
    pend.swap(pending_after_create_);
    id = in_party_ ? party_id_ : std::string();
  }
  if (!id.empty()) {
    for (uint64_t t : pend) {
      std::thread(&PartyManager::DoInvite, this, id, me, t).detach();
    }
  }
}

void PartyManager::DoInvite(std::string party_id, uint64_t me,
                            uint64_t target) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"fromXuid\":\"" + HexXuid(me) +
                           "\",\"targetXuid\":\"" + HexXuid(target) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("party/" + party_id +
                                                     "/invite"),
                             reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    XELOGI("[party] invited {:016X}", target);
  } else {
    XELOGW("[party] invite failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

void PartyManager::DoJoin(std::string party_id, uint64_t me) {
  auto* api = Api();
  if (!api) {
    return;
  }
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\"}";
  auto resp = api->Post(XLiveAPI::BuildEndpoint("party/" + party_id +
                                                     "/join"),
                             reinterpret_cast<const uint8_t*>(body.data()));
  if (resp && HttpOk(resp->StatusCode())) {
    const auto& raw = resp->RawResponse();
    if (raw.response && raw.size) {
      HandlePartyBody(raw.response, raw.size);
    }
    // We've acted on the invite; drop it locally. The server clears it on join,
    // but joining pushes no WS update and the poll is suppressed while the WS is
    // up, so nothing else reconciles invites_ -- the accepted invite would
    // otherwise linger (same optimistic clear as DoDecline).
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invites_.erase(
          std::remove_if(
              invites_.begin(), invites_.end(),
              [&](const PartyInvite& i) { return i.party_id == party_id; }),
          invites_.end());
    }
    XELOGI("[party] joined {}", party_id);
  } else {
    XELOGW("[party] join failed: {}",
           resp ? resp->Message() : std::string("no response"));
  }
}

void PartyManager::DoLeave(std::string party_id, uint64_t me) {
  auto* api = Api();
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\"}";
  if (api) {
    api->Post(XLiveAPI::BuildEndpoint("party/" + party_id + "/leave"),
              reinterpret_cast<const uint8_t*>(body.data()));
  }
  // Whatever the server says, we're leaving locally: drop voice + state now.
  ApplyParty(false, std::string(), 0, {});
  XELOGI("[party] left {}", party_id);
}

void PartyManager::DoDecline(std::string party_id, uint64_t me) {
  auto* api = Api();
  const std::string body = "{\"xuid\":\"" + HexXuid(me) + "\"}";
  if (api) {
    api->Post(XLiveAPI::BuildEndpoint("party/" + party_id + "/decline"),
              reinterpret_cast<const uint8_t*>(body.data()));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&](const PartyInvite& i) { return i.party_id == party_id; }),
        invites_.end());
  }
}

// --- UI snapshots ----------------------------------------------------------

bool PartyManager::InParty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return in_party_;
}

std::string PartyManager::GetPartyId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return party_id_;
}

uint64_t PartyManager::GetOwnerXuid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return owner_xuid_;
}

std::vector<PartyMember> PartyManager::GetMembers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return members_;
}

std::vector<PartyInvite> PartyManager::GetInvites() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return invites_;
}

bool PartyManager::IsMember(uint64_t target_xuid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& m : members_) {
    if (m.xuid == target_xuid) {
      return true;
    }
  }
  return false;
}

}  // namespace kernel
}  // namespace xe
