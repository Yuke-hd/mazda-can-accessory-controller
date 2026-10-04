#pragma once

#include "companion_protocol/pairing_window.hpp"

#include <cstdint>
#include <optional>

namespace companion_protocol {

// A link without an accepted, stored bond is dropped this long after the
// latest of its connection, its first protected-attribute rejection and the
// latest accepted pairing PDU (ble-protocol.md, roles and connections).
inline constexpr PairingClock kUnbondedLinkIdleLimit{30'000};
// ... and never later than this after its connection.
inline constexpr PairingClock kUnbondedLinkAbsoluteLimit{90'000};

// Only full-size keys make an accepted bond.
inline constexpr std::uint8_t kRequiredEncryptionKeySize = 16U;

enum class LinkEncryption : std::uint8_t { Unencrypted, Encrypted };
// Whether the controller has a stored bond for the central's identity.
enum class PeerBond : std::uint8_t { None, Stored };

// The outcome of a read, write or CCCD write of a protected attribute.
enum class ProtectedAccess : std::uint8_t {
  Allow,
  // Unencrypted, no bond for the central: it may pair.
  InsufficientAuthentication,
  // Unencrypted, bond stored: the central re-encrypts with its key.
  InsufficientEncryption,
  // Encrypted without an accepted bond: reject and drop the link at once.
  InsufficientAuthenticationAndDisconnect,
};

// The ATT error code of a rejected access; 0 for Allow.
[[nodiscard]] std::uint8_t att_error_code(ProtectedAccess access) noexcept;

// The link's security state once encryption completed.
struct EncryptedLink {
  // The host reports the link as bonded (its keys are stored).
  bool bonded{false};
  std::uint8_t key_size{0U};
  PeerBond peer_bond{PeerBond::None};
};

enum class EncryptionVerdict : std::uint8_t {
  // Re-encryption with a bond stored before this link.
  BondRestored,
  // A pairing inside the window stored a new bond: the window closes.
  NewBondAccepted,
  // No accepted bond; this link stored nothing. Drop the link.
  Rejected,
  // No accepted bond, but this link's pairing stored keys: delete them, then
  // drop the link.
  RejectedDeleteKeys,
};

// Access-control state of the single companion connection. A link becomes
// trusted only after encryption with an accepted, stored bond; until then it
// may hold the connection slot for a bounded time.
class LinkSecurity final {
public:
  LinkSecurity() noexcept = default;

  // A central connected; any previous link's state is forgotten.
  void connected(PairingClock now) noexcept;
  // The bond store wrote this link's pairing keys (allowed only inside the
  // pairing window).
  void bond_keys_written() noexcept { keys_written_ = true; }
  // A pairing PDU the controller accepted inside the window was received.
  void accepted_pairing_pdu(PairingClock now) noexcept { idle_since_ = now; }
  [[nodiscard]] EncryptionVerdict encrypted(const EncryptedLink &link) noexcept;
  // The link's bond was deleted, for example by Clear bonds.
  void revoke_bond() noexcept { accepted_ = false; }

  [[nodiscard]] ProtectedAccess check_protected_access(PairingClock now, LinkEncryption encryption,
                                                       PeerBond peer_bond) noexcept;
  [[nodiscard]] bool has_accepted_bond() const noexcept { return accepted_; }
  // When the link must be dropped; empty once its bond is accepted.
  [[nodiscard]] std::optional<PairingClock> drop_deadline() const noexcept;
  [[nodiscard]] bool must_drop(PairingClock now) const noexcept;

private:
  explicit LinkSecurity(PairingClock now) noexcept;

  PairingClock connected_at_{0};
  PairingClock idle_since_{0};
  bool rejection_seen_{false};
  bool keys_written_{false};
  bool accepted_{false};
};

} // namespace companion_protocol
