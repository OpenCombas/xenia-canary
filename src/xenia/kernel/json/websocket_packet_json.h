/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_WEBSOCKET_PACKET_JSON_H_
#define XENIA_KERNEL_WEBSOCKET_PACKET_JSON_H_

#include "xenia/kernel/json/base_object_json.h"

#include <span>

namespace xe {
namespace kernel {
class WebsocketPacketObjectJSON : public BaseObjectJSON {
 public:
  using BaseObjectJSON::Deserialize;
  using BaseObjectJSON::Serialize;

  WebsocketPacketObjectJSON();
  virtual ~WebsocketPacketObjectJSON();

  virtual bool Deserialize(const rapidjson::Value& obj);
  virtual bool Serialize(
      rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const;

  const std::string& Type() const { return type_; }
  void Type(const std::string& type) { type_ = type; }

  const std::string& SourceIp() const { return source_ip_; }
  void SourceIp(const std::string& ip) { source_ip_ = ip; }

  const uint16_t& SourcePort() const { return source_port_; }
  void SourcePort(const uint16_t& port) { source_port_ = port; }

  const std::string& TargetIp() const { return target_ip_; }
  void TargetIp(const std::string& ip) { target_ip_ = ip; }

  const uint16_t& TargetPort() const { return target_port_; }
  void TargetPort(const uint16_t& port) { target_port_ = port; }

  const std::span<uint8_t>& Payload() const { return payload_; }
  void Payload(const std::span<uint8_t>& payload) { payload_ = payload; }

 private:
  std::string type_;
  std::string source_ip_;
  uint16_t source_port_;
  std::string target_ip_;
  uint16_t target_port_;
  std::span<uint8_t> payload_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_WEBSOCKET_PACKET_JSON_H_