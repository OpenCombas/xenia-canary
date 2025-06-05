/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "util/net_utils.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/turn.h"

#include <third_party/libjuice/include/juice/juice.h>

DEFINE_string(turn_server_host, "", "TURN server url or IP", "Live");

DEFINE_string(turn_server_username, "", "TURN server username", "Live");

DEFINE_string(turn_server_password, "", "TURN server password", "Live");

DECLARE_int32(turn_server_port, 3478, "TURN server port.");

DEFINE_bool(turn_nat, false, "Use TURN NAT for port access", "Live");

DECLARE_bool(logging);

using namespace xe::threading;

namespace xe {
namespace kernel {

TURN::TURN() {}

void TURN::Initialize() {

  juice_config_t config;
  memset(&config, 0, sizeof(config));

  config.stun_server_host = cvars::turn_server_host.c_str();
  config.stun_server_port = cvars::turn_server_port;

  juice_turn_server_t turn_server;
  memset(&turn_server, 0, sizeof(turn_server));
  turn_server.host = cvars::turn_server_host.c_str();
  turn_server.port = cvars::turn_server_port;
  turn_server.username = cvars::turn_server_username.c_str();
  turn_server.password = cvars::turn_server_password.c_str();

  config.cb_recv = 



};

}  // namespace kernel
}  // namespace xe