#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include <shared_mutex>

#include <third_party/libjuice/include/juice/juice.h>
#include <xenia/base/threading_timer_queue.h>

namespace xe {
namespace kernel {

class TURN {
 public:
  TURN();

  void Initialize();



 private:

};
}  // namespace kernel
}  // namespace xe
