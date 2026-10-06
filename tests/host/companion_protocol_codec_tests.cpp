#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/command.hpp"
#include "companion_protocol/config_status.hpp"
#include "companion_protocol/crc32.hpp"
#include "companion_protocol/device_info.hpp"
#include "companion_protocol/read_back.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace companion_protocol;

ByteView bytes_of(std::string_view text) {
  return ByteView{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

template <std::size_t N> std::vector<std::uint8_t> to_vector(const BoundedBytes<N> &value) {
  return std::vector<std::uint8_t>(value.view().begin(), value.view().end());
}

std::uint16_t u16_at(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes.at(offset) | (bytes.at(offset + 1) << 8U));
}

std::uint32_t u32_at(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(u16_at(bytes, offset)) |
         (static_cast<std::uint32_t>(u16_at(bytes, offset + 2)) << 16U);
}

} // namespace

TEST_CASE("crc32 of 123456789 is the CRC-32/ISO-HDLC check value 0xCBF43926") {
  CHECK(crc32(bytes_of("123456789")) == 0xCBF43926U);
}

TEST_CASE("crc32 of no bytes is zero") { CHECK(crc32(ByteView{}) == 0U); }

TEST_CASE("device info encodes the version 1 layout little-endian") {
  const auto encoded = encode_device_info(DeviceInfo{0x0102, true, "1.2.3", "weact-can485-v1.1"});
  REQUIRE(encoded.has_value());
  const auto bytes = to_vector(*encoded);
  const std::vector<std::uint8_t> header{1, 0, 0x02, 0x01, 2, 0x01, 0x00, 0x10, 5};
  REQUIRE(bytes.size() == 9 + 5 + 1 + 17);
  CHECK(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 9) == header);
  CHECK(std::string(bytes.begin() + 9, bytes.begin() + 14) == "1.2.3");
  CHECK(bytes.at(14) == 17);
  CHECK(std::string(bytes.begin() + 15, bytes.end()) == "weact-can485-v1.1");
}

TEST_CASE("device info flags bit 0 is clear when the pairing window is closed") {
  const auto encoded = encode_device_info(DeviceInfo{1, false, "", ""});
  REQUIRE(encoded.has_value());
  CHECK(to_vector(*encoded) == std::vector<std::uint8_t>{1, 0, 1, 0, 2, 0, 0x00, 0x10, 0, 0});
}

TEST_CASE("device info with two 31-byte strings is the 72-byte maximum") {
  const std::string text(31, 'x');
  const auto encoded = encode_device_info(DeviceInfo{1, false, text, text});
  REQUIRE(encoded.has_value());
  CHECK(encoded->size() == kMaxDeviceInfoBytes);
}

TEST_CASE("device info rejects a 32-byte string instead of truncating it") {
  const std::string text(32, 'x');
  CHECK_FALSE(encode_device_info(DeviceInfo{1, false, text, "id"}).has_value());
  CHECK_FALSE(encode_device_info(DeviceInfo{1, false, "1.0", text}).has_value());
}

TEST_CASE("command accepts each opcode with its check byte when idle") {
  const std::array<std::uint8_t, 2> revert{0x01, 0xFE};
  const std::array<std::uint8_t, 2> clear{0x02, 0xFD};
  const auto revert_decision = decode_command(revert, false);
  CHECK(revert_decision.error == AttError::None);
  CHECK(revert_decision.command == CommandOpcode::RevertToFactory);
  const auto clear_decision = decode_command(clear, false);
  CHECK(clear_decision.error == AttError::None);
  CHECK(clear_decision.command == CommandOpcode::ClearBonds);
}

TEST_CASE("command checks run in protocol order and never yield an opcode on failure") {
  const std::array<std::uint8_t, 1> short_write{0x01};
  const std::array<std::uint8_t, 3> long_write{0x01, 0xFE, 0x00};
  const std::array<std::uint8_t, 2> bad_check{0x09, 0x00};
  const std::array<std::uint8_t, 2> unknown{0x03, 0xFC};
  const std::array<std::uint8_t, 2> revert{0x01, 0xFE};

  CHECK(decode_command(ByteView{}, true).error == AttError::InvalidAttributeValueLength);
  CHECK(decode_command(short_write, true).error == AttError::InvalidAttributeValueLength);
  CHECK(decode_command(long_write, true).error == AttError::InvalidAttributeValueLength);
  // A wrong check byte wins over an unknown opcode and over Busy.
  CHECK(decode_command(bad_check, true).error == AttError::InvalidPdu);
  // An unknown opcode wins over Busy.
  CHECK(decode_command(unknown, true).error == AttError::UnsupportedOperation);
  const auto busy = decode_command(revert, true);
  CHECK(busy.error == AttError::Busy);
  CHECK_FALSE(busy.command.has_value());
  CHECK_FALSE(decode_command(bad_check, false).command.has_value());
}

TEST_CASE("diagnostic path keeps a short path unchanged") {
  const DiagnosticPath path{"outputs[3].zone.length"};
  CHECK(path.text() == "outputs[3].zone.length");
  CHECK_FALSE(path.truncated());
}

TEST_CASE("diagnostic path keeps exactly 26 bytes without truncation") {
  const std::string text(26, 'a');
  const DiagnosticPath path{text};
  CHECK(path.text() == text);
  CHECK_FALSE(path.truncated());
}

TEST_CASE("diagnostic path cuts a long path at a UTF-8 character boundary") {
  // 25 ASCII bytes, then a 2-byte character spanning bytes 25 and 26.
  const std::string text = std::string(25, 'a') + "\xC3\xA9" + "tail";
  const DiagnosticPath path{text};
  CHECK(path.text() == std::string(25, 'a'));
  CHECK(path.truncated());

  const std::string ascii(40, 'b');
  const DiagnosticPath cut{ascii};
  CHECK(cut.text() == std::string(26, 'b'));
  CHECK(cut.truncated());
}

TEST_CASE("apply section is derived from the stage") {
  ApplyRejection apply{};
  apply.validation_section = ConfigSection::Outputs;
  apply.stage = ApplyStage::ActionId;
  CHECK(apply.section() == ConfigSection::Actions);
  apply.stage = ApplyStage::OutputBinding;
  CHECK(apply.section() == ConfigSection::Outputs);
  apply.stage = ApplyStage::Rule;
  CHECK(apply.section() == ConfigSection::Rules);
  apply.stage = ApplyStage::SinkRegistration;
  CHECK(apply.section() == ConfigSection::Document);
  apply.stage = ApplyStage::Complete;
  CHECK(apply.section() == ConfigSection::Document);
  apply.stage = ApplyStage::Validation;
  CHECK(apply.section() == ConfigSection::Outputs);
  apply.validation_section = ConfigSection::Rules;
  CHECK(apply.section() == ConfigSection::Rules);
}

TEST_CASE("boot flags map to their bits") {
  CHECK(BootFlags{}.bits() == 0);
  CHECK(BootFlags{true, false, false, false}.bits() == 0x01);
  CHECK(BootFlags{false, true, false, false}.bits() == 0x02);
  CHECK(BootFlags{false, false, true, false}.bits() == 0x04);
  CHECK(BootFlags{false, false, false, true}.bits() == 0x08);
  CHECK(BootFlags{true, true, true, true}.bits() == 0x0F);
}

namespace {

constexpr std::string_view kFactoryDocument = R"({"schema_version":1})";

BootStatus factory_boot() {
  BootStatus boot{};
  const auto active = ActiveDocument::selected(ConfigSource::Factory, bytes_of(kFactoryDocument));
  REQUIRE(active.has_value());
  boot.active = *active;
  return boot;
}

// Every detail field populated, so tests can check which ones the encoder
// keeps for a given state and result.
TransferStatus populated_transfer(TransferState state, TransferResult result) {
  TransferStatus transfer{};
  transfer.state = state;
  transfer.result = result;
  transfer.transfer_length = 300;
  transfer.received_length = 120;
  transfer.saved = SavedDocument{0x0605, 0x11223344U};
  transfer.rejection.category = DiagnosticCategory::Semantic;
  transfer.rejection.code = DiagnosticCode::SchemaValidation;
  transfer.rejection.validation = ValidationCode::ZoneOutOfRange;
  transfer.rejection.index = 3;
  transfer.rejection.path = DiagnosticPath{"outputs[3].zone.length"};
  transfer.apply.stage = ApplyStage::Rule;
  transfer.apply.validation = ValidationCode::InvalidOperand;
  transfer.apply.validation_section = ConfigSection::Outputs;
  transfer.apply.index = 7;
  transfer.apply.binding = BindingCode::CapacityExceeded;
  transfer.apply.engine = EngineConfigCode::UnknownChoice;
  return transfer;
}

bool all_zero(const std::vector<std::uint8_t> &bytes, std::size_t first, std::size_t last) {
  for (std::size_t index = first; index < last; ++index) {
    if (bytes.at(index) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("config status for an idle boot is 35 bytes with only the boot fields") {
  const auto bytes = to_vector(encode_config_status(factory_boot(), TransferStatus{}));
  REQUIRE(bytes.size() == kConfigStatusFixedBytes);
  CHECK(bytes.at(0) == 0);
  CHECK(bytes.at(1) == 0);
  CHECK(bytes.at(2) == 0);
  CHECK(bytes.at(3) == 0);
  CHECK(u16_at(bytes, 4) == kFactoryDocument.size());
  CHECK(u32_at(bytes, 6) == crc32(bytes_of(kFactoryDocument)));
  CHECK(all_zero(bytes, 10, 35));
}

TEST_CASE("config status reports transfer lengths only while receiving") {
  const auto receiving = to_vector(encode_config_status(
      factory_boot(), populated_transfer(TransferState::Receiving, TransferResult::None)));
  CHECK(receiving.at(0) == 1);
  CHECK(u16_at(receiving, 10) == 300);
  CHECK(u16_at(receiving, 12) == 120);
  CHECK(all_zero(receiving, 14, 35));

  const auto idle = to_vector(encode_config_status(
      factory_boot(), populated_transfer(TransferState::Idle, TransferResult::Aborted)));
  CHECK(idle.at(1) == 5);
  CHECK(all_zero(idle, 10, 35));
}

TEST_CASE("config status reports the saved document only for Saved") {
  const auto bytes = to_vector(encode_config_status(
      factory_boot(), populated_transfer(TransferState::RestartPending, TransferResult::Saved)));
  CHECK(bytes.at(0) == 2);
  CHECK(bytes.at(1) == 1);
  CHECK(all_zero(bytes, 10, 14));
  CHECK(u16_at(bytes, 14) == 0x0605);
  CHECK(u32_at(bytes, 16) == 0x11223344U);
  CHECK(all_zero(bytes, 20, 35));
}

TEST_CASE("config status reports the diagnostic and path only for ConfigRejected") {
  const auto bytes = to_vector(encode_config_status(
      factory_boot(), populated_transfer(TransferState::Idle, TransferResult::ConfigRejected)));
  const std::string_view path = "outputs[3].zone.length";
  REQUIRE(bytes.size() == kConfigStatusFixedBytes + path.size());
  CHECK(bytes.at(1) == 2);
  CHECK(all_zero(bytes, 10, 20));
  CHECK(bytes.at(20) == 3);
  CHECK(bytes.at(21) == 10);
  CHECK(bytes.at(22) == 18);
  CHECK(u16_at(bytes, 23) == 3);
  CHECK(all_zero(bytes, 25, 34));
  CHECK(bytes.at(34) == path.size());
  CHECK(std::string(bytes.begin() + 35, bytes.end()) == path);
}

TEST_CASE("config status marks a truncated path with bit 7 of path_info") {
  auto transfer = populated_transfer(TransferState::Idle, TransferResult::ConfigRejected);
  transfer.rejection.path = DiagnosticPath{std::string(40, 'p')};
  const auto bytes = to_vector(encode_config_status(factory_boot(), transfer));
  REQUIRE(bytes.size() == kMaxConfigStatusBytes);
  CHECK(bytes.at(34) == (0x80 | 26));
}

TEST_CASE("config status reports the apply fields only for ApplyRejected") {
  const auto bytes = to_vector(encode_config_status(
      factory_boot(), populated_transfer(TransferState::Idle, TransferResult::ApplyRejected)));
  REQUIRE(bytes.size() == kConfigStatusFixedBytes);
  CHECK(bytes.at(1) == 3);
  CHECK(all_zero(bytes, 10, 25));
  CHECK(bytes.at(25) == 5);
  CHECK(bytes.at(26) == 11);
  CHECK(bytes.at(27) == 2); // Rule stage gives the Rules section.
  CHECK(u16_at(bytes, 28) == 7);
  CHECK(bytes.at(30) == 3);
  CHECK(bytes.at(31) == 9);
  CHECK(all_zero(bytes, 32, 35));
}

TEST_CASE("config status zeroes every detail for results without one") {
  for (const auto result :
       {TransferResult::None, TransferResult::StorageFailed, TransferResult::Aborted,
        TransferResult::TimedOut, TransferResult::ChecksumMismatch, TransferResult::Interrupted}) {
    const auto bytes = to_vector(
        encode_config_status(factory_boot(), populated_transfer(TransferState::Idle, result)));
    CHECK(bytes.at(1) == static_cast<std::uint8_t>(result));
    CHECK(bytes.size() == kConfigStatusFixedBytes);
    CHECK(all_zero(bytes, 10, 35));
  }
}

TEST_CASE("config status saturates indexes at 0xFFFF") {
  auto rejected = populated_transfer(TransferState::Idle, TransferResult::ConfigRejected);
  rejected.rejection.index = 70'000;
  CHECK(u16_at(to_vector(encode_config_status(factory_boot(), rejected)), 23) == 0xFFFF);

  auto apply = populated_transfer(TransferState::Idle, TransferResult::ApplyRejected);
  apply.apply.index = 0x10000;
  CHECK(u16_at(to_vector(encode_config_status(factory_boot(), apply)), 28) == 0xFFFF);
  apply.apply.index = 0xFFFE;
  CHECK(u16_at(to_vector(encode_config_status(factory_boot(), apply)), 28) == 0xFFFE);
}

TEST_CASE("config status reports the boot diagnostic only with boot flag 0") {
  BootStatus boot = factory_boot();
  boot.override_code = DiagnosticCode::MissingField;
  boot.override_validation = ValidationCode::InvalidColor;
  boot.flags.override_read_failed = true;
  boot.flags.lighting_setup_failed = true;
  const auto without = to_vector(encode_config_status(boot, TransferStatus{}));
  CHECK(without.at(2) == 0x0A);
  CHECK(without.at(32) == 0);
  CHECK(without.at(33) == 0);

  boot.flags.invalid_override = true;
  const auto with = to_vector(encode_config_status(boot, TransferStatus{}));
  CHECK(with.at(2) == 0x0B);
  CHECK(with.at(32) == 6);
  CHECK(with.at(33) == 19);
}

TEST_CASE("config status reports no active length or CRC without an active source") {
  BootStatus boot = factory_boot();
  boot.active = ActiveDocument{};
  const auto bytes = to_vector(encode_config_status(boot, TransferStatus{}));
  CHECK(bytes.at(3) == 0xFF);
  CHECK(all_zero(bytes, 4, 10));
}

TEST_CASE("wire code tables keep their protocol values") {
  CHECK(static_cast<int>(TransferResult::Interrupted) == 8);
  CHECK(static_cast<int>(DiagnosticCode::ResourceExhausted) == 11);
  CHECK(static_cast<int>(ValidationCode::IncompatibleActionKind) == 23);
  CHECK(static_cast<int>(ConfigSection::Outputs) == 3);
  CHECK(static_cast<int>(ApplyStage::Rule) == 5);
  CHECK(static_cast<int>(BindingCode::InvalidEffect) == 4);
  CHECK(static_cast<int>(EngineConfigCode::InvalidHysteresis) == 13);
  CHECK(static_cast<int>(AttError::ApplyRejected) == 0x8B);
  CHECK(static_cast<int>(AttError::MtuTooSmall) == 0x85);
}

namespace {

std::string document_of(std::size_t length) {
  std::string text;
  for (std::size_t index = 0; index < length; ++index) {
    text.push_back(static_cast<char>('a' + index % 26));
  }
  return text;
}

} // namespace

TEST_CASE("read-back without an active config is a 9-byte page with source 0xFF") {
  const auto page = encode_read_back_page(ActiveDocument{}, 0);
  REQUIRE(page.has_value());
  CHECK(to_vector(*page) == std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0, 0, 0, 0, 0});
  CHECK_FALSE(encode_read_back_page(ActiveDocument{}, 1).has_value());
  CHECK_FALSE(ActiveDocument::selected(ConfigSource::None, bytes_of("{}")).has_value());
}

TEST_CASE("read-back pages carry the header and up to 200 data bytes") {
  const std::string text = document_of(450);
  const auto document = ActiveDocument::selected(ConfigSource::Override, bytes_of(text));
  REQUIRE(document.has_value());
  CHECK(document->crc32() == crc32(bytes_of(text)));

  const auto first = to_vector(*encode_read_back_page(*document, 0));
  REQUIRE(first.size() == kMaxReadBackPageBytes);
  CHECK(first.at(0) == 1);
  CHECK(u16_at(first, 1) == 450);
  CHECK(u32_at(first, 3) == crc32(bytes_of(text)));
  CHECK(u16_at(first, 7) == 0);
  CHECK(std::string(first.begin() + 9, first.end()) == text.substr(0, 200));

  const auto last = to_vector(*encode_read_back_page(*document, 400));
  REQUIRE(last.size() == 9 + 50);
  CHECK(u16_at(last, 7) == 400);
  CHECK(std::string(last.begin() + 9, last.end()) == text.substr(400));

  const auto end = to_vector(*encode_read_back_page(*document, 450));
  CHECK(end.size() == 9);
  CHECK_FALSE(encode_read_back_page(*document, 451).has_value());
}

TEST_CASE("read-back rejects a document longer than 65535 bytes") {
  const std::string text(0x10000, 'x');
  CHECK_FALSE(ActiveDocument::selected(ConfigSource::Factory, bytes_of(text)).has_value());
}
