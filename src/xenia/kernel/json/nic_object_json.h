/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NIC_OBJECT_JSON_H_
#define XENIA_KERNEL_NIC_OBJECT_JSON_H_

#include <vector>

#include "xenia/kernel/json/base_object_json.h"

namespace xe {
namespace kernel {
class NicObjectJSON : public BaseObjectJSON {
 public:
  using BaseObjectJSON::Deserialize;
  using BaseObjectJSON::Serialize;

  NicObjectJSON();
  virtual ~NicObjectJSON();

  virtual bool Deserialize(const rapidjson::Value& obj);
  virtual bool Serialize(
      rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const;

  const std::string& LocalIpAddress() const { return localIpAddress_; }
  void LocalIpAddress(const std::string& localIpAddress) {
    localIpAddress_ = localIpAddress;
  }

  const xe::be<uint16_t> port() const { return port_; }
  void port(const xe::be<uint16_t> port) {
    port_ = port;
  }

  const std::string& Sdp() const { return sdp_; }
  void Sdp(const std::string& sdp) { sdp_ = sdp; }

 private:
  std::string localIpAddress_;
  xe::be<uint16_t> port_;
  std::string sdp_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NIC_OBJECT_JSON_H_