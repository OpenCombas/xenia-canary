/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay_auth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>
#include <third_party/rapidjson/include/rapidjson/stringbuffer.h>
#include <third_party/rapidjson/include/rapidjson/writer.h>

#include "third_party/imgui/imgui.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"

namespace xe {
namespace kernel {

namespace {

std::filesystem::path AuthFilePath() {
  return xe::filesystem::GetExecutableFolder() / "netplay_auth.json";
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(v));
  return std::string(buf);
}

std::string Base64Url(const uint8_t* data, size_t len) {
  static const char* t =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
    out.push_back(t[(n >> 18) & 63]);
    out.push_back(t[(n >> 12) & 63]);
    if (i + 1 < len) out.push_back(t[(n >> 6) & 63]);
    if (i + 2 < len) out.push_back(t[n & 63]);
  }
  return out;
}

// A one-shot "save your recovery code" popup. The code sits in a read-only,
// auto-selecting text field with a Copy button so it can be highlighted/copied.
class RecoveryCodeDialog final : public ui::ImGuiDialog {
 public:
  RecoveryCodeDialog(ui::ImGuiDrawer* drawer, std::string code)
      : ui::ImGuiDialog(drawer), code_(std::move(code)) {}

  void OnDraw(ImGuiIO& io) override {
    if (!opened_) {
      ImGui::OpenPopup("Netplay Recovery Code");
      opened_ = true;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, -1), ImVec2(420, -1));
    if (ImGui::BeginPopupModal("Netplay Recovery Code", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped(
          "This is your netplay account RECOVERY CODE. Save it somewhere safe "
          "-- it's the only way to recover your account on a new install, and "
          "it is shown only once.");
      ImGui::Spacing();
      // Read-only + auto-select so it highlights/copies; a Copy button too.
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##recovery", code_.data(), code_.size() + 1,
                       ImGuiInputTextFlags_ReadOnly |
                           ImGuiInputTextFlags_AutoSelectAll);
      if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(code_.c_str());
      }
      ImGui::SameLine();
      if (ImGui::Button("I've saved it")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  std::string code_;
  bool opened_ = false;
};

}  // namespace

NetplayAuth* NetplayAuth::Get() {
  static NetplayAuth instance;
  return &instance;
}

std::string NetplayAuth::GenerateSecret(size_t bytes) {
  std::random_device rd;
  std::vector<uint8_t> raw(bytes);
  for (auto& b : raw) {
    b = static_cast<uint8_t>(rd());
  }
  return Base64Url(raw.data(), raw.size());
}

void NetplayAuth::Load() {
  std::ifstream f(AuthFilePath(), std::ios::binary);
  if (!f) {
    return;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string body = ss.str();
  rapidjson::Document d;
  d.Parse(body.c_str(), body.size());
  if (d.HasParseError() || !d.IsObject()) {
    return;
  }
  for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
    if (!it->value.IsObject()) {
      continue;
    }
    const uint64_t xuid = std::strtoull(it->name.GetString(), nullptr, 16);
    if (!xuid) {
      continue;
    }
    Entry e;
    const auto& o = it->value;
    if (o.HasMember("token") && o["token"].IsString()) {
      e.token = o["token"].GetString();
    }
    if (o.HasMember("password") && o["password"].IsString()) {
      e.password = o["password"].GetString();
    }
    if (o.HasMember("recovery") && o["recovery"].IsString()) {
      e.recovery_code = o["recovery"].GetString();
    }
    entries_[xuid] = std::move(e);
  }
}

void NetplayAuth::Save() {
  rapidjson::Document d;
  d.SetObject();
  auto& al = d.GetAllocator();
  for (const auto& [xuid, e] : entries_) {
    // NOTE: token + password persist (password re-issues tokens); the recovery
    // code is deliberately NOT written -- it's a one-time secret the user saves.
    rapidjson::Value o(rapidjson::kObjectType);
    o.AddMember("token", rapidjson::Value(e.token, al), al);
    o.AddMember("password", rapidjson::Value(e.password, al), al);
    d.AddMember(rapidjson::Value(Hex64(xuid), al), o, al);
  }
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  d.Accept(w);

  std::ofstream f(AuthFilePath(), std::ios::binary | std::ios::trunc);
  if (f) {
    f.write(sb.GetString(), sb.GetLength());
  }
}

std::string NetplayAuth::GetToken(uint64_t xuid) {
  if (!xuid) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(xuid);
  return it == entries_.end() ? std::string() : it->second.token;
}

std::string NetplayAuth::EnsurePassword(uint64_t xuid) {
  if (!xuid) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto& e = entries_[xuid];
  if (e.password.empty()) {
    e.password = GenerateSecret(24);  // 32-char base64url
    Save();
  }
  return e.password;
}

void NetplayAuth::StoreClaim(uint64_t xuid, const std::string& token,
                             const std::string& recovery_code) {
  if (!xuid || token.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = entries_[xuid];
    e.token = token;
    if (!recovery_code.empty()) {
      e.recovery_code = recovery_code;
      pending_recovery_ = recovery_code;
    }
    Save();
  }
  XELOGI("[auth] claimed xuid {:016X}; token stored{}", xuid,
         recovery_code.empty() ? "" : " (recovery code queued)");
  if (!recovery_code.empty()) {
    MaybeShowRecoveryDialog();
  }
}

void NetplayAuth::StoreToken(uint64_t xuid, const std::string& token) {
  if (!xuid || token.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[xuid].token = token;
  Save();
}

std::string NetplayAuth::ReAuth(uint64_t xuid) {
  std::string password;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Throttle: at most one re-auth attempt per 30s (a permanently-bad
    // credential shouldn't hammer /auth/token on every failing poll).
    const int64_t now = NowMs();
    if (now - last_reauth_ms_ < 30000) {
      return std::string();
    }
    last_reauth_ms_ = now;
    auto it = entries_.find(xuid);
    if (it == entries_.end() || it->second.password.empty()) {
      return std::string();  // no password -> can't re-auth (needs recovery)
    }
    password = it->second.password;
  }

  auto* api = kernel_state() ? kernel_state()->GetXboxLiveAPI() : nullptr;
  if (!api) {
    return std::string();
  }
  const std::string body =
      fmt::format("{{\"xuid\":\"{:016X}\",\"password\":\"{}\"}}", xuid, password);
  auto resp = api->Post(XLiveAPI::BuildEndpoint("auth/token"),
                        reinterpret_cast<const uint8_t*>(body.data()));
  if (!resp) {
    return std::string();
  }
  const auto& raw = resp->RawResponse();
  if (raw.response && raw.size) {
    rapidjson::Document d;
    d.Parse(raw.response, raw.size);
    if (!d.HasParseError() && d.IsObject() && d.HasMember("token") &&
        d["token"].IsString()) {
      std::string token(d["token"].GetString(), d["token"].GetStringLength());
      StoreToken(xuid, token);
      XELOGI("[auth] re-authed xuid {:016X}", xuid);
      return token;
    }
  }
  XELOGW("[auth] re-auth failed for xuid {:016X} (http {})", xuid,
         resp->StatusCode());
  return std::string();
}

bool NetplayAuth::HasPendingRecoveryCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  return !pending_recovery_.empty();
}

std::string NetplayAuth::PendingRecoveryCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_recovery_;
}

void NetplayAuth::ClearPendingRecovery() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_recovery_.clear();
}

void NetplayAuth::MaybeShowRecoveryDialog() {
  std::string code;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    code = pending_recovery_;
  }
  if (code.empty() || !kernel_state()) {
    return;  // no UI up yet; the Diagnostics tab can still surface it later
  }
  Emulator* emulator = kernel_state()->emulator();
  if (!emulator || !emulator->display_window() || !emulator->imgui_drawer()) {
    return;
  }
  ClearPendingRecovery();  // we're presenting it now
  emulator->display_window()->app_context().CallInUIThread([emulator, code]() {
    if (emulator->imgui_drawer()) {
      new RecoveryCodeDialog(emulator->imgui_drawer(), code);  // self-owned
    }
  });
}

}  // namespace kernel
}  // namespace xe
