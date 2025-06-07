#pragma once

#ifndef XENIA_KERNEL_JUICESOCK_H_
#define XENIA_KERNEL_JUICESOCK_H_


#include "xenia/kernel/json/base_object_json.h"
#include "xenia/kernel/json/http_response_object_json.h"
#include "xenia/kernel/json/nic_object_json.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/kernel_state.h"


#include "third_party/libcurl/include/curl/curl.h"
#include <third_party/libjuice/include/juice/juice.h>
#include <winsock2.h>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace xe {
namespace kernel {

class JuiceSocket {
 public:
  JuiceSocket(in_addr remote_ip);
  ~JuiceSocket();

  // Mimic socket functions
  X_STATUS bind(const sockaddr* name, int namelen);  // Placeholder, no-op
  X_STATUS connect(const std::string& remoteSdp);
  int recv(char* buffer, int len, int flags);
  int recvfrom(char* buffer, int len, int flags, sockaddr* from,
                    int* fromlen);
  int WSARecvFrom(char* buffer, int len, int flags, sockaddr* from,
                  int* fromlen);
  int WSAPoll(int timeout);
  int send(const char* buffer, int len, int flags);
  int sendto(const char* buffer, int len, int flags, const sockaddr* to,
             int tolen);
  X_STATUS closesocket();


  int setLocalSdp() const;
  void RegisterSDP(in_addr address_ip) const;

 private:
  static void onData(juice_agent_t* agent, const char* data, size_t size,
                     void* user_ptr);
  static void onCandidate(juice_agent_t* agent, const char* sdp, void* user_ptr);
  static void onGatheringDone(juice_agent_t* agent, void* user_ptr);
  static void onStateChanged(juice_agent_t* agent, juice_state_t state, void* user_ptr);

  void queueIncomingData(const char* data, size_t size);

  juice_agent_t* agent;
  juice_config_t config;
  char local_sdp[JUICE_MAX_SDP_STRING_LEN];
  char remote_sdp[JUICE_MAX_SDP_STRING_LEN];


  std::string turnServer;
  std::string username;
  std::string password;

  std::queue<std::vector<uint8_t>> recvQueue;
  std::mutex queueMutex;
  std::condition_variable dataAvailable;
  juice_state_t connectionState;
};
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_JUICESOCK_H_
