#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_protocol/hook_guard.hpp"
#include "companion_protocol/link_security.hpp"
#include "companion_protocol/pairing_window.hpp"
#include "companion_protocol/user_key_debouncer.hpp"

#include <optional>

namespace {

using companion_protocol::BondStorage;
using companion_protocol::EncryptedLink;
using companion_protocol::EncryptionVerdict;
using companion_protocol::kPairingWindowPeriod;
using companion_protocol::kUnbondedLinkAbsoluteLimit;
using companion_protocol::kUnbondedLinkIdleLimit;
using companion_protocol::LinkEncryption;
using companion_protocol::LinkSecurity;
using companion_protocol::PairingClock;
using companion_protocol::PairingWindow;
using companion_protocol::PeerBond;
using companion_protocol::ProtectedAccess;
using companion_protocol::RepeatPairingAction;
using companion_protocol::UserKeyDebouncer;
using companion_protocol::UserKeyLevel;

constexpr PairingClock ms(const long long value) { return PairingClock{value}; }

// --- Pairing window -------------------------------------------------------

TEST_CASE("the pairing window is closed at boot") {
  const PairingWindow window{BondStorage::Available};
  CHECK_FALSE(window.is_open(ms(0)));
  CHECK_FALSE(window.closes_at().has_value());
}

TEST_CASE("a key press opens the window for 120 s") {
  PairingWindow window{BondStorage::Available};
  CHECK(window.open(ms(1'000)));
  CHECK(window.is_open(ms(1'000)));
  CHECK(window.is_open(ms(1'000) + kPairingWindowPeriod - ms(1)));
  CHECK_FALSE(window.is_open(ms(1'000) + kPairingWindowPeriod));
  CHECK(window.closes_at() == std::optional<PairingClock>{ms(121'000)});
}

TEST_CASE("a press while the window is open restarts the 120 s period") {
  PairingWindow window{BondStorage::Available};
  CHECK(window.open(ms(0)));
  CHECK(window.open(ms(100'000)));
  CHECK(window.is_open(ms(219'999)));
  CHECK_FALSE(window.is_open(ms(220'000)));
}

TEST_CASE("the window closes after one new bond is stored") {
  PairingWindow window{BondStorage::Available};
  CHECK(window.open(ms(0)));
  window.close();
  CHECK_FALSE(window.is_open(ms(1)));
  CHECK_FALSE(window.closes_at().has_value());
}

TEST_CASE("without bond storage the window never opens") {
  PairingWindow window{BondStorage::Unavailable};
  CHECK_FALSE(window.open(ms(0)));
  CHECK_FALSE(window.is_open(ms(0)));
  CHECK_FALSE(window.closes_at().has_value());
}

TEST_CASE("a repeat pairing inside the window deletes the old bond and retries") {
  PairingWindow window{BondStorage::Available};
  CHECK(window.open(ms(0)));
  CHECK(window.repeat_pairing_action(ms(10)) == RepeatPairingAction::DeleteOldBondAndRetry);
}

TEST_CASE("a repeat pairing outside the window keeps the bond and drops the link") {
  PairingWindow window{BondStorage::Available};
  CHECK(window.repeat_pairing_action(ms(10)) == RepeatPairingAction::IgnoreAndDisconnect);
  CHECK(window.open(ms(0)));
  CHECK(window.repeat_pairing_action(kPairingWindowPeriod) ==
        RepeatPairingAction::IgnoreAndDisconnect);
}

// --- User key debouncer ---------------------------------------------------

void feed(UserKeyDebouncer &debouncer, const UserKeyLevel level, const int samples, int &presses) {
  for (int sample = 0; sample < samples; ++sample) {
    if (debouncer.sample(level))
      ++presses;
  }
}

TEST_CASE("a key held from boot is not a press until it is released and pressed again") {
  UserKeyDebouncer debouncer{};
  int presses = 0;
  feed(debouncer, UserKeyLevel::Pressed, 50, presses);
  CHECK(presses == 0);
  feed(debouncer, UserKeyLevel::Released, companion_protocol::kUserKeyDebounceSamples, presses);
  feed(debouncer, UserKeyLevel::Pressed, companion_protocol::kUserKeyDebounceSamples, presses);
  CHECK(presses == 1);
}

TEST_CASE("a debounced press after a debounced release fires once") {
  UserKeyDebouncer debouncer{};
  int presses = 0;
  feed(debouncer, UserKeyLevel::Released, 10, presses);
  feed(debouncer, UserKeyLevel::Pressed, companion_protocol::kUserKeyDebounceSamples - 1, presses);
  CHECK(presses == 0);
  feed(debouncer, UserKeyLevel::Pressed, 1, presses);
  CHECK(presses == 1);
  feed(debouncer, UserKeyLevel::Pressed, 100, presses);
  CHECK(presses == 1);
}

TEST_CASE("contact bounce shorter than the debounce period is ignored") {
  UserKeyDebouncer debouncer{};
  int presses = 0;
  feed(debouncer, UserKeyLevel::Released, 10, presses);
  for (int bounce = 0; bounce < 20; ++bounce) {
    feed(debouncer, UserKeyLevel::Pressed, companion_protocol::kUserKeyDebounceSamples - 1,
         presses);
    feed(debouncer, UserKeyLevel::Released, 1, presses);
  }
  CHECK(presses == 0);
}

TEST_CASE("each separate press fires once") {
  UserKeyDebouncer debouncer{};
  int presses = 0;
  for (int press = 0; press < 3; ++press) {
    feed(debouncer, UserKeyLevel::Released, 5, presses);
    feed(debouncer, UserKeyLevel::Pressed, 5, presses);
  }
  CHECK(presses == 3);
}

// --- Link security --------------------------------------------------------

constexpr EncryptedLink kBondedFullKey{true, 16U, PeerBond::Stored};

TEST_CASE("a new link without an accepted bond is dropped 30 s after connecting") {
  LinkSecurity link{};
  link.connected(ms(5'000));
  CHECK_FALSE(link.has_accepted_bond());
  CHECK(link.drop_deadline() == std::optional<PairingClock>{ms(5'000) + kUnbondedLinkIdleLimit});
  CHECK_FALSE(link.must_drop(ms(34'999)));
  CHECK(link.must_drop(ms(35'000)));
}

TEST_CASE("the first protected-attribute rejection restarts the 30 s period") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.check_protected_access(ms(20'000), LinkEncryption::Unencrypted, PeerBond::None) ==
        ProtectedAccess::InsufficientAuthentication);
  CHECK(link.drop_deadline() == std::optional<PairingClock>{ms(50'000)});
  // Only the first rejection counts.
  CHECK(link.check_protected_access(ms(40'000), LinkEncryption::Unencrypted, PeerBond::None) ==
        ProtectedAccess::InsufficientAuthentication);
  CHECK(link.drop_deadline() == std::optional<PairingClock>{ms(50'000)});
}

TEST_CASE("an accepted pairing PDU restarts the 30 s period") {
  LinkSecurity link{};
  link.connected(ms(0));
  link.accepted_pairing_pdu(ms(25'000));
  CHECK(link.drop_deadline() == std::optional<PairingClock>{ms(55'000)});
}

TEST_CASE("nothing extends an unbonded link past 90 s") {
  LinkSecurity link{};
  link.connected(ms(1'000));
  link.accepted_pairing_pdu(ms(70'000));
  CHECK(link.drop_deadline() ==
        std::optional<PairingClock>{ms(1'000) + kUnbondedLinkAbsoluteLimit});
  CHECK_FALSE(link.must_drop(ms(90'999)));
  CHECK(link.must_drop(ms(91'000)));
}

TEST_CASE("unencrypted protected access asks for encryption when a bond is stored") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.check_protected_access(ms(1), LinkEncryption::Unencrypted, PeerBond::Stored) ==
        ProtectedAccess::InsufficientEncryption);
}

TEST_CASE("an encrypted link without an accepted bond is rejected and dropped") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.check_protected_access(ms(1), LinkEncryption::Encrypted, PeerBond::Stored) ==
        ProtectedAccess::InsufficientAuthenticationAndDisconnect);
}

TEST_CASE("re-encryption with a stored bond is accepted and lifts the time limits") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.encrypted(kBondedFullKey) == EncryptionVerdict::BondRestored);
  CHECK(link.has_accepted_bond());
  CHECK_FALSE(link.drop_deadline().has_value());
  CHECK_FALSE(link.must_drop(ms(1'000'000)));
  CHECK(link.check_protected_access(ms(1), LinkEncryption::Encrypted, PeerBond::Stored) ==
        ProtectedAccess::Allow);
}

TEST_CASE("a pairing whose keys were stored on this link is a new accepted bond") {
  LinkSecurity link{};
  link.connected(ms(0));
  link.bond_keys_written();
  CHECK(link.encrypted(kBondedFullKey) == EncryptionVerdict::NewBondAccepted);
  CHECK(link.has_accepted_bond());
}

TEST_CASE("an encryption without a stored bond is rejected without deleting bonds") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.encrypted(EncryptedLink{true, 16U, PeerBond::None}) == EncryptionVerdict::Rejected);
  CHECK(link.encrypted(EncryptedLink{false, 16U, PeerBond::Stored}) == EncryptionVerdict::Rejected);
  CHECK_FALSE(link.has_accepted_bond());
}

TEST_CASE("a rejected pairing that stored keys on this link deletes them") {
  LinkSecurity link{};
  link.connected(ms(0));
  link.bond_keys_written();
  CHECK(link.encrypted(EncryptedLink{true, 7U, PeerBond::Stored}) ==
        EncryptionVerdict::RejectedDeleteKeys);
  CHECK(link.encrypted(EncryptedLink{false, 16U, PeerBond::Stored}) ==
        EncryptionVerdict::RejectedDeleteKeys);
  CHECK_FALSE(link.has_accepted_bond());
}

TEST_CASE("a reduced encryption key size is never an accepted bond") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.encrypted(EncryptedLink{true, 15U, PeerBond::Stored}) == EncryptionVerdict::Rejected);
}

TEST_CASE("revoking the bond returns the link to the unbonded limits") {
  LinkSecurity link{};
  link.connected(ms(0));
  CHECK(link.encrypted(kBondedFullKey) == EncryptionVerdict::BondRestored);
  link.revoke_bond();
  CHECK_FALSE(link.has_accepted_bond());
  CHECK(link.check_protected_access(ms(1), LinkEncryption::Encrypted, PeerBond::None) ==
        ProtectedAccess::InsufficientAuthenticationAndDisconnect);
}

TEST_CASE("a new connection forgets the previous link") {
  LinkSecurity link{};
  link.connected(ms(0));
  link.bond_keys_written();
  CHECK(link.encrypted(kBondedFullKey) == EncryptionVerdict::NewBondAccepted);
  link.connected(ms(100'000));
  CHECK_FALSE(link.has_accepted_bond());
  CHECK(link.encrypted(kBondedFullKey) == EncryptionVerdict::BondRestored);
}

TEST_CASE("ATT error codes match the protected access outcomes") {
  CHECK(companion_protocol::att_error_code(ProtectedAccess::Allow) == 0x00U);
  CHECK(companion_protocol::att_error_code(ProtectedAccess::InsufficientAuthentication) == 0x05U);
  CHECK(companion_protocol::att_error_code(ProtectedAccess::InsufficientEncryption) == 0x0FU);
  CHECK(companion_protocol::att_error_code(
            ProtectedAccess::InsufficientAuthenticationAndDisconnect) == 0x05U);
}

// --- Hook guard -----------------------------------------------------------

using companion_protocol::HookGuard;
using companion_protocol::HookState;
using StoreWrite = int (*)(int);

int host_store_write(const int value) { return value; }
int reinitialized_store_write(const int value) { return value + 1; }
int guarded_write(const int value) { return -value; }

TEST_CASE("the hook guard installs itself and forwards to the host function") {
  HookGuard<StoreWrite> guard{guarded_write};
  StoreWrite slot = host_store_write;
  CHECK(guard.ensure(slot) == HookState::Installed);
  CHECK(slot == &guarded_write);
  CHECK(guard.original() == &host_store_write);
  CHECK(guard.ensure(slot) == HookState::AlreadyInstalled);
  CHECK(slot == &guarded_write);
}

TEST_CASE("the hook guard survives the host resetting its slot at sync") {
  HookGuard<StoreWrite> guard{guarded_write};
  StoreWrite slot = host_store_write;
  CHECK(guard.ensure(slot) == HookState::Installed);
  // NimBLE's host startup re-initializes the store and overwrites the slot.
  slot = reinitialized_store_write;
  CHECK(guard.ensure(slot) == HookState::Installed);
  CHECK(slot == &guarded_write);
  CHECK(guard.original() == &reinitialized_store_write);
}

TEST_CASE("the hook guard reports a missing host function") {
  HookGuard<StoreWrite> guard{guarded_write};
  StoreWrite slot = nullptr;
  CHECK(guard.ensure(slot) == HookState::Missing);
  CHECK(slot == nullptr);
  CHECK(guard.original() == nullptr);
  // A guard placed in the slot without anything to forward to is not usable.
  slot = guarded_write;
  CHECK(guard.ensure(slot) == HookState::Missing);
}

} // namespace
