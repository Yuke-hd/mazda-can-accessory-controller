#include "companion_protocol/command.hpp"

namespace companion_protocol {

namespace {

constexpr std::size_t kCommandBytes = 2;

std::optional<CommandOpcode> known_opcode(std::uint8_t opcode) noexcept {
  switch (opcode) {
  case static_cast<std::uint8_t>(CommandOpcode::RevertToFactory):
    return CommandOpcode::RevertToFactory;
  case static_cast<std::uint8_t>(CommandOpcode::ClearBonds):
    return CommandOpcode::ClearBonds;
  default:
    return std::nullopt;
  }
}

CommandDecision rejected(AttError error) noexcept { return CommandDecision{error, std::nullopt}; }

} // namespace

CommandDecision decode_command(ByteView pdu, bool operation_in_progress) noexcept {
  if (pdu.size() != kCommandBytes) {
    return rejected(AttError::InvalidAttributeValueLength);
  }
  if (pdu[1] != static_cast<std::uint8_t>(pdu[0] ^ 0xFFU)) {
    return rejected(AttError::InvalidPdu);
  }
  const auto command = known_opcode(pdu[0]);
  if (!command.has_value()) {
    return rejected(AttError::UnsupportedOperation);
  }
  if (operation_in_progress) {
    return rejected(AttError::Busy);
  }
  return CommandDecision{AttError::None, command};
}

} // namespace companion_protocol
