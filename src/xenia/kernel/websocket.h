/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_WEBSOCKETCLIENT_H_
#define XENIA_KERNEL_WEBSOCKETCLIENT_H_


#include <windows.h>
#include <winhttp.h>

#include "xenia/base/platform_win.h"
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#include "xenia/kernel/json/websocket_packet_json.h"
#include "xenia/kernel/json/websocket_bind_request_json.h"
#include "xenia/kernel/util/net_utils.h"

#pragma comment(lib, "winhttp.lib")

namespace xe {
namespace kernel {

class WebSocketClient {
 public:
  WebSocketClient();
  ~WebSocketClient();

  bool connect(const std::string& host, INTERNET_PORT port,
               const std::string& path);
  bool bind(std::string ip_addr, uint16_t port);
  int sendto(uint8_t* buf, uint32_t buf_len, sockaddr* to, int tolen);
  int recvfrom(uint8_t* buffer, uint32_t buf_len, sockaddr* from,
               uint32_t* from_len);
  void enqueue_message(WebsocketPacketObjectJSON msg);
  void receive_loop();
  void close();

 private:
  void log_error(const std::string& tag);


  std::wstring host_;
  INTERNET_PORT port_;
  std::wstring path_;

  std::string bound_ip_;
  uint16_t bound_port_;

  HINTERNET hSession = nullptr;
  HINTERNET hConnect = nullptr;
  HINTERNET hRequest = nullptr;
  HINTERNET hWebSocket = nullptr;

  std::thread recv_thread;
  std::atomic<bool> running{false};

  struct Message {
    std::string payload;
    std::string source_ip;
    int source_port;
  };

  std::queue<WebsocketPacketObjectJSON> msg_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
};
}  // namespace kernel
}  // namespace xe
#endif  // XENIA_KERNEL_WEBSOCKETCLIENT_H_