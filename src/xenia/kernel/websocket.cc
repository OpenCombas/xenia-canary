#include "xenia/base/platform.h"

#include "src/xenia/kernel/websocket.h"
#include "xenia/emulator.h"
#include "xenia/base/logging.h"
#include <codecvt>
#include <string>
#include <span>

namespace xe {
namespace kernel {
WebSocketClient::WebSocketClient() {}

bool WebSocketClient::connect(const std::string& host, INTERNET_PORT port,
                              const std::string& path) {
  host_ = std::wstring(host.begin(), host.end());
  path_ = std::wstring(path.begin(), path.end());
  port_ = port;
  hSession =
      WinHttpOpen(L"WinHTTP WebSocket Client/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

  if (!hSession) return log_error("WinHttpOpen"), false;

  hConnect = WinHttpConnect(hSession, host_.c_str(), port_, 0);
  if (!hConnect) return log_error("WinHttpConnect"), false;

  hRequest =
      WinHttpOpenRequest(hConnect, L"GET", path_.c_str(), NULL,
                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

  if (!hRequest) return log_error("WinHttpOpenRequest"), false;

  WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);

  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    return log_error("WinHttpSendRequest"), false;

  if (!WinHttpReceiveResponse(hRequest, NULL))
    return log_error("WinHttpReceiveResponse"), false;

  hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
  if (!hWebSocket) return log_error("WinHttpWebSocketCompleteUpgrade"), false;

  running = true;
  recv_thread = std::thread(&WebSocketClient::receive_loop, this);
  std::wcout << L"[INFO] Connected\n";
  return true;
}

bool WebSocketClient::bind(std::string ip_addr, uint16_t port) {
  if (!hWebSocket) return false;

  WebsocketBindRequestObjectJSON bind_request = WebsocketBindRequestObjectJSON();
  std::string request_type = "bind";
  bound_ip_ = ip_addr;
  bound_port_ = port;
  bind_request.Ip(bound_ip_);
  bind_request.Port(bound_port_);
  bind_request.Type(request_type);

  std::string bind_request_output;
  bool valid = bind_request.Serialize(bind_request_output);
  assert_true(valid);


  DWORD res = WinHttpWebSocketSend(hWebSocket,
                                   WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
      (void*)bind_request_output.data(), (DWORD)bind_request_output.size());

  return res == ERROR_SUCCESS;
}

int WebSocketClient::sendto(uint8_t* buf, uint32_t buf_len,
                                    sockaddr* to, int tolen) {
  if (!hWebSocket) return false;

  const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(to);
  char ipbuf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, INET_ADDRSTRLEN);
  int port = ntohs(addr->sin_port);
  std::span<uint8_t> payload_data(buf, buf_len);
  WebsocketPacketObjectJSON send_packet = WebsocketPacketObjectJSON();
  send_packet.TargetIp(ip_to_string(addr->sin_addr));
  send_packet.TargetPort(port);
  send_packet.SourceIp(bound_ip_);
  send_packet.SourcePort(bound_port_);
  send_packet.Type("packet");
  send_packet.Payload(payload_data);

  std::string send_packet_out;
  bool valid = send_packet.Serialize(send_packet_out);
  assert_true(valid);

  DWORD res = WinHttpWebSocketSend(hWebSocket,
                                   WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
      (void*)send_packet_out.data(), (DWORD)send_packet_out.size());
  if (res == ERROR_SUCCESS) {
    return buf_len;
  } 
  return -1;
}

int WebSocketClient::recvfrom(uint8_t* buffer, uint32_t buf_len, sockaddr* from,
                              uint32_t* from_len) {
  std::unique_lock<std::mutex> lock(queue_mutex);

  if (msg_queue.empty()) {
    return 0;  // Indicate that no message is currently available
  }

  WebsocketPacketObjectJSON msg = msg_queue.front();
  msg_queue.pop();
  lock.unlock();

  // Copy payload to buffer
  int len = std::min((int)msg.Payload().size(), (int)buf_len);
  memcpy(buffer, msg.Payload().data(), len - 2);

  // Populate sockaddr_in
  sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(from);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(msg.SourcePort());
  inet_pton(AF_INET, msg.SourceIp().c_str(), &sin->sin_addr);
  *from_len = sizeof(sockaddr_in);

  return len - 2;
}

void WebSocketClient::enqueue_message(WebsocketPacketObjectJSON msg) {
  std::unique_lock<std::mutex> lock(queue_mutex);
  msg_queue.push({msg});
  lock.unlock();
  queue_cv.notify_one();
}

void WebSocketClient::receive_loop() {
  while (running) {
    char buffer[2048];
    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

    DWORD result = WinHttpWebSocketReceive(hWebSocket, buffer, sizeof(buffer),
                                           &bytesRead, &bufferType);

    if (result != ERROR_SUCCESS || bytesRead == 0) {
      std::cerr << "[ERROR] WebSocket receive failed: " << result << std::endl;
      break;
    }
    WebsocketPacketObjectJSON msg_out = WebsocketPacketObjectJSON();
    std::string msg(buffer, bytesRead);
    bool valid = msg_out.Deserialize(msg);
    assert_true(valid);
    enqueue_message(msg_out);
  }

  running = false;
  queue_cv.notify_all();
}
void WebSocketClient::close() {
  running = false;
  if (recv_thread.joinable()) recv_thread.join();
  if (hWebSocket) {
    WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                          NULL, 0);
    WinHttpCloseHandle(hWebSocket);
    hWebSocket = nullptr;
  }
  if (hRequest) WinHttpCloseHandle(hRequest), hRequest = nullptr;
  if (hConnect) WinHttpCloseHandle(hConnect), hConnect = nullptr;
  if (hSession) WinHttpCloseHandle(hSession), hSession = nullptr;
}

WebSocketClient::~WebSocketClient() { close(); }

void WebSocketClient::log_error(const std::string& tag) {
  std::cerr << "[ERROR] " << tag << " failed: " << GetLastError() << std::endl;
}
}  // namespace kernel
}  // namespace xe