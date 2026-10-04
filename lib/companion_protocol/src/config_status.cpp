#include "companion_protocol/config_status.hpp"

namespace companion_protocol {

namespace {

constexpr std::uint8_t kPathTruncatedBit = 0x80;

// True when `byte` continues a UTF-8 multi-byte character (10xxxxxx).
constexpr bool continuation_byte(char byte) noexcept {
  return (static_cast<std::uint8_t>(byte) & 0xC0U) == 0x80U;
}

std::uint16_t saturated_index(std::size_t index) noexcept {
  return index > 0xFFFFU ? std::uint16_t{0xFFFF} : static_cast<std::uint16_t>(index);
}

std::uint8_t code(TransferState value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(TransferResult value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(ConfigSource value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(DiagnosticCategory value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(DiagnosticCode value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(ValidationCode value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(ConfigSection value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(ApplyStage value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(BindingCode value) noexcept { return static_cast<std::uint8_t>(value); }
std::uint8_t code(EngineConfigCode value) noexcept { return static_cast<std::uint8_t>(value); }

void push_zeros(EncodedConfigStatus &value, std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    value.push(0);
  }
}

// Offsets 2-9: boot flags and the active document.
void push_boot(EncodedConfigStatus &value, const BootStatus &boot) noexcept {
  value.push(boot.flags.bits());
  // A boot with no active document reports source None, length 0 and CRC 0.
  value.push(code(boot.active.source()));
  value.push_u16(boot.active.length());
  value.push_u32(boot.active.crc32());
}

// Offsets 10-13.
void push_lengths(EncodedConfigStatus &value, const TransferStatus &transfer) noexcept {
  if (transfer.state != TransferState::Receiving) {
    push_zeros(value, 4);
    return;
  }
  value.push_u16(transfer.transfer_length);
  value.push_u16(transfer.received_length);
}

// Offsets 14-19.
void push_saved(EncodedConfigStatus &value, const TransferStatus &transfer) noexcept {
  if (transfer.result != TransferResult::Saved) {
    push_zeros(value, 6);
    return;
  }
  value.push_u16(transfer.saved.length);
  value.push_u32(transfer.saved.crc32);
}

// Offsets 20-24.
void push_rejection(EncodedConfigStatus &value, const TransferStatus &transfer) noexcept {
  if (transfer.result != TransferResult::ConfigRejected) {
    push_zeros(value, 5);
    return;
  }
  const ConfigRejection &rejection = transfer.rejection;
  value.push(code(rejection.category));
  value.push(code(rejection.code));
  value.push(code(rejection.validation));
  value.push_u16(saturated_index(rejection.index));
}

// Offsets 25-31.
void push_apply(EncodedConfigStatus &value, const TransferStatus &transfer) noexcept {
  if (transfer.result != TransferResult::ApplyRejected) {
    push_zeros(value, 7);
    return;
  }
  const ApplyRejection &apply = transfer.apply;
  value.push(code(apply.stage));
  value.push(code(apply.validation));
  value.push(code(apply.section()));
  value.push_u16(saturated_index(apply.index));
  value.push(code(apply.binding));
  value.push(code(apply.engine));
}

// Offsets 32-33.
void push_boot_diagnostic(EncodedConfigStatus &value, const BootStatus &boot) noexcept {
  if (!boot.flags.invalid_override) {
    push_zeros(value, 2);
    return;
  }
  value.push(code(boot.override_code));
  value.push(code(boot.override_validation));
}

// Offset 34 and the path.
void push_path(EncodedConfigStatus &value, const TransferStatus &transfer) noexcept {
  if (transfer.result != TransferResult::ConfigRejected) {
    value.push(0);
    return;
  }
  const DiagnosticPath &path = transfer.rejection.path;
  const std::string_view text = path.text();
  const auto length = static_cast<std::uint8_t>(text.size());
  value.push(path.truncated() ? static_cast<std::uint8_t>(length | kPathTruncatedBit) : length);
  value.append(ByteView{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

} // namespace

DiagnosticPath::DiagnosticPath(std::string_view path) noexcept {
  std::size_t length = path.size();
  if (length > kMaxDiagnosticPathBytes) {
    truncated_ = true;
    length = kMaxDiagnosticPathBytes;
    // Back up to the lead byte of the character the cut falls inside.
    while (length > 0 && continuation_byte(path[length])) {
      --length;
    }
  }
  for (std::size_t index = 0; index < length; ++index) {
    bytes_[index] = path[index];
  }
  length_ = length;
}

ConfigSection ApplyRejection::section() const noexcept {
  switch (stage) {
  case ApplyStage::ActionId:
    return ConfigSection::Actions;
  case ApplyStage::OutputBinding:
    return ConfigSection::Outputs;
  case ApplyStage::Rule:
    return ConfigSection::Rules;
  case ApplyStage::Validation:
    return validation_section;
  default:
    return ConfigSection::Document;
  }
}

std::uint8_t BootFlags::bits() const noexcept {
  return static_cast<std::uint8_t>(
      (invalid_override ? 0x01U : 0U) | (override_read_failed ? 0x02U : 0U) |
      (no_config_store ? 0x04U : 0U) | (lighting_setup_failed ? 0x08U : 0U));
}

EncodedConfigStatus encode_config_status(const BootStatus &boot,
                                         const TransferStatus &transfer) noexcept {
  EncodedConfigStatus value{};
  value.push(code(transfer.state));
  value.push(code(transfer.result));
  push_boot(value, boot);
  push_lengths(value, transfer);
  push_saved(value, transfer);
  push_rejection(value, transfer);
  push_apply(value, transfer);
  push_boot_diagnostic(value, boot);
  push_path(value, transfer);
  return value;
}

} // namespace companion_protocol
