#pragma once

#include <string>

#include "companion_config/apply_check.hpp"
#include "companion_protocol/config_ports.hpp"
#include "companion_protocol/config_transfer.hpp"
#include "controller_config/persisted/config_store.hpp"
#include "controller_config/persisted/model.hpp"

namespace companion_config {

// The controller side of the companion config transfer. It implements the
// companion protocol's ports over the controller configuration, so the BLE
// binding never sees a configuration type:
//
// - ConfigBootSource: the boot selection and the one ActiveDocument served
//   by Config status and read-back;
// - ConfigCommitter: parse, dry-run apply, then save_override(), the only
//   persistence path (config-transfer.md, "Commit");
// - FactoryReverter: clear_override() for Revert to factory.
//
// record_boot() and mark_lighting_setup_failed() run before the BLE startup
// task is created. Every other call runs on the BLE tasks afterwards. A commit
// only persists: the running configuration changes at the next boot.
class ConfigService final : public companion_protocol::ConfigBootSource,
                            public companion_protocol::ConfigCommitter,
                            public companion_protocol::FactoryReverter {
public:
  // `active` is the configuration the boot path applies; it must outlive the
  // service and stay unchanged after record_boot().
  ConfigService(const controller_config::persisted::ControllerConfig &active,
                ApplyCheck &apply_check) noexcept
      : active_(active), apply_check_(apply_check) {}

  // Records the boot selection and the config store, which must outlive the
  // service. A null store disables commits and Revert to factory.
  void record_boot(const controller_config::persisted::BootConfigurationResult &selection,
                   controller_config::persisted::ConfigStoreBackend *store,
                   bool configuration_applied = true,
                   bool override_apply_rejected = false) noexcept;

  // Reports a later startup refusal before BLE serves Config status. The
  // stopped vehicle service has no active document, but its config store may
  // still accept a repair upload.
  void mark_lighting_setup_failed() noexcept;

  // Serializes the active configuration once into storage owned by this
  // service; later calls return the same document.
  [[nodiscard]] companion_protocol::ConfigBoot prepare_boot() noexcept override;
  [[nodiscard]] companion_protocol::CommitOutcome
  commit(companion_protocol::ByteView document) noexcept override;
  [[nodiscard]] bool revert_to_factory() noexcept override;

private:
  [[nodiscard]] companion_protocol::ActiveDocument active_document() noexcept;

  const controller_config::persisted::ControllerConfig &active_;
  ApplyCheck &apply_check_;
  controller_config::persisted::ConfigStoreBackend *store_{nullptr};
  companion_protocol::BootStatus boot_{};
  companion_protocol::ConfigSource source_{companion_protocol::ConfigSource::None};
  bool prepared_{false};
  // The canonical active document; never modified once prepared, so the
  // ActiveDocument view into it stays valid until restart.
  std::string canonical_{};
};

} // namespace companion_config
