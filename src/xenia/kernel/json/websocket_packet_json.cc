/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
extern "C" {
#include "third_party/FFmpeg/libavutil/base64.h"
}

#include <string>

#include "xenia/base/string_util.h"
#include "xenia/kernel/json/websocket_packet_json.h"
#include "xenia/kernel/util/net_utils.h"

#define RAPIDJSON_NO_SIZETYPEDEFINE


namespace xe {
namespace kernel {
WebsocketPacketObjectJSON::WebsocketPacketObjectJSON()
    : type_(""),
      source_ip_(""),
      source_port_(0),
      target_ip_(""),
      target_port_(0),
      payload_({}) {}

WebsocketPacketObjectJSON::~WebsocketPacketObjectJSON() {}

bool WebsocketPacketObjectJSON::Deserialize(const rapidjson::Value& obj) {
  if (obj.HasMember("type")) {
    Type(obj["type"].GetString());
  }
  if (obj.HasMember("source_ip")) {
    SourceIp(obj["source_ip"].GetString());
  }
  if (obj.HasMember("source_port")) {
    SourcePort(obj["source_port"].GetInt());
  }
  if (obj.HasMember("target_ip")) {
    TargetIp(obj["source_ip"].GetString());
  }
  if (obj.HasMember("target_port")) {
    TargetPort(obj["target_port"].GetInt());
  }
  if (obj.HasMember("payload")) {
    const std::string base64 = obj["payload"].GetString();
    std::uint32_t base64_size = static_cast<uint32_t>(base64.size());
    std::uint32_t base64_decode_size = AV_BASE64_DECODE_SIZE(base64_size);

    uint8_t* data_out = new uint8_t[base64_decode_size];
    auto out = av_base64_decode(data_out, base64.c_str(), base64_decode_size);

    std::span<uint8_t> data_span =
        std::span<uint8_t>(data_out, base64_decode_size);
    payload_ = data_span;
  }
  return true;
}

bool WebsocketPacketObjectJSON::Serialize(
    rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const {
  writer->StartObject();

  writer->String("type");
  writer->String(type_);

  writer->String("source_ip");
  writer->String(source_ip_);

  writer->String("source_port");
  writer->Int(source_port_);

  writer->String("target_ip");
  writer->String(target_ip_);

  writer->String("target_port");
  writer->Int(target_port_);

  writer->String("payload");
  //const char* payload_ptr = reinterpret_cast<const char*>(payload_.data());
  //rapidjson::SizeType payload_size =
  //    static_cast<rapidjson::SizeType>(payload_.size());
  const uint32_t payload_size = static_cast<uint32_t>(payload_.size());
  const uint32_t payload_out_size = AV_BASE64_SIZE(payload_size);

  std::vector<char> payload_serialized(payload_out_size);

  auto out = av_base64_encode(payload_serialized.data(), payload_out_size,
                              payload_.data(), payload_size);

  std::string base64_out = std::string(payload_serialized.data());
  writer->String(base64_out);

  //writer->RawNumber(payload_ptr, payload_size, true);
  writer->EndObject();
  return true;
}
}  // namespace kernel
}  // namespace xe