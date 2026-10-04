#include "companion_protocol/config_transfer.hpp"

#include <cstddef>

#include "companion_protocol/crc32.hpp"

namespace companion_protocol {

namespace {

enum class Opcode : std::uint8_t {
  Start = 0x01,
  Chunk = 0x02,
  Commit = 0x03,
  Abort = 0x04,
  SelectReadPage = 0x05,
};

// ATT Write Request header bytes that the MTU also has to carry.
constexpr int kWriteHeaderBytes = 3;
constexpr std::size_t kChunkHeaderBytes = 3;
constexpr std::size_t kMinimumChunkBytes = kChunkHeaderBytes + 1;

std::uint16_t read_u16(ByteView pdu, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(pdu[offset] | (pdu[offset + 1] << 8U));
}

std::uint32_t read_u32(ByteView pdu, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(read_u16(pdu, offset)) |
         (static_cast<std::uint32_t>(read_u16(pdu, offset + 2)) << 16U);
}

bool known_opcode(std::uint8_t opcode) noexcept {
  return opcode >= static_cast<std::uint8_t>(Opcode::Start) &&
         opcode <= static_cast<std::uint8_t>(Opcode::SelectReadPage);
}

// The length range of each opcode. A chunk is 4..MTU - 3 bytes; with an MTU
// below 7 no chunk length is valid.
bool valid_length(Opcode opcode, std::size_t length, std::uint16_t att_mtu) noexcept {
  switch (opcode) {
  case Opcode::Start:
    return length == 7;
  case Opcode::Chunk: {
    const int largest = static_cast<int>(att_mtu) - kWriteHeaderBytes;
    return length >= kMinimumChunkBytes && static_cast<int>(length) <= largest;
  }
  case Opcode::Commit:
  case Opcode::Abort:
    return length == 1;
  case Opcode::SelectReadPage:
    return length == 3;
  }
  return false;
}

} // namespace

CommitOutcome CommitOutcome::saved(SavedDocument document) noexcept {
  CommitOutcome outcome{TransferResult::Saved};
  outcome.saved_ = document;
  return outcome;
}

CommitOutcome CommitOutcome::config_rejected(const ConfigRejection &rejection) noexcept {
  CommitOutcome outcome{TransferResult::ConfigRejected};
  outcome.rejection_ = rejection;
  return outcome;
}

CommitOutcome CommitOutcome::apply_rejected(const ApplyRejection &rejection) noexcept {
  CommitOutcome outcome{TransferResult::ApplyRejected};
  outcome.apply_ = rejection;
  return outcome;
}

CommitOutcome CommitOutcome::storage_failed() noexcept {
  return CommitOutcome{TransferResult::StorageFailed};
}

ConfigTransfer::ConfigTransfer(ConfigCommitter &committer,
                               ConfigTransferEnvironment environment) noexcept
    : committer_(committer), environment_(environment) {}

ConfigWriteResponse ConfigTransfer::handle_write(ByteView pdu,
                                                 const ConfigWriteContext &context) noexcept {
  const TransferState state_before = status_.state;
  const TransferResult result_before = status_.result;
  expire_if_idle(context.now);

  ConfigWriteResponse response{};
  response.error = dispatch(pdu, context, response);
  response.status_changed = status_.state != state_before || status_.result != result_before;
  return response;
}

bool ConfigTransfer::expire_if_idle(TransferClock now) noexcept {
  if (!receiving() || now - upload_.last_activity < kTransferIdleTimeout) {
    return false;
  }
  finish(TransferResult::TimedOut);
  return true;
}

BusyCheck ConfigTransfer::check_busy(TransferClock now) noexcept {
  BusyCheck check{};
  check.status_changed = expire_if_idle(now);
  check.busy = status_.state != TransferState::Idle;
  return check;
}

bool ConfigTransfer::interrupt() noexcept {
  if (!receiving()) {
    return false;
  }
  finish(TransferResult::Interrupted);
  return true;
}

bool ConfigTransfer::enter_restart_pending() noexcept {
  if (status_.state != TransferState::Idle) {
    return false;
  }
  status_.state = TransferState::RestartPending;
  return true;
}

AttError ConfigTransfer::dispatch(ByteView pdu, const ConfigWriteContext &context,
                                  ConfigWriteResponse &response) noexcept {
  if (pdu.empty()) {
    return AttError::InvalidAttributeValueLength;
  }
  if (!known_opcode(pdu[0])) {
    return AttError::UnsupportedOperation;
  }
  const auto opcode = static_cast<Opcode>(pdu[0]);
  if (!valid_length(opcode, pdu.size(), context.att_mtu)) {
    return AttError::InvalidAttributeValueLength;
  }
  if (restart_pending()) {
    return AttError::Busy;
  }
  switch (opcode) {
  case Opcode::Start:
    return start(pdu, context);
  case Opcode::Chunk:
    return chunk(pdu, context);
  case Opcode::Commit:
    return commit();
  case Opcode::Abort:
    return abort();
  case Opcode::SelectReadPage:
    return select_read_page(pdu, response);
  }
  return AttError::UnsupportedOperation;
}

AttError ConfigTransfer::start(ByteView pdu, const ConfigWriteContext &context) noexcept {
  const std::uint16_t total_length = read_u16(pdu, 1);
  if (context.att_mtu < kMinimumTransferMtu) {
    return AttError::MtuTooSmall;
  }
  if (total_length == 0) {
    return AttError::InvalidPdu;
  }
  if (total_length > kMaxConfigBytes) {
    return AttError::TooLarge;
  }
  if (receiving()) {
    return AttError::Busy;
  }
  if (!environment_.config_store_available) {
    return AttError::StorageFailure;
  }
  status_ = TransferStatus{};
  status_.state = TransferState::Receiving;
  status_.transfer_length = total_length;
  upload_.crc32 = read_u32(pdu, 3);
  upload_.last_activity = context.now;
  return AttError::None;
}

AttError ConfigTransfer::chunk(ByteView pdu, const ConfigWriteContext &context) noexcept {
  if (!receiving()) {
    return AttError::InvalidState;
  }
  if (read_u16(pdu, 1) != status_.received_length) {
    return AttError::OffsetMismatch;
  }
  const ByteView data = pdu.from(kChunkHeaderBytes);
  const std::size_t end = status_.received_length + data.size();
  if (end > status_.transfer_length) {
    return AttError::InvalidPdu;
  }
  std::size_t position = status_.received_length;
  for (const std::uint8_t byte : data) {
    upload_.buffer[position++] = byte;
  }
  status_.received_length = static_cast<std::uint16_t>(end);
  upload_.last_activity = context.now;
  return AttError::None;
}

AttError ConfigTransfer::commit() noexcept {
  if (!receiving()) {
    return AttError::InvalidState;
  }
  if (status_.received_length < status_.transfer_length) {
    return AttError::Incomplete;
  }
  const ByteView document{upload_.buffer.data(), status_.transfer_length};
  if (crc32(document) != upload_.crc32) {
    finish(TransferResult::ChecksumMismatch);
    return AttError::ChecksumMismatch;
  }
  const CommitOutcome outcome = committer_.commit(document);
  switch (outcome.result()) {
  case TransferResult::Saved:
    finish(TransferResult::Saved);
    status_.state = TransferState::RestartPending;
    status_.saved = outcome.saved_document();
    return AttError::None;
  case TransferResult::ConfigRejected:
    finish(TransferResult::ConfigRejected);
    status_.rejection = outcome.rejection();
    return AttError::ConfigRejected;
  case TransferResult::ApplyRejected:
    finish(TransferResult::ApplyRejected);
    status_.apply = outcome.apply();
    return AttError::ApplyRejected;
  default:
    finish(TransferResult::StorageFailed);
    return AttError::StorageFailure;
  }
}

AttError ConfigTransfer::abort() noexcept {
  if (receiving()) {
    finish(TransferResult::Aborted);
  }
  return AttError::None;
}

AttError ConfigTransfer::select_read_page(ByteView pdu,
                                          ConfigWriteResponse &response) const noexcept {
  const std::uint16_t offset = read_u16(pdu, 1);
  if (offset > environment_.active.length()) {
    return AttError::InvalidPdu;
  }
  response.read_page_offset = offset;
  return AttError::None;
}

void ConfigTransfer::finish(TransferResult result) noexcept {
  status_ = TransferStatus{};
  status_.result = result;
  upload_.crc32 = 0;
}

} // namespace companion_protocol
