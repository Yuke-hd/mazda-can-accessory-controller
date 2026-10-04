#include "companion_protocol/link_security.hpp"

#include <algorithm>

namespace companion_protocol {
namespace {

constexpr std::uint8_t kInsufficientAuthentication = 0x05U;
constexpr std::uint8_t kInsufficientEncryption = 0x0FU;

bool is_accepted_bond(const EncryptedLink &link) noexcept {
  return link.bonded && link.key_size == kRequiredEncryptionKeySize &&
         link.peer_bond == PeerBond::Stored;
}

} // namespace

std::uint8_t att_error_code(const ProtectedAccess access) noexcept {
  switch (access) {
  case ProtectedAccess::Allow:
    return 0U;
  case ProtectedAccess::InsufficientEncryption:
    return kInsufficientEncryption;
  case ProtectedAccess::InsufficientAuthentication:
  case ProtectedAccess::InsufficientAuthenticationAndDisconnect:
    return kInsufficientAuthentication;
  }
  return kInsufficientAuthentication;
}

void LinkSecurity::connected(const PairingClock now) noexcept { *this = LinkSecurity{now}; }

LinkSecurity::LinkSecurity(const PairingClock now) noexcept
    : connected_at_{now}, idle_since_{now} {}

EncryptionVerdict LinkSecurity::encrypted(const EncryptedLink &link) noexcept {
  accepted_ = is_accepted_bond(link);
  if (accepted_)
    return keys_written_ ? EncryptionVerdict::NewBondAccepted : EncryptionVerdict::BondRestored;
  return keys_written_ ? EncryptionVerdict::RejectedDeleteKeys : EncryptionVerdict::Rejected;
}

ProtectedAccess LinkSecurity::check_protected_access(const PairingClock now,
                                                     const LinkEncryption encryption,
                                                     const PeerBond peer_bond) noexcept {
  if (accepted_)
    return ProtectedAccess::Allow;
  if (encryption == LinkEncryption::Encrypted)
    return ProtectedAccess::InsufficientAuthenticationAndDisconnect;
  if (!rejection_seen_) {
    rejection_seen_ = true;
    idle_since_ = std::max(idle_since_, now);
  }
  return peer_bond == PeerBond::Stored ? ProtectedAccess::InsufficientEncryption
                                       : ProtectedAccess::InsufficientAuthentication;
}

std::optional<PairingClock> LinkSecurity::drop_deadline() const noexcept {
  if (accepted_)
    return std::nullopt;
  return std::min(idle_since_ + kUnbondedLinkIdleLimit, connected_at_ + kUnbondedLinkAbsoluteLimit);
}

bool LinkSecurity::must_drop(const PairingClock now) const noexcept {
  const auto deadline = drop_deadline();
  return deadline.has_value() && now >= *deadline;
}

} // namespace companion_protocol
