#include "companion_config/config_service.hpp"

#include <cstdint>
#include <string_view>

#include "companion_config/wire_codes.hpp"
#include "companion_protocol/crc32.hpp"
#include "controller_config/persisted/json_loader.hpp"

namespace companion_config {
namespace {

namespace wire = companion_protocol;
namespace persisted = controller_config::persisted;

wire::ByteView bytes_of(const std::string_view text) noexcept {
  return wire::ByteView{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::string_view text_of(const wire::ByteView bytes) noexcept {
  return std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

} // namespace

void ConfigService::record_boot(const persisted::BootConfigurationResult &selection,
                                persisted::ConfigStoreBackend *const store) noexcept {
  store_ = store;
  if (!selection.ok())
    source_ = wire::ConfigSource::None;
  else if (selection.source == persisted::ConfigurationSource::PersistedOverride)
    source_ = wire::ConfigSource::Override;
  else
    source_ = wire::ConfigSource::Factory;

  boot_ = wire::BootStatus{};
  boot_.flags.invalid_override = selection.override_diagnostic.has_value();
  boot_.flags.override_read_failed = !selection.override_storage_message.empty();
  boot_.flags.no_config_store = store == nullptr;
  // Only the selection failure is known here. TODO(GH-165 follow-up): bit 3
  // also covers apply, engine attach, progress watch and telemetry start
  // failures, but app_main fails off and returns on each of them before the
  // companion link starts, so no Config status is ever served for such a
  // boot. Record them here once a failed boot keeps the companion link.
  boot_.flags.lighting_setup_failed = !selection.ok();
  if (selection.override_diagnostic.has_value()) {
    boot_.override_code = to_wire(selection.override_diagnostic->code);
    boot_.override_validation = to_wire(selection.override_diagnostic->schema_error);
  }
  prepared_ = false;
  canonical_.clear();
}

wire::ConfigBoot ConfigService::prepare_boot() noexcept {
  if (!prepared_) {
    boot_.active = active_document();
    prepared_ = true;
  }
  return wire::ConfigBoot{boot_, store_ != nullptr};
}

wire::ActiveDocument ConfigService::active_document() noexcept {
  if (source_ == wire::ConfigSource::None)
    return wire::ActiveDocument{};
  // The boot path applied `active_`, which loaded from canonical JSON, so its
  // serialization is the stored override or the canonical factory document.
  canonical_ = persisted::serialize_controller_config(active_);
  const auto document = wire::ActiveDocument::selected(source_, bytes_of(canonical_));
  return document.value_or(wire::ActiveDocument{});
}

wire::CommitOutcome ConfigService::commit(const wire::ByteView document) noexcept {
  if (store_ == nullptr)
    return wire::CommitOutcome::storage_failed();

  const std::string_view json = text_of(document);
  const auto parsed = persisted::parse_controller_config(json);
  if (!parsed.ok())
    return wire::CommitOutcome::config_rejected(parsed.diagnostic.has_value()
                                                    ? config_rejection(*parsed.diagnostic)
                                                    : wire::ConfigRejection{});

  const auto applied = apply_check_.dry_run(*parsed.configuration);
  if (!applied.ok())
    return wire::CommitOutcome::apply_rejected(apply_rejection(applied));

  // save_override() is the only persistence path. It re-parses the upload and
  // enforces the 4 KiB canonical limit before writing.
  persisted::ConfigStore store{*store_};
  const auto saved = store.save_override(json);
  switch (saved.status) {
  case persisted::SaveOverrideResult::Status::Saved:
    return wire::CommitOutcome::saved(
        wire::SavedDocument{static_cast<std::uint16_t>(saved.canonical_json.size()),
                            wire::crc32(bytes_of(saved.canonical_json))});
  case persisted::SaveOverrideResult::Status::InvalidCandidate:
    return wire::CommitOutcome::config_rejected(saved.diagnostic.has_value()
                                                    ? config_rejection(*saved.diagnostic)
                                                    : wire::ConfigRejection{});
  case persisted::SaveOverrideResult::Status::Failed:
    return wire::CommitOutcome::storage_failed();
  }
  return wire::CommitOutcome::storage_failed();
}

bool ConfigService::revert_to_factory() noexcept {
  if (store_ == nullptr)
    return false;
  persisted::ConfigStore store{*store_};
  return store.clear_override().ok;
}

} // namespace companion_config
