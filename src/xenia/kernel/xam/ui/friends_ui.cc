/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/friends_ui.h"
#include "xenia/kernel/XLiveAPI.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

FriendsUI::FriendsUI(xe::ui::ImGuiDrawer* imgui_drawer, UserProfile* profile)
    : XamDialog(imgui_drawer), profile_(profile) {
  friends_presence_ = kernel_state()->GetXboxLiveAPI()->GetFriendsPresenceAsync(
      profile->xuid());
  // Gamerpics for the server-authoritative friends are fetched lazily in OnDraw
  // (the list is server-driven and can change), not from the config friends.
}

// TODO(Adrian): Move into a separate function so draw can be reused with dialog
// manager to reduce duplication.
void FriendsUI::OnDraw(ImGuiIO& io) {
  if (!args.friends_open) {
    args.first_draw = true;
    args.friends_open = true;

    ImGui::OpenPopup("Friends");

    // Show all friends by default (server presence can lag / the online flag is
    // still being nailed down; don't hide people out from under the user).
    args.filter_offline = false;
  }

  if (friends_presence_.valid()) {
    if (friends_presence_.wait_for(0s) == std::future_status::ready) {
      friends_presence_result_ = friends_presence_.get();
    }
  }

  if (args.refresh_presence) {
    friends_presence_ =
        kernel_state()->GetXboxLiveAPI()->GetFriendsPresenceAsync(
            profile_->xuid());
    args.refresh_presence = false;
  }

  // Server-friend gamerpics are fetched inside xeDrawFriendsContent now (shared
  // by all hosts), so nothing to do here.
  xeDrawFriendsContent(imgui_drawer(), profile_, args,
                       &friends_presence_result_, immediate_gamerpics_result_);

  if (!args.friends_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
