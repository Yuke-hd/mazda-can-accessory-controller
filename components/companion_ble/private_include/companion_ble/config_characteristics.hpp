#pragma once

#include "companion_ble/companion_ble.hpp"
#include "companion_ble/gatt_service.hpp"

namespace companion_ble::internal {

// The NimBLE binding of the companion config transfer (config-transfer.md) and
// the Command characteristic (ble-protocol.md, "Command"). The transfer state
// machine, wire codes and read-back pages live in the portable
// companion_protocol ConfigTransfer; parsing, the dry-run apply and the
// persistence behind it are the composition root's ports in `ConfigInputs`.
// Every function except prepare_config_transfer() runs on the NimBLE host
// task, and so does every commit: transfer writes, commits, the idle timeout,
// the disconnect interruption and the controlled restart are serialized there.

// Builds this boot's ActiveDocument and the transfer from `inputs`. Call it
// once on the startup task, before registration. Returns false, leaving the
// Config, Config status and Command characteristics unregistered, when a port
// is missing.
[[nodiscard]] bool prepare_config_transfer(const ConfigInputs &inputs) noexcept;

// Initializes the timers and the GAP listener. Call it once, after a
// successful prepare_config_transfer() and nimble_port_init(), before the
// host task starts.
void configure_config_transfer() noexcept;

// Config: Read returns a read-back page, Write carries the transfer PDUs.
[[nodiscard]] CharacteristicDefinition config_characteristic() noexcept;
// Config status: Read and Notify.
[[nodiscard]] CharacteristicDefinition config_status_characteristic() noexcept;
// Command: Write (Revert to factory, Clear bonds).
[[nodiscard]] CharacteristicDefinition command_characteristic() noexcept;

} // namespace companion_ble::internal
