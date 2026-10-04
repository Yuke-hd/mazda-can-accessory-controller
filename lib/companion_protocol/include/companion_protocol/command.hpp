#pragma once

#include <cstdint>
#include <optional>

#include "companion_protocol/att_error.hpp"
#include "companion_protocol/bytes.hpp"

namespace companion_protocol {

enum class CommandOpcode : std::uint8_t {
  RevertToFactory = 0x01,
  ClearBonds = 0x02,
};

// The decoded Command write: either an accepted opcode with AttError::None,
// or the ATT error that rejects the write and no opcode.
struct CommandDecision {
  AttError error{AttError::InvalidAttributeValueLength};
  std::optional<CommandOpcode> command{};
};

// Applies the Command checks in protocol order: length, check byte, opcode,
// then Busy while a config transfer, commit or restart is in progress
// (`operation_in_progress`, normally ConfigTransfer::check_busy(now).busy). Decoding never
// performs the command; the caller does that after an accepted decision.
[[nodiscard]] CommandDecision decode_command(ByteView pdu, bool operation_in_progress) noexcept;

} // namespace companion_protocol
