/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/base/string_util.h"
#include "xenia/kernel/json/nic_object_json.h"
#include "xenia/kernel/util/net_utils.h"

namespace xe {
namespace kernel {
NicObjectJSON::NicObjectJSON()
      : localIpAddress_(""), remoteIpAddress_(""), 
        sdp_("") {}

NicObjectJSON::~NicObjectJSON() {}

bool NicObjectJSON::Deserialize(const rapidjson::Value& obj) {

  if (obj.HasMember("localIpAddress")) {
    LocalIpAddress(obj["localIpAddress"].GetString());
  }

  if (obj.HasMember("remoteIpAddress")) {
    RemoteIpAddress(obj["remoteIpAddress"].GetString());
  }

  if (obj.HasMember("sdp")) {
    Sdp(obj["sdp"].GetString());
  }


  return true;
}

bool NicObjectJSON::Serialize(
    rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const {
  writer->StartObject();

  writer->String("localIpAddress");
  writer->String(localIpAddress_);

  writer->String("remoteIpAddress");
  writer->String(remoteIpAddress_);

  writer->String("sdp");
  writer->String(sdp_);

  writer->EndObject();

  return true;
}
}  // namespace kernel
}  // namespace xe