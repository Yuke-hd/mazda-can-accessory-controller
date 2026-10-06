#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/crc32.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace companion_protocol;
using namespace std::chrono_literals;

constexpr std::uint16_t kMtu = 247;

// Records each commit request and answers with a scripted outcome. It never
// parses or stores anything; the transfer must not either.
class ScriptedCommitter final : public ConfigCommitter {
public:
  CommitOutcome commit(ByteView document) noexcept override {
    ++calls;
    received.assign(document.begin(), document.end());
    return outcome;
  }

  CommitOutcome outcome = CommitOutcome::saved(SavedDocument{12, 0xCAFEF00DU});
  std::size_t calls{0};
  std::vector<std::uint8_t> received{};
};

std::vector<std::uint8_t> document_of(std::size_t length) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t index = 0; index < length; ++index) {
    bytes.push_back(static_cast<std::uint8_t>('a' + index % 26));
  }
  return bytes;
}

void push_u16(std::vector<std::uint8_t> &pdu, std::size_t value) {
  pdu.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  pdu.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

std::vector<std::uint8_t> start_pdu(std::size_t length, std::uint32_t crc) {
  std::vector<std::uint8_t> pdu{0x01};
  push_u16(pdu, length);
  push_u16(pdu, crc & 0xFFFFU);
  push_u16(pdu, crc >> 16U);
  return pdu;
}

std::vector<std::uint8_t> chunk_pdu(std::size_t offset, const std::vector<std::uint8_t> &data,
                                    std::size_t first, std::size_t count) {
  std::vector<std::uint8_t> pdu{0x02};
  push_u16(pdu, offset);
  pdu.insert(pdu.end(), data.begin() + static_cast<std::ptrdiff_t>(first),
             data.begin() + static_cast<std::ptrdiff_t>(first + count));
  return pdu;
}

const std::vector<std::uint8_t> kCommit{0x03};
const std::vector<std::uint8_t> kAbort{0x04};

std::vector<std::uint8_t> select_pdu(std::size_t offset) {
  std::vector<std::uint8_t> pdu{0x05};
  push_u16(pdu, offset);
  return pdu;
}

ByteView view(const std::vector<std::uint8_t> &bytes) {
  return ByteView{bytes.data(), bytes.size()};
}

// A 450-byte active override document. The bytes have static lifetime, as the
// integration's canonical serialization does.
ActiveDocument active_450() {
  static const std::vector<std::uint8_t> canonical = document_of(450);
  const auto document = ActiveDocument::selected(ConfigSource::Override, view(canonical));
  REQUIRE(document.has_value());
  return *document;
}

struct Fixture {
  explicit Fixture(ConfigTransferEnvironment environment = {true, ActiveDocument{}})
      : transfer(std::make_unique<ConfigTransfer>(committer, environment)) {}

  ConfigWriteResponse write(const std::vector<std::uint8_t> &pdu, std::uint16_t mtu = kMtu) {
    return transfer->handle_write(view(pdu), ConfigWriteContext{mtu, now});
  }
  // Opens a transfer of `document` and sends it in chunks of `chunk_size`.
  void upload(const std::vector<std::uint8_t> &document, std::size_t chunk_size = 200) {
    REQUIRE(write(start_pdu(document.size(), crc32(view(document)))).error == AttError::None);
    for (std::size_t offset = 0; offset < document.size(); offset += chunk_size) {
      const std::size_t count = std::min(chunk_size, document.size() - offset);
      REQUIRE(write(chunk_pdu(offset, document, offset, count)).error == AttError::None);
    }
  }
  const TransferStatus &status() const { return transfer->status(); }

  ScriptedCommitter committer{};
  std::unique_ptr<ConfigTransfer> transfer;
  TransferClock now{1'000};
};

} // namespace

TEST_CASE("a transfer starts idle with no result") {
  Fixture fixture;
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::None);
  CHECK_FALSE(fixture.transfer->check_busy(fixture.now).busy);
}

TEST_CASE("a valid upload in order commits once and becomes RestartPending") {
  Fixture fixture;
  const auto document = document_of(1000);
  fixture.upload(document, 241);
  CHECK(fixture.status().state == TransferState::Receiving);
  CHECK(fixture.status().transfer_length == 1000);
  CHECK(fixture.status().received_length == 1000);

  const auto response = fixture.write(kCommit);
  CHECK(response.error == AttError::None);
  CHECK(response.status_changed);
  CHECK(fixture.committer.calls == 1);
  CHECK(fixture.committer.received == document);
  CHECK(fixture.status().state == TransferState::RestartPending);
  CHECK(fixture.status().result == TransferResult::Saved);
  CHECK(fixture.status().saved.length == 12);
  CHECK(fixture.status().saved.crc32 == 0xCAFEF00DU);
  CHECK(fixture.transfer->restart_pending());
  CHECK(fixture.transfer->check_busy(fixture.now).busy);
}

TEST_CASE("every write is Busy while a restart is pending, after the common checks") {
  Fixture fixture;
  fixture.upload(document_of(10));
  REQUIRE(fixture.write(kCommit).error == AttError::None);
  CHECK(fixture.write(start_pdu(10, 0)).error == AttError::Busy);
  CHECK(fixture.write(kAbort).error == AttError::Busy);
  CHECK(fixture.write(kCommit).error == AttError::Busy);
  CHECK(fixture.write(select_pdu(0)).error == AttError::Busy);
  CHECK(fixture.write({}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x09}).error == AttError::UnsupportedOperation);
  CHECK(fixture.write({0x03, 0x00}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.transfer->interrupt() == false);
  CHECK(fixture.status().state == TransferState::RestartPending);
}

TEST_CASE("common checks reject empty, unknown and wrongly sized writes") {
  Fixture fixture;
  CHECK(fixture.write({}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x00}).error == AttError::UnsupportedOperation);
  CHECK(fixture.write({0x06, 0, 0}).error == AttError::UnsupportedOperation);
  CHECK(fixture.write({0x01, 0, 0, 0, 0, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x01, 0, 0, 0, 0, 0, 0, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x02, 0, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x03, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x04, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.write({0x05, 0}).error == AttError::InvalidAttributeValueLength);
  CHECK(fixture.status().state == TransferState::Idle);
}

TEST_CASE("start checks the MTU, length, open transfer and store in order") {
  Fixture fixture;
  CHECK(fixture.write(start_pdu(10, 0), 63).error == AttError::MtuTooSmall);
  CHECK(fixture.write(start_pdu(0, 0), 63).error == AttError::MtuTooSmall);
  CHECK(fixture.write(start_pdu(0, 0)).error == AttError::InvalidPdu);
  CHECK(fixture.write(start_pdu(kMaxConfigBytes + 1, 0)).error == AttError::TooLarge);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.write(start_pdu(kMaxConfigBytes, 0), 64).error == AttError::None);
  CHECK(fixture.write(start_pdu(10, 0)).error == AttError::Busy);
  CHECK(fixture.status().transfer_length == kMaxConfigBytes);
}

TEST_CASE("start fails with StorageFailure when the boot has no config store") {
  Fixture fixture{ConfigTransferEnvironment{false, ActiveDocument{}}};
  CHECK(fixture.write(start_pdu(0, 0)).error == AttError::InvalidPdu);
  CHECK(fixture.write(start_pdu(10, 0)).error == AttError::StorageFailure);
  CHECK(fixture.status().state == TransferState::Idle);
}

TEST_CASE("start clears the previous result and reports the status change") {
  Fixture fixture;
  fixture.upload(document_of(10));
  REQUIRE(fixture.write(kAbort).error == AttError::None);
  REQUIRE(fixture.status().result == TransferResult::Aborted);
  const auto response = fixture.write(start_pdu(20, 0));
  CHECK(response.error == AttError::None);
  CHECK(response.status_changed);
  CHECK(fixture.status().result == TransferResult::None);
  CHECK(fixture.status().received_length == 0);
}

TEST_CASE("a chunk without an open transfer fails with InvalidState") {
  Fixture fixture;
  const auto document = document_of(10);
  CHECK(fixture.write(chunk_pdu(0, document, 0, 10)).error == AttError::InvalidState);
}

TEST_CASE("out-of-order and duplicate chunks fail with OffsetMismatch and change nothing") {
  Fixture fixture;
  const auto document = document_of(100);
  REQUIRE(fixture.write(start_pdu(100, crc32(view(document)))).error == AttError::None);
  CHECK(fixture.write(chunk_pdu(10, document, 10, 10)).error == AttError::OffsetMismatch);
  REQUIRE(fixture.write(chunk_pdu(0, document, 0, 50)).error == AttError::None);
  const auto duplicate = fixture.write(chunk_pdu(0, document, 0, 50));
  CHECK(duplicate.error == AttError::OffsetMismatch);
  CHECK_FALSE(duplicate.status_changed);
  CHECK(fixture.status().received_length == 50);
  REQUIRE(fixture.write(chunk_pdu(50, document, 50, 50)).error == AttError::None);
  CHECK(fixture.write(kCommit).error == AttError::None);
  CHECK(fixture.committer.received == document);
}

TEST_CASE("a chunk past the total length fails with InvalidPdu and keeps the transfer") {
  Fixture fixture;
  const auto document = document_of(30);
  REQUIRE(fixture.write(start_pdu(20, 0)).error == AttError::None);
  CHECK(fixture.write(chunk_pdu(0, document, 0, 21)).error == AttError::InvalidPdu);
  CHECK(fixture.status().received_length == 0);
  CHECK(fixture.status().state == TransferState::Receiving);
}

TEST_CASE("a chunk longer than MTU - 3 fails the length check") {
  Fixture fixture;
  const auto document = document_of(200);
  REQUIRE(fixture.write(start_pdu(200, 0), 64).error == AttError::None);
  // At MTU 64 a chunk PDU is at most 61 bytes: 58 data bytes.
  CHECK(fixture.write(chunk_pdu(0, document, 0, 59), 64).error ==
        AttError::InvalidAttributeValueLength);
  CHECK(fixture.write(chunk_pdu(0, document, 0, 58), 64).error == AttError::None);
}

TEST_CASE("commit without an open transfer fails with InvalidState and keeps the result") {
  Fixture fixture;
  fixture.upload(document_of(10));
  REQUIRE(fixture.write(kAbort).error == AttError::None);
  const auto response = fixture.write(kCommit);
  CHECK(response.error == AttError::InvalidState);
  CHECK_FALSE(response.status_changed);
  CHECK(fixture.status().result == TransferResult::Aborted);
  CHECK(fixture.committer.calls == 0);
}

TEST_CASE("an incomplete commit fails with Incomplete and keeps the transfer") {
  Fixture fixture;
  const auto document = document_of(100);
  REQUIRE(fixture.write(start_pdu(100, crc32(view(document)))).error == AttError::None);
  REQUIRE(fixture.write(chunk_pdu(0, document, 0, 60)).error == AttError::None);
  const auto response = fixture.write(kCommit);
  CHECK(response.error == AttError::Incomplete);
  CHECK_FALSE(response.status_changed);
  CHECK(fixture.status().state == TransferState::Receiving);
  CHECK(fixture.status().received_length == 60);
  CHECK(fixture.committer.calls == 0);
  REQUIRE(fixture.write(chunk_pdu(60, document, 60, 40)).error == AttError::None);
  CHECK(fixture.write(kCommit).error == AttError::None);
}

TEST_CASE("a CRC mismatch discards the transfer without calling the committer") {
  Fixture fixture;
  const auto document = document_of(64);
  REQUIRE(fixture.write(start_pdu(64, crc32(view(document)) ^ 1U)).error == AttError::None);
  REQUIRE(fixture.write(chunk_pdu(0, document, 0, 64)).error == AttError::None);
  const auto response = fixture.write(kCommit);
  CHECK(response.error == AttError::ChecksumMismatch);
  CHECK(response.status_changed);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::ChecksumMismatch);
  CHECK(fixture.committer.calls == 0);
  CHECK(fixture.write(kCommit).error == AttError::InvalidState);
}

TEST_CASE("a rejected config discards the transfer and keeps the diagnostic") {
  Fixture fixture;
  ConfigRejection rejection{};
  rejection.category = DiagnosticCategory::Parse;
  rejection.code = DiagnosticCode::MalformedJson;
  rejection.path = DiagnosticPath{"$"};
  fixture.committer.outcome = CommitOutcome::config_rejected(rejection);
  fixture.upload(document_of(10));
  const auto response = fixture.write(kCommit);
  CHECK(response.error == AttError::ConfigRejected);
  CHECK(response.status_changed);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::ConfigRejected);
  CHECK(fixture.status().rejection.code == DiagnosticCode::MalformedJson);
  CHECK(fixture.status().rejection.path.text() == "$");
  CHECK_FALSE(fixture.transfer->check_busy(fixture.now).busy);
}

TEST_CASE("a failed dry-run apply discards the transfer and keeps the apply status") {
  Fixture fixture;
  ApplyRejection apply{};
  apply.stage = ApplyStage::Rule;
  apply.engine = EngineConfigCode::UnknownSignal;
  apply.index = 4;
  fixture.committer.outcome = CommitOutcome::apply_rejected(apply);
  fixture.upload(document_of(10));
  CHECK(fixture.write(kCommit).error == AttError::ApplyRejected);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::ApplyRejected);
  CHECK(fixture.status().apply.engine == EngineConfigCode::UnknownSignal);
  CHECK(fixture.status().apply.index == 4);
}

TEST_CASE("a storage failure discards the transfer and does not become RestartPending") {
  Fixture fixture;
  fixture.committer.outcome = CommitOutcome::storage_failed();
  fixture.upload(document_of(10));
  CHECK(fixture.write(kCommit).error == AttError::StorageFailure);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::StorageFailed);
  CHECK_FALSE(fixture.transfer->restart_pending());
}

TEST_CASE("abort discards an open transfer and is a no-op without one") {
  Fixture fixture;
  const auto idle = fixture.write(kAbort);
  CHECK(idle.error == AttError::None);
  CHECK_FALSE(idle.status_changed);
  CHECK(fixture.status().result == TransferResult::None);

  fixture.upload(document_of(10));
  const auto response = fixture.write(kAbort);
  CHECK(response.error == AttError::None);
  CHECK(response.status_changed);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::Aborted);
  CHECK(fixture.write(kCommit).error == AttError::InvalidState);
}

TEST_CASE("the idle timeout discards a transfer 10 s after the last accepted write") {
  Fixture fixture;
  const auto document = document_of(100);
  REQUIRE(fixture.write(start_pdu(100, crc32(view(document)))).error == AttError::None);
  fixture.now += 9'000ms;
  REQUIRE(fixture.write(chunk_pdu(0, document, 0, 50)).error == AttError::None);
  // A rejected chunk does not restart the timer.
  fixture.now += 5'000ms;
  REQUIRE(fixture.write(chunk_pdu(0, document, 0, 50)).error == AttError::OffsetMismatch);
  CHECK_FALSE(fixture.transfer->expire_if_idle(fixture.now + 4'999ms));
  CHECK(fixture.status().state == TransferState::Receiving);
  CHECK(fixture.transfer->expire_if_idle(fixture.now + 5'000ms));
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::TimedOut);
  CHECK_FALSE(fixture.transfer->expire_if_idle(fixture.now + 60'000ms));
}

TEST_CASE("a write after the idle timeout sees the transfer already discarded") {
  Fixture fixture;
  const auto document = document_of(100);
  REQUIRE(fixture.write(start_pdu(100, crc32(view(document)))).error == AttError::None);
  fixture.now += kTransferIdleTimeout;
  const auto response = fixture.write(chunk_pdu(0, document, 0, 50));
  CHECK(response.error == AttError::InvalidState);
  CHECK(response.status_changed);
  CHECK(fixture.status().result == TransferResult::TimedOut);
}

TEST_CASE("a busy check discards an expired transfer instead of reporting Busy") {
  Fixture fixture;
  const auto document = document_of(100);
  REQUIRE(fixture.write(start_pdu(100, crc32(view(document)))).error == AttError::None);

  const auto before = fixture.transfer->check_busy(fixture.now + kTransferIdleTimeout - 1ms);
  CHECK(before.busy);
  CHECK_FALSE(before.status_changed);
  CHECK(fixture.status().state == TransferState::Receiving);

  const auto expired = fixture.transfer->check_busy(fixture.now + kTransferIdleTimeout);
  CHECK_FALSE(expired.busy);
  CHECK(expired.status_changed);
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::TimedOut);

  const auto again = fixture.transfer->check_busy(fixture.now + kTransferIdleTimeout);
  CHECK_FALSE(again.busy);
  CHECK_FALSE(again.status_changed);
}

TEST_CASE("a busy check never expires a pending restart") {
  Fixture fixture;
  fixture.upload(document_of(10));
  REQUIRE(fixture.write(kCommit).error == AttError::None);
  const auto check = fixture.transfer->check_busy(fixture.now + 60'000ms);
  CHECK(check.busy);
  CHECK_FALSE(check.status_changed);
  CHECK(fixture.transfer->restart_pending());
}

TEST_CASE("select read page is bounded by the active document's own length") {
  Fixture fixture{ConfigTransferEnvironment{true, ActiveDocument{}}};
  CHECK(fixture.write(select_pdu(0)).read_page_offset == std::optional<std::uint16_t>{0});
  CHECK(fixture.write(select_pdu(1)).error == AttError::InvalidPdu);
}

TEST_CASE("a disconnect interrupts an open transfer and is a no-op otherwise") {
  Fixture fixture;
  CHECK_FALSE(fixture.transfer->interrupt());
  fixture.upload(document_of(10));
  CHECK(fixture.transfer->interrupt());
  CHECK(fixture.status().state == TransferState::Idle);
  CHECK(fixture.status().result == TransferResult::Interrupted);
  CHECK(fixture.status().received_length == 0);
  CHECK(fixture.status().transfer_length == 0);
}

TEST_CASE("an accepted revert enters RestartPending only from Idle") {
  Fixture fixture;
  fixture.upload(document_of(10));
  CHECK_FALSE(fixture.transfer->enter_restart_pending());
  CHECK(fixture.status().state == TransferState::Receiving);
  REQUIRE(fixture.write(kAbort).error == AttError::None);
  CHECK(fixture.transfer->enter_restart_pending());
  CHECK(fixture.transfer->restart_pending());
  CHECK(fixture.write(start_pdu(10, 0)).error == AttError::Busy);
}

TEST_CASE("select read page accepts offsets up to the active length") {
  Fixture fixture{ConfigTransferEnvironment{true, active_450()}};
  const auto accepted = fixture.write(select_pdu(400));
  CHECK(accepted.error == AttError::None);
  CHECK(accepted.read_page_offset == std::optional<std::uint16_t>{400});
  CHECK(fixture.write(select_pdu(450)).read_page_offset == std::optional<std::uint16_t>{450});
  const auto rejected = fixture.write(select_pdu(451));
  CHECK(rejected.error == AttError::InvalidPdu);
  CHECK_FALSE(rejected.read_page_offset.has_value());
}

TEST_CASE("select read page works while a transfer is open and below the transfer MTU") {
  Fixture fixture{ConfigTransferEnvironment{true, active_450()}};
  fixture.upload(document_of(10));
  const auto response = fixture.write(select_pdu(200), 23);
  CHECK(response.error == AttError::None);
  CHECK(response.read_page_offset == std::optional<std::uint16_t>{200});
  CHECK(fixture.status().state == TransferState::Receiving);
}

TEST_CASE("a maximum-size upload fills the whole buffer") {
  Fixture fixture;
  const auto document = document_of(kMaxConfigBytes);
  fixture.upload(document, 241);
  CHECK(fixture.write(kCommit).error == AttError::None);
  CHECK(fixture.committer.received == document);
}

TEST_CASE("only the committing write reports entering RestartPending") {
  Fixture fixture;
  fixture.upload(document_of(10));
  const auto commit = fixture.write(kCommit);
  REQUIRE(commit.error == AttError::None);
  CHECK(commit.entered_restart_pending);
  // Later writes, Busy or malformed, must not schedule the restart again.
  for (const auto &pdu : {start_pdu(10, 0), kAbort, kCommit, select_pdu(0),
                          std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{0x09}}) {
    const auto later = fixture.write(pdu);
    CHECK_FALSE(later.entered_restart_pending);
    CHECK_FALSE(later.status_changed);
  }
  CHECK(fixture.transfer->restart_pending());
}

TEST_CASE("writes that do not commit never report entering RestartPending") {
  Fixture fixture;
  CHECK_FALSE(fixture.write(start_pdu(10, crc32(view(document_of(10))))).entered_restart_pending);
  fixture.committer.outcome = CommitOutcome::storage_failed();
  REQUIRE(fixture.write(chunk_pdu(0, document_of(10), 0, 10)).error == AttError::None);
  const auto failed = fixture.write(kCommit);
  CHECK_FALSE(failed.entered_restart_pending);
  CHECK_FALSE(fixture.transfer->restart_pending());
}

TEST_CASE("a revert's restart is not reported by later writes") {
  Fixture fixture;
  REQUIRE(fixture.transfer->enter_restart_pending());
  CHECK_FALSE(fixture.write(start_pdu(10, 0)).entered_restart_pending);
}
