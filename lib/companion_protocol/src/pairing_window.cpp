#include "companion_protocol/pairing_window.hpp"

namespace companion_protocol {

bool PairingWindow::open(const PairingClock now) noexcept {
  if (storage_ != BondStorage::Available)
    return false;
  closes_at_ = now + kPairingWindowPeriod;
  return true;
}

bool PairingWindow::is_open(const PairingClock now) const noexcept {
  return closes_at_.has_value() && now < *closes_at_;
}

RepeatPairingAction PairingWindow::repeat_pairing_action(const PairingClock now) const noexcept {
  return is_open(now) ? RepeatPairingAction::DeleteOldBondAndRetry
                      : RepeatPairingAction::IgnoreAndDisconnect;
}

} // namespace companion_protocol
