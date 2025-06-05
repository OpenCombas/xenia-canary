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
      : ipAddress_(""),
        macAddress_(0), 
        sdp_("") {}

NicObjectJSON::~NicObjectJSON() {}

bool NicObjectJSON::Deserialize(const rapidjson::Value& obj) {

  if (obj.HasMember("ipAddress")) {
    IpAddress(obj["ipAddress"].GetString());
  }

  if (obj.HasMember("macAddress")) {
    xe::kernel::MacAddress address =
        xe::kernel::MacAddress(obj["macAddress"].GetString());

    MacAddress(address.to_uint64());
  }

  if (obj.HasMember("sdp")) {
    IpAddress(obj["sdp"].GetString());
  }


  return true;
}

bool NicObjectJSON::Serialize(
    rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const {
  writer->StartObject();

  writer->String("ipAddress");
  writer->String(ipAddress_);

  writer->String("macAddress");
  writer->String(fmt::format("{:012x}", macAddress_.get()));

  writer->String("sdp");
  writer->String(sdp_);

  writer->EndObject();

  return true;
}
}  // namespace kernel
}  // namespace xe