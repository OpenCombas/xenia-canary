/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/log_uploader.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#define RAPIDJSON_HAS_STDSTRING 1
#include <third_party/rapidjson/include/rapidjson/document.h>

#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"

#ifdef _WIN32
#include "third_party/libcurl/include/curl/curl.h"
#else
#include <curl/curl.h>
#endif

DEFINE_string(
    log_sink_url, "",
    "Netplay diagnostics: URL the 'Upload Log' button POSTs the (gzip) log to. "
    "Empty = <api server>/logs. The button is only enabled when this endpoint "
    "answers a GET (server-gated), so the project can turn log collection on/off "
    "without a client build.",
    "Live");

DEFINE_int32(
    log_upload_max_mb, 256,
    "Netplay diagnostics: cap on how much of the log to upload, in MiB. A larger "
    "log is truncated to its last N MiB (the tail). Bounds memory + the tester's "
    "upload.",
    "Live");

namespace xe {
namespace kernel {

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t WallMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t LocalXuid() {
  if (!kernel_state()) {
    return 0;
  }
  auto* xam = kernel_state()->xam_state();
  if (!xam) {
    return 0;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!xam->IsUserSignedIn(i)) {
      continue;
    }
    xam::UserProfile* p = xam->GetUserProfile(i);
    if (p && p->GetOnlineXUID()) {
      return p->GetOnlineXUID();
    }
  }
  return 0;
}

std::string LocalGamertag() {
  if (!kernel_state()) {
    return std::string();
  }
  auto* xam = kernel_state()->xam_state();
  if (!xam) {
    return std::string();
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!xam->IsUserSignedIn(i)) {
      continue;
    }
    xam::UserProfile* p = xam->GetUserProfile(i);
    if (p) {
      return p->name();
    }
  }
  return std::string();
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

// Gzip the last `max_bytes` of the file at `path` into `out` (streaming, so peak
// memory is the compressed result, not the whole log). Returns the source bytes
// read (0 on failure); sets *truncated if the head was dropped.
uint64_t GzipTail(const std::filesystem::path& path, uint64_t max_bytes,
                  std::vector<uint8_t>& out, bool* truncated) {
  std::error_code ec;
  const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  if (ec) {
    return 0;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return 0;
  }
  uint64_t start = 0, to_read = size;
  if (size > max_bytes) {
    start = size - max_bytes;
    to_read = max_bytes;
    if (truncated) {
      *truncated = true;
    }
  }
  in.seekg(static_cast<std::streamoff>(start));

  zng_stream strm{};
  // windowBits 15 + 16 => gzip wrapper (matches Content-Encoding: gzip).
  if (zng_deflateInit2(&strm, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8,
                       Z_DEFAULT_STRATEGY) != Z_OK) {
    return 0;
  }

  std::vector<char> ibuf(256 * 1024);
  std::vector<uint8_t> obuf(256 * 1024);
  uint64_t remaining = to_read, read_total = 0;
  for (;;) {
    const std::streamsize want =
        static_cast<std::streamsize>(std::min<uint64_t>(ibuf.size(), remaining));
    if (want > 0) {
      in.read(ibuf.data(), want);
    }
    const std::streamsize n = want > 0 ? in.gcount() : 0;
    remaining -= static_cast<uint64_t>(n);
    read_total += static_cast<uint64_t>(n);
    const bool last = (n == 0 || remaining == 0);
    strm.next_in = reinterpret_cast<uint8_t*>(ibuf.data());
    strm.avail_in = static_cast<uint32_t>(n);
    const int flush = last ? Z_FINISH : Z_NO_FLUSH;
    do {
      strm.next_out = obuf.data();
      strm.avail_out = static_cast<uint32_t>(obuf.size());
      const int ret = zng_deflate(&strm, flush);
      if (ret == Z_STREAM_ERROR) {
        zng_deflateEnd(&strm);
        return 0;
      }
      out.insert(out.end(), obuf.data(),
                 obuf.data() + (obuf.size() - strm.avail_out));
    } while (strm.avail_out == 0);
    if (last) {
      break;
    }
  }
  zng_deflateEnd(&strm);
  return read_total;
}

}  // namespace

LogUploader* LogUploader::Get() {
  static LogUploader instance;
  return &instance;
}

std::string LogUploader::SinkUrl() const {
  if (!cvars::log_sink_url.empty()) {
    return cvars::log_sink_url;
  }
  return XLiveAPI::BuildEndpoint("logs");
}

void LogUploader::SetStatus(State state, const std::string& message) {
  state_.store(state);
  std::lock_guard<std::mutex> lock(msg_mutex_);
  status_message_ = message;
}

std::string LogUploader::status_message() const {
  std::lock_guard<std::mutex> lock(msg_mutex_);
  return status_message_;
}

void LogUploader::RefreshAvailability() {
  if (probing_.load()) {
    return;
  }
  const int64_t now = NowMs();
  // Re-probe at most every 5s; if already available, coast for 30s.
  if (available_.load() && now - last_probe_ms_.load() < 30000) {
    return;
  }
  if (now - last_probe_ms_.load() < 5000) {
    return;
  }
  bool expected = false;
  if (!probing_.compare_exchange_strong(expected, true)) {
    return;
  }
  last_probe_ms_.store(now);
  std::thread(&LogUploader::DoProbe, this).detach();
}

void LogUploader::DoProbe() {
  const std::string url = SinkUrl();
  bool ok = false;
  if (!url.empty()) {
    CURL* c = curl_easy_init();
    if (c) {
      std::string discard;
      curl_easy_setopt(c, CURLOPT_URL, url.c_str());
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteToString);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &discard);
      curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
      curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
      if (curl_easy_perform(c) == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        ok = (code == 200);
      }
      curl_easy_cleanup(c);
    }
  }
  available_.store(ok);
  probing_.store(false);
}

bool LogUploader::Upload(const std::string& note) {
  bool expected = false;
  if (!uploading_.compare_exchange_strong(expected, true)) {
    return false;  // an upload is already running
  }
  std::thread(&LogUploader::DoUpload, this, note).detach();
  return true;
}

void LogUploader::DoUpload(std::string note) {
  SetStatus(State::kUploading, "Compressing log...");

  const auto path = xe::GetLogFilePath();
  if (path.empty()) {
    SetStatus(State::kFailed, "No log file to upload.");
    uploading_.store(false);
    return;
  }
  FlushLog();

  const uint64_t max_bytes =
      static_cast<uint64_t>(std::max(1, cvars::log_upload_max_mb)) * 1024 * 1024;
  bool truncated = false;
  std::vector<uint8_t> gz;
  const uint64_t src = GzipTail(path, max_bytes, gz, &truncated);
  if (!src || gz.empty()) {
    SetStatus(State::kFailed, "Could not read/compress the log file.");
    uploading_.store(false);
    return;
  }

  SetStatus(State::kUploading, "Uploading...");

  const std::string url = SinkUrl();
  CURL* c = curl_easy_init();
  if (!c || url.empty()) {
    SetStatus(State::kFailed, "No log server configured.");
    if (c) {
      curl_easy_cleanup(c);
    }
    uploading_.store(false);
    return;
  }

  char* note_enc = curl_easy_escape(c, note.c_str(), static_cast<int>(note.size()));
  const std::string gt = LocalGamertag();
  char* gt_enc = curl_easy_escape(c, gt.c_str(), static_cast<int>(gt.size()));

  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Encoding: gzip");
  hdrs = curl_slist_append(hdrs, "Content-Type: text/plain; charset=utf-8");
  hdrs = curl_slist_append(
      hdrs, fmt::format("X-Xenia-Xuid: {:016X}", LocalXuid()).c_str());
  hdrs = curl_slist_append(
      hdrs, fmt::format("X-Xenia-Gamertag: {}", gt_enc ? gt_enc : "").c_str());
  hdrs = curl_slist_append(
      hdrs, fmt::format("X-Xenia-Note: {}", note_enc ? note_enc : "").c_str());
  hdrs = curl_slist_append(
      hdrs, fmt::format("X-Xenia-Log-Bytes: {}", src).c_str());
  hdrs = curl_slist_append(
      hdrs, fmt::format("X-Xenia-Truncated: {}", truncated ? 1 : 0).c_str());
  hdrs = curl_slist_append(hdrs,
                           fmt::format("X-Xenia-Time: {}", WallMs()).c_str());

  std::string resp;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, gz.data());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(gz.size()));
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &WriteToString);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);  // uploads can be multi-MB

  const CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

  if (note_enc) {
    curl_free(note_enc);
  }
  if (gt_enc) {
    curl_free(gt_enc);
  }
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);

  if (rc == CURLE_OK && (code == 200 || code == 201)) {
    // The report id is the tester-facing reference. NOTE: intentionally do NOT
    // surface any viewer url (the backend view is operator-only / behind a VPN).
    std::string id;
    rapidjson::Document d;
    d.Parse(resp.c_str(), resp.size());
    if (!d.HasParseError() && d.IsObject() && d.HasMember("id") &&
        d["id"].IsString()) {
      id.assign(d["id"].GetString(), d["id"].GetStringLength());
    }
    XELOGI("[logupload] uploaded {} bytes gz ({} src){} -> id={}", gz.size(),
           src, truncated ? " (tail)" : "", id.empty() ? "?" : id);
    SetStatus(State::kDone, id.empty()
                                ? "Log uploaded. Thank you!"
                                : ("Uploaded. Report id: " + id));
  } else {
    XELOGW("[logupload] failed: http={} curl={}", code, curl_easy_strerror(rc));
    std::string msg;
    switch (code) {
      case 413:
        msg = "Log is too large for the server.";
        break;
      case 429:
        msg = "Too many uploads right now -- try again later.";
        break;
      case 502:
      case 503:
        msg = "Log server is unavailable -- try again later.";
        break;
      case 0:
        msg = fmt::format("Couldn't reach the log server ({}).",
                          curl_easy_strerror(rc));
        break;
      default:
        msg = fmt::format("Upload failed (HTTP {}).", code);
        break;
    }
    SetStatus(State::kFailed, msg);
  }
  uploading_.store(false);
}

}  // namespace kernel
}  // namespace xe
