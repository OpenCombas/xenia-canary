#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_WEBSOCKETBINDREQUEST_OBJECT_JSON_H_
#define XENIA_KERNEL_WEBSOCKETBINDREQUEST_OBJECT_JSON_H_

#include "xenia/kernel/json/base_object_json.h"

namespace xe {
namespace kernel {
class WebsocketBindRequestObjectJSON : public BaseObjectJSON {
 public:
  using BaseObjectJSON::Deserialize;
  using BaseObjectJSON::Serialize;

  WebsocketBindRequestObjectJSON();
  virtual ~WebsocketBindRequestObjectJSON();

  virtual bool Deserialize(const rapidjson::Value& obj);
  virtual bool Serialize(
      rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const;

  const std::string& Type() const { return type_; }
  void Type(const std::string& type) { type_ = type; }

  const std::string& Ip() const { return ip_; }
  void Ip(const std::string& ip) { ip_ = ip; }

  const uint16_t& Port() const { return port_; }
  void Port(const uint16_t& port) { port_ = port; }

 private:
  std::string type_;
  std::string ip_;
  uint16_t port_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_WEBSOCKETBINDREQUEST_OBJECT_JSON_H_