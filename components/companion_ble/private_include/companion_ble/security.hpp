#pragma once

#include "companion_ble/companion_ble.hpp"
#include "host/ble_gap.h"

#include <cstdint>

namespace companion_ble::internal {

// The NimBLE binding of the companion pairing policy (ble-protocol.md,
// "Pairing and bonding"; ADR-0003). The decisions live in the portable
// companion_protocol PairingWindow, UserKeyDebouncer and LinkSecurity; this
// module only feeds them NimBLE events and applies their results. Every
// function except configure_security() runs on the NimBLE host task.

// Configures the security manager, the bond store callbacks and the policy
// timers. Call it once, after nimble_port_init() and before the host task
// starts.
void configure_security(const PairingInputs &inputs) noexcept;

// Re-applies the security binding after a NimBLE host sync, which resets the
// bond store callbacks. Call it first in the host sync callback.
void on_host_synced() noexcept;

// Handles the security-related GAP events of the companion connection. The
// advertising module forwards every GAP event here first. Returns the value
// the event handler must return to NimBLE (only BLE_GAP_EVENT_REPEAT_PAIRING
// uses it); BLE_GAP_EVENT_AUTHORIZE is answered through `event`.
[[nodiscard]] int handle_security_event(ble_gap_event &event) noexcept;

// Whether the pairing window is open now; served as Device info flags bit 0.
[[nodiscard]] bool pairing_window_open() noexcept;

// The access check of every protected (encrypted, bonded) attribute. Returns
// 0 when the access may proceed, otherwise the ATT error to return. Restarts
// the unbonded-link period on the link's first rejection and schedules a
// disconnect for an encrypted link without an accepted bond. A local access
// (BLE_HS_CONN_HANDLE_NONE) is allowed.
[[nodiscard]] int check_protected_access(std::uint16_t connection) noexcept;

// Whether `connection` is encrypted with an accepted, stored bond. Protected
// notifiers (#165, #166) send only to such a link.
[[nodiscard]] bool link_has_accepted_bond(std::uint16_t connection) noexcept;

// Deletes every stored bond, including the connected central's, and revokes
// the current link's accepted bond. Returns false when a deletion failed and
// some bonds may remain. Does not open the pairing window and does not
// disconnect: the Command characteristic (#165) answers the write, then calls
// disconnect_after_response().
[[nodiscard]] bool clear_bonds() noexcept;

// Terminates `connection` from the host event queue, after the current ATT
// response has been queued.
void disconnect_after_response(std::uint16_t connection) noexcept;

} // namespace companion_ble::internal
