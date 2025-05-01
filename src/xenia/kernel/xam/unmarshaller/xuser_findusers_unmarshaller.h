/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UNMARSHALLER_XUSER_FINDUSERS_UNMARSHALLER_H_
#define XENIA_KERNEL_XAM_UNMARSHALLER_XUSER_FINDUSERS_UNMARSHALLER_H_

#include "xenia/kernel/xam/unmarshaller/unmarshaller.h"

namespace xe {
namespace kernel {
namespace xam {

class XUserFindUsersUnmarshaller : public Unmarshaller {
 public:
  XUserFindUsersUnmarshaller(uint32_t marshaller_buffer);

  ~XUserFindUsersUnmarshaller() {};

  virtual X_HRESULT Deserialize();

  // 1065
  const uint32_t ValueConst() const { return value_const_; };

  // Data from 58017
  const uint64_t UnknValue() const { return unkn_value_; };

  // XnpLogonGetStatus
  const SGADDR SecurityGateway() const { return security_gateway_; };

  const uint64_t XUIDIssuer() const { return xuid_issuer_; };

  const uint32_t NumUsers() const { return num_users_; };

  const std::vector<FIND_USER_INFO>& Users() const { return users_; };

 private:
  uint32_t value_const_;
  uint64_t unkn_value_;
  SGADDR security_gateway_;
  uint64_t xuid_issuer_;
  uint32_t num_users_;
  std::vector<FIND_USER_INFO> users_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_UNMARSHALLER_XUSER_FINDUSERS_UNMARSHALLER_H_