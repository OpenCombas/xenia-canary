/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_NETPLAY_MANAGER_UI_H_
#define XENIA_KERNEL_XAM_UI_NETPLAY_MANAGER_UI_H_

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <set>

#include "third_party/imgui/imgui.h"

namespace xe {
namespace ui {
class ImmediateTexture;
}  // namespace ui
namespace kernel {
namespace xam {
namespace ui {

struct AddFriendArgs {
  bool add_friend_open;
  bool add_friend_first_draw;
  bool search_filter_context_open;
  bool add_friend_context_open;
  bool added_friend;
  bool are_friends;
  bool valid_xuid;
  char add_xuid_[17];
};

struct FriendsContentArgs {
  bool first_draw;
  bool friends_open;
  bool filter_joinable;
  bool filter_title;
  bool filter_offline;
  bool refresh_presence;
  AddFriendArgs add_friend_args = {};
  ImGuiTextFilter filter = {};
  // Server-friend gamerpic fetch state (owned by xeDrawFriendsContent so every
  // host of the friends list gets pics without duplicating the logic).
  std::set<uint64_t> gamerpic_xuids = {};
  std::future<std::map<uint64_t, std::shared_ptr<xe::ui::ImmediateTexture>>>
      gamerpic_fetch = {};
};

struct SessionsContentArgs {
  bool first_draw;
  bool sessions_open;
  bool filter_own;
  bool refresh_sessions;
  bool refresh_sessions_sync;
};

struct MyDeletedProfilesArgs {
  bool first_draw;
  bool deleted_profiles_open;
};

struct UPnPAndPortsArgs {
  bool first_draw;
  bool dialog_open;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
