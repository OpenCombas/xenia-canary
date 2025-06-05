#include <iostream>
#include <xenia/kernel/juicesocket.h>
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#include <third_party/libjuice/include/juice/juice.h>

#include <xenia/kernel/XLiveAPI.h>


DEFINE_string(turn_server_host, "", "TURN server url or IP", "Live");

DEFINE_string(turn_server_username, "", "TURN server username", "Live");

DEFINE_string(turn_server_password, "", "TURN server password", "Live");

DECLARE_int32(turn_server_port, 3478, "TURN server port.");

namespace xe {
namespace kernel {

JuiceSocket::JuiceSocket()
    : agent(nullptr),
      connectionState(JUICE_STATE_DISCONNECTED) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  juice_config_t config;

  memset(&config, 0, sizeof(config));
  config.user_ptr = this;
  config.stun_server_host = cvars::turn_server_host.c_str();
  config.stun_server_port = cvars::turn_server_port;

  juice_config_t config;
  memset(&config, 0, sizeof(config));


  juice_turn_server_t turn_server;
  memset(&turn_server, 0, sizeof(turn_server));
  turn_server.host = cvars::turn_server_host.c_str();
  turn_server.port = cvars::turn_server_port;
  turn_server.username = cvars::turn_server_username.c_str();
  turn_server.password = cvars::turn_server_password.c_str();

  config.cb_recv = &JuiceSocket::onData;
  config.cb_candidate = &JuiceSocket::onCandidate;
  config.cb_gathering_done = &JuiceSocket::onGatheringDone;
  config.cb_state_changed = &JuiceSocket::onStateChanged;

  setLocalSdp();

  agent = juice_create(&config);
}

JuiceSocket::~JuiceSocket() {
  if (agent) juice_destroy(agent);
}

X_STATUS JuiceSocket::bind(const sockaddr* name, int namelen) {
  return 0;
}

X_STATUS JuiceSocket::connect(const std::string& remoteSdp) {
  if (juice_set_remote_description(agent, remoteSdp.c_str()) != 0) return -1;

  return X_STATUS_SUCCESS;
}

int JuiceSocket::recv(char* buffer, int len, int flags) {
  std::unique_lock<std::mutex> lock(queueMutex);
  dataAvailable.wait(lock, [this] { return !recvQueue.empty(); });

  std::vector<uint8_t> data = std::move(recvQueue.front());
  recvQueue.pop();

  int copyLen = static_cast<int>(data.size());
  memcpy(buffer, data.data(), copyLen);
  return copyLen;
}

int JuiceSocket::recvfrom(char* buffer, int len, int flags, sockaddr* from,
                          int* fromlen) {
  return recv(buffer, len, flags);
}

int JuiceSocket::WSARecvFrom(char* buffer, int len, int flags,
                                  sockaddr* from,
                             int* fromlen) {
  return recv(buffer, len, flags);
}

int JuiceSocket::WSAPoll(int timeout) {
  std::unique_lock<std::mutex> lock(queueMutex);
  if (recvQueue.empty()) {
    if (timeout < 0) {
      dataAvailable.wait(lock, [this] { return !recvQueue.empty(); });
    } else {
      dataAvailable.wait_for(lock, std::chrono::milliseconds(timeout),
                             [this] { return !recvQueue.empty(); });
    }
  }
  return recvQueue.empty() ? 0 : 1;
}

int JuiceSocket::send(const char* buffer, int len, int flags) {
  if (!agent || connectionState != JUICE_STATE_CONNECTED) {
    return SOCKET_ERROR;
  }

  int res = juice_send(agent, buffer, len);
  return (res == 0) ? len : SOCKET_ERROR;
}

int JuiceSocket::sendto(const char* buffer, int len, int flags,
                        const sockaddr* to, int tolen) {
  return send(buffer, len, flags);
}

X_STATUS JuiceSocket::closesocket() {
  juice_destroy(agent);
  return X_STATUS_SUCCESS;
}

int JuiceSocket::setLocalSdp() const {
  char sdp[JUICE_MAX_SDP_STRING_LEN];
  juice_get_local_description(agent, sdp, JUICE_MAX_SDP_STRING_LEN);
  std::string result = sdp;
  std::unique_ptr<HTTPResponseObjectJSON> register_sdp_result =
      XLiveAPI::UpdateNicSdp(result);

  if (register_sdp_result &&
      register_sdp_result->StatusCode() == HTTP_STATUS_CODE::HTTP_CREATED) {
    XELOGI("Successfully posted SDP");
    return 0;
  }
  return 1;
}

void JuiceSocket::setRemoteSdp(in_addr address_ip) const {
  char sdp[JUICE_MAX_SDP_STRING_LEN];
  juice_set_remote_description(agent, sdp);
  juice_gather_candidates(agent);
}

void JuiceSocket::onData(juice_agent_t* agent, const char* data, size_t size, void* user_ptr) {
  JuiceSocket* self = static_cast<JuiceSocket*>(user_ptr);
  self->queueIncomingData(data, size);
}

void JuiceSocket::queueIncomingData(const char* data, size_t size) {
  std::lock_guard<std::mutex> lock(queueMutex);
  recvQueue.emplace(data, data + size);
  dataAvailable.notify_one();
}

void JuiceSocket::onCandidate(juice_agent_t* agent, const char* sdp,
                              void* user_ptr) {
  std::cout << "Candidate SDP: " << sdp << "\n";
}

void JuiceSocket::onGatheringDone(juice_agent_t* agent, void* user_ptr) {
  std::cout << "Candidate gathering done\n";
}

void JuiceSocket::onStateChanged(juice_agent_t* agent, juice_state_t state, void* user_ptr) {
  JuiceSocket* self = static_cast<JuiceSocket*>(user_ptr);
  self->connectionState = state;
  std::cout << "State changed: " << state << "\n";
};

}  // namespace kernel
}  // namespace xe
