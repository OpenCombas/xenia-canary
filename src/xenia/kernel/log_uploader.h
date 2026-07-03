/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_LOG_UPLOADER_H_
#define XENIA_KERNEL_LOG_UPLOADER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace xe {
namespace kernel {

// One-click upload of the current Xenia log to a project-maintained ingest
// endpoint, so testers can submit diagnostic logs from the Netplay UI.
//
// Availability is SERVER-GATED: the client probes GET <sink> and only offers the
// upload when it returns OK, so the project can turn log collection on/off on
// demand (e.g. while chasing a bug) without any client build. The log is gzipped
// client-side and sent with Content-Encoding: gzip -- the payoff is the tester's
// constrained uplink; the ingest may stream it on as-is (blob storage) or
// decompress-and-forward (log indexer). Singleton.
class LogUploader {
 public:
  static LogUploader* Get();

  enum class State { kIdle, kUploading, kDone, kFailed };

  // Probe the sink (GET) on a detached worker if not already in flight / recently
  // checked. Cheap to call every frame while the diagnostics UI is open.
  void RefreshAvailability();
  bool available() const { return available_.load(); }

  // Upload the current log (gzip + metadata) on a detached worker. `note` is the
  // tester's free-text description. No-op if already uploading. Returns false on
  // a local precondition failure (no log file / not signed in).
  bool Upload(const std::string& note);

  State state() const { return state_.load(); }
  std::string status_message() const;  // report id/url, or an error

 private:
  LogUploader() = default;
  ~LogUploader() = default;

  void DoProbe();
  void DoUpload(std::string note);
  std::string SinkUrl() const;  // log_sink_url cvar, else <api>/logs
  void SetStatus(State state, const std::string& message);

  std::atomic<bool> available_{false};
  std::atomic<bool> probing_{false};
  std::atomic<int64_t> last_probe_ms_{0};
  std::atomic<bool> uploading_{false};
  std::atomic<State> state_{State::kIdle};
  mutable std::mutex msg_mutex_;
  std::string status_message_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_LOG_UPLOADER_H_
