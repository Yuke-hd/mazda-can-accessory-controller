#pragma once

#include "companion_protocol/config_status.hpp"
#include "companion_protocol/config_transfer.hpp"

namespace companion_protocol {

// What the controller selected at boot, as the config transfer serves it
// (config-transfer.md, "Read-back" and "Boot flags"). `status.active` is the
// one ActiveDocument of this boot: Config status and read-back pages report
// the same source, length and CRC because both are built from this value.
// Its bytes belong to the ConfigBootSource and stay unchanged until restart.
struct ConfigBoot {
  BootStatus status{};
  bool config_store_available{false};

  [[nodiscard]] ConfigTransferEnvironment environment() const noexcept {
    return ConfigTransferEnvironment{config_store_available, status.active};
  }
};

// Supplies the boot selection to the BLE binding. Called once, before the
// BLE host serves any companion attribute.
class ConfigBootSource {
public:
  ConfigBootSource(const ConfigBootSource &) = delete;
  ConfigBootSource &operator=(const ConfigBootSource &) = delete;

  [[nodiscard]] virtual ConfigBoot prepare_boot() noexcept = 0;

protected:
  ConfigBootSource() noexcept = default;
  ~ConfigBootSource() = default;
};

// Deactivates the persisted override for the Revert to factory command
// (ble-protocol.md, "Revert to factory"). Returns false when the store is
// unavailable or clearing failed; the caller then does not restart.
class FactoryReverter {
public:
  FactoryReverter(const FactoryReverter &) = delete;
  FactoryReverter &operator=(const FactoryReverter &) = delete;

  [[nodiscard]] virtual bool revert_to_factory() noexcept = 0;

protected:
  FactoryReverter() noexcept = default;
  ~FactoryReverter() = default;
};

} // namespace companion_protocol
