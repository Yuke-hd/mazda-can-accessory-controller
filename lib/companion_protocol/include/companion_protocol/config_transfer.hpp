#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#include "companion_protocol/active_document.hpp"
#include "companion_protocol/att_error.hpp"
#include "companion_protocol/bytes.hpp"
#include "companion_protocol/config_status.hpp"
#include "companion_protocol/device_info.hpp"

namespace companion_protocol {

// Monotonic time on the BLE host context. Only differences are used.
using TransferClock = std::chrono::milliseconds;

// No Start or accepted Chunk for this long discards an open transfer.
inline constexpr TransferClock kTransferIdleTimeout{10'000};

// What the commit port reports for a CRC-verified document. Only the four
// outcomes a commit can end with are constructible.
class CommitOutcome final {
public:
  // save_override() returned Saved; the canonical length and CRC.
  [[nodiscard]] static CommitOutcome saved(SavedDocument document) noexcept;
  // Parse or validate failed, or save_override() returned InvalidCandidate.
  [[nodiscard]] static CommitOutcome config_rejected(const ConfigRejection &rejection) noexcept;
  // The dry-run apply returned any stage other than Complete.
  [[nodiscard]] static CommitOutcome apply_rejected(const ApplyRejection &rejection) noexcept;
  // save_override() returned Failed. No diagnostic is reported.
  [[nodiscard]] static CommitOutcome storage_failed() noexcept;

  [[nodiscard]] TransferResult result() const noexcept { return result_; }
  [[nodiscard]] const SavedDocument &saved_document() const noexcept { return saved_; }
  [[nodiscard]] const ConfigRejection &rejection() const noexcept { return rejection_; }
  [[nodiscard]] const ApplyRejection &apply() const noexcept { return apply_; }

private:
  explicit CommitOutcome(TransferResult result) noexcept : result_(result) {}

  TransferResult result_;
  SavedDocument saved_{};
  ConfigRejection rejection_{};
  ApplyRejection apply_{};
};

// Commit port injected by the integration. It receives the complete,
// CRC-verified upload and performs commit steps 4 to 6: parse and validate,
// the dry-run apply on scratch objects, and save_override(). The codec itself
// never parses, applies or persists a config; it only reports the outcome.
// The document view is valid only for the duration of the call.
class ConfigCommitter {
public:
  ConfigCommitter(const ConfigCommitter &) = delete;
  ConfigCommitter &operator=(const ConfigCommitter &) = delete;

  [[nodiscard]] virtual CommitOutcome commit(ByteView document) noexcept = 0;

protected:
  ConfigCommitter() noexcept = default;
  ~ConfigCommitter() = default;
};

// Facts about this boot that the write checks need.
struct ConfigTransferEnvironment {
  // False when this boot has no config store; Start then fails with
  // StorageFailure.
  bool config_store_available{false};
  // The document read-back serves. Select read page accepts offsets up to its
  // length; pass the same value Config status reports in BootStatus::active.
  ActiveDocument active{};
};

// Per-write facts from the BLE stack.
struct ConfigWriteContext {
  std::uint16_t att_mtu{0};
  TransferClock now{0};
};

struct ConfigWriteResponse {
  AttError error{AttError::None};
  // True when the status `state` or `result` changed, so Config status must
  // be notified.
  bool status_changed{false};
  // Set by an accepted Select read page: the connection's new page offset.
  std::optional<std::uint16_t> read_page_offset{};
};

// The answer to ConfigTransfer::check_busy().
struct BusyCheck {
  bool busy{false};
  // True when the check discarded an expired transfer, so Config status must
  // be notified.
  bool status_changed{false};
};

// Config characteristic write handling: the chunked transfer state machine
// (Start, Chunk, Commit, Abort, Select read page), the idle timeout and the
// disconnect interruption. It holds at most one transfer, in a fixed
// 4096-byte buffer, across all connections. Writes, timeouts and interrupts
// must come from one context, normally the BLE host task.
class ConfigTransfer final {
public:
  ConfigTransfer(ConfigCommitter &committer, ConfigTransferEnvironment environment) noexcept;
  ConfigTransfer(const ConfigTransfer &) = delete;
  ConfigTransfer &operator=(const ConfigTransfer &) = delete;

  // Handles one Config write PDU. An open transfer that has been idle for
  // kTransferIdleTimeout at `context.now` is discarded first.
  [[nodiscard]] ConfigWriteResponse handle_write(ByteView pdu,
                                                 const ConfigWriteContext &context) noexcept;
  // Discards an open transfer idle for kTransferIdleTimeout with TimedOut.
  // Returns true when the status changed.
  bool expire_if_idle(TransferClock now) noexcept;
  // The connection ended: discards an open transfer with Interrupted.
  // Returns true when the status changed.
  bool interrupt() noexcept;
  // An accepted Revert to factory: no writes are accepted until the restart.
  // Fails, changing nothing, while a transfer is open.
  bool enter_restart_pending() noexcept;

  // Whether a Command written at `now` must be refused as Busy: a transfer
  // is open or a restart is pending. An open transfer idle for
  // kTransferIdleTimeout is discarded first with TimedOut, so an expired
  // transfer never makes a Command Busy.
  [[nodiscard]] BusyCheck check_busy(TransferClock now) noexcept;
  [[nodiscard]] bool restart_pending() const noexcept {
    return status_.state == TransferState::RestartPending;
  }
  [[nodiscard]] const TransferStatus &status() const noexcept { return status_; }

private:
  struct Upload {
    std::uint32_t crc32{0};
    TransferClock last_activity{0};
    std::array<std::uint8_t, kMaxConfigBytes> buffer{};
  };

  AttError dispatch(ByteView pdu, const ConfigWriteContext &context,
                    ConfigWriteResponse &response) noexcept;
  AttError start(ByteView pdu, const ConfigWriteContext &context) noexcept;
  AttError chunk(ByteView pdu, const ConfigWriteContext &context) noexcept;
  AttError commit() noexcept;
  AttError abort() noexcept;
  AttError select_read_page(ByteView pdu, ConfigWriteResponse &response) const noexcept;
  void finish(TransferResult result) noexcept;
  [[nodiscard]] bool receiving() const noexcept {
    return status_.state == TransferState::Receiving;
  }

  ConfigCommitter &committer_;
  ConfigTransferEnvironment environment_;
  TransferStatus status_{};
  Upload upload_{};
};

} // namespace companion_protocol
