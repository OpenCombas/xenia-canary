/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/base/string_util.h"
#include "xenia/kernel/json/websocket_bind_request_json.h"
#include "xenia/kernel/util/net_utils.h"

namespace xe {
namespace kernel {
WebsocketBindRequestObjectJSON::WebsocketBindRequestObjectJSON()
    : ip_(""), port_(0), type_("") {}

WebsocketBindRequestObjectJSON::~WebsocketBindRequestObjectJSON() {}

bool WebsocketBindRequestObjectJSON::Deserialize(
    const rapidjson::Value& obj) {
  Type();

  if (obj.HasMember("ip")) {
    Ip(obj["ip"].GetString());
  }
  if (obj.HasMember("port")) {
      Port(obj["port"].GetInt());
  }
  if (obj.HasMember("type")) {
    Type(obj["type"].GetString());
  }
  return true;
}

bool WebsocketBindRequestObjectJSON::Serialize(
    rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const {
  writer->StartObject();

  writer->String("type");
  writer->String(type_);

  writer->String("ip");
  writer->String(ip_);

  writer->String("port");
  writer->Int(port_);

  writer->EndObject();
  return true;
}
}  // namespace kernel
}  // namespace xe