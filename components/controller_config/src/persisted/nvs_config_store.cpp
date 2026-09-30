#include "controller_config/persisted/nvs_config_store_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#if defined(ESP_PLATFORM)
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace controller_config::persisted::internal {
namespace {

constexpr char kActiveKey[] = "active_slot";
constexpr char kSlotKeys[][7] = {"slot_a", "slot_b"};
constexpr std::size_t kSlotCount = 2U;

} // namespace

NvsConfigBackend::~NvsConfigBackend() { api_.close(handle_); }

std::string NvsConfigBackend::error_message(const char *const operation,
                                            const NvsResult result) const {
  return std::string(operation) + " failed: " + api_.error_name(result);
}

NvsConfigBackend::BlobRead NvsConfigBackend::read_blob(const char *const key) const {
  std::size_t length = 0U;
  NvsResult result = api_.get_blob(handle_, key, nullptr, 0U, length);
  if (result == kNvsNotFound)
    return {};
  if (result != kNvsOk)
    return {BlobRead::Status::Failed, {}, error_message("read configuration size", result)};
  if (length == 0U || length > kMaxControllerConfigJsonBytes)
    return {BlobRead::Status::Failed, {}, "stored configuration JSON exceeds storage bounds"};

  std::string value(length, '\0');
  result = api_.get_blob(handle_, key, value.data(), value.size(), length);
  if (result != kNvsOk)
    return {BlobRead::Status::Failed, {}, error_message("read configuration", result)};
  value.resize(length);
  return {BlobRead::Status::Present, std::move(value), {}};
}

std::optional<std::uint8_t> NvsConfigBackend::read_active_slot() {
  active_slot_error_.reset();
  std::uint8_t active_slot = 0U;
  const NvsResult result = api_.get_u8(handle_, kActiveKey, active_slot);
  if (result == kNvsNotFound)
    return std::nullopt;
  if (result != kNvsOk) {
    active_slot_error_ = error_message("read active slot", result);
    return std::nullopt;
  }
  if (active_slot >= kSlotCount) {
    active_slot_error_ = "active slot marker is out of range";
    return std::nullopt;
  }
  return active_slot;
}

ConfigStoreBackend::ReadResult NvsConfigBackend::read_override() {
  std::uint8_t active_slot = 0U;
  const NvsResult active_result = api_.get_u8(handle_, kActiveKey, active_slot);
  if (active_result == kNvsNotFound)
    return {};
  if (active_result != kNvsOk)
    return {ReadStatus::Failed, {}, error_message("read active slot", active_result)};
  if (active_slot >= kSlotCount)
    return {ReadStatus::Failed, {}, "active slot marker is out of range"};

  const BlobRead blob = read_blob(kSlotKeys[active_slot]);
  if (blob.status == BlobRead::Status::Missing)
    return {ReadStatus::Failed, {}, "active slot has no configuration JSON"};
  if (blob.status == BlobRead::Status::Failed)
    return {ReadStatus::Failed, {}, blob.message};
  return {ReadStatus::Present, blob.value, {}};
}

ConfigStoreBackend::WriteResult
NvsConfigBackend::write_override(const std::string_view canonical_json) {
  if (canonical_json.empty() || canonical_json.size() > kMaxControllerConfigJsonBytes)
    return {false, "canonical configuration JSON exceeds storage bounds"};

  const auto active_slot = read_active_slot();
  if (!active_slot.has_value() && active_slot_error_)
    return {false, *active_slot_error_};
  const std::uint8_t target_slot = active_slot.has_value() ? (*active_slot == 0U ? 1U : 0U) : 0U;
  const char *const target_key = kSlotKeys[target_slot];

  NvsResult result =
      api_.set_blob(handle_, target_key, canonical_json.data(), canonical_json.size());
  if (result != kNvsOk)
    return {false, error_message("write configuration slot", result)};
  result = api_.commit(handle_);
  if (result != kNvsOk)
    return {false, error_message("commit configuration slot", result)};

  const BlobRead written = read_blob(target_key);
  if (written.status != BlobRead::Status::Present || written.value != canonical_json)
    return {false, written.status == BlobRead::Status::Failed
                       ? written.message
                       : "configuration slot read-back verification failed"};

  result = api_.set_u8(handle_, kActiveKey, target_slot);
  if (result != kNvsOk)
    return {false, error_message("write active slot", result)};
  result = api_.commit(handle_);
  if (result != kNvsOk) {
    const auto restored = restore_marker(active_slot, error_message("commit active slot", result));
    return {false, restored.message};
  }

  std::uint8_t verified_slot = 0U;
  result = api_.get_u8(handle_, kActiveKey, verified_slot);
  if (result != kNvsOk || verified_slot != target_slot) {
    const auto restored =
        restore_marker(active_slot, result == kNvsOk ? "active slot read-back verification failed"
                                                     : error_message("read active slot", result));
    return {false, restored.message};
  }
  const BlobRead verified = read_blob(kSlotKeys[verified_slot]);
  if (verified.status != BlobRead::Status::Present || verified.value != canonical_json) {
    const auto restored =
        restore_marker(active_slot, verified.status == BlobRead::Status::Failed
                                        ? verified.message
                                        : "active configuration read-back verification failed");
    return {false, restored.message};
  }
  return {true, {}};
}

ConfigStoreBackend::ClearResult NvsConfigBackend::clear_override() {
  const auto active_slot = read_active_slot();
  if (!active_slot.has_value()) {
    if (active_slot_error_)
      return {false, *active_slot_error_};
    return {true, {}};
  }

  NvsResult result = api_.erase_key(handle_, kActiveKey);
  if (result != kNvsOk && result != kNvsNotFound)
    return {false, error_message("clear active slot", result)};
  result = api_.commit(handle_);
  if (result != kNvsOk) {
    const auto restored = restore_marker(*active_slot, error_message("commit clear", result));
    return {false, restored.message};
  }

  std::uint8_t ignored = 0U;
  result = api_.get_u8(handle_, kActiveKey, ignored);
  if (result == kNvsNotFound)
    return {true, {}};
  const auto restored = restore_marker(
      *active_slot, result == kNvsOk ? "clear read-back verification failed"
                                     : error_message("read cleared active slot", result));
  return {false, restored.message};
}

NvsConfigBackend::RestoreResult
NvsConfigBackend::restore_marker(const std::optional<std::uint8_t> previous_slot,
                                 std::string message) {
  NvsResult result = kNvsOk;
  if (previous_slot.has_value())
    result = api_.set_u8(handle_, kActiveKey, *previous_slot);
  else {
    result = api_.erase_key(handle_, kActiveKey);
    if (result == kNvsNotFound)
      result = kNvsOk;
  }
  if (result != kNvsOk) {
    message += "; restore failed: ";
    message += error_message("active slot", result);
  } else {
    result = api_.commit(handle_);
    if (result != kNvsOk) {
      message += "; restore commit failed: ";
      message += error_message("active slot", result);
    }
  }
  return {false, std::move(message)};
}

} // namespace controller_config::persisted::internal

namespace controller_config::persisted {
#if defined(ESP_PLATFORM)
namespace {

using internal::kNvsNotFound;
using internal::kNvsOk;
using internal::NvsHandle;
using internal::NvsResult;

class EspNvsApi final : public internal::NvsApi {
public:
  [[nodiscard]] NvsResult get_u8(const NvsHandle handle, const char *const key,
                                 std::uint8_t &value) override {
    return map(nvs_get_u8(static_cast<nvs_handle_t>(handle), key, &value));
  }
  [[nodiscard]] NvsResult set_u8(const NvsHandle handle, const char *const key,
                                 const std::uint8_t value) override {
    return map(nvs_set_u8(static_cast<nvs_handle_t>(handle), key, value));
  }
  [[nodiscard]] NvsResult get_blob(const NvsHandle handle, const char *const key,
                                   void *const buffer, const std::size_t capacity,
                                   std::size_t &length) override {
    std::size_t requested = capacity;
    const NvsResult result = map(nvs_get_blob(static_cast<nvs_handle_t>(handle), key, buffer,
                                              buffer == nullptr ? &length : &requested));
    if (buffer != nullptr && result == kNvsOk)
      length = requested;
    return result;
  }
  [[nodiscard]] NvsResult set_blob(const NvsHandle handle, const char *const key,
                                   const void *const value, const std::size_t length) override {
    return map(nvs_set_blob(static_cast<nvs_handle_t>(handle), key, value, length));
  }
  [[nodiscard]] NvsResult erase_key(const NvsHandle handle, const char *const key) override {
    return map(nvs_erase_key(static_cast<nvs_handle_t>(handle), key));
  }
  [[nodiscard]] NvsResult commit(const NvsHandle handle) override {
    return map(nvs_commit(static_cast<nvs_handle_t>(handle)));
  }
  void close(const NvsHandle handle) noexcept override {
    nvs_close(static_cast<nvs_handle_t>(handle));
  }
  [[nodiscard]] const char *error_name(const NvsResult result) const noexcept override {
    return result == kNvsNotFound ? "ESP_ERR_NVS_NOT_FOUND" : "ESP_ERR_NVS_FAILURE";
  }

private:
  static NvsResult map(const esp_err_t result) noexcept {
    if (result == ESP_OK)
      return kNvsOk;
    if (result == ESP_ERR_NVS_NOT_FOUND)
      return kNvsNotFound;
    return -1;
  }
};

constexpr char kNamespace[] = "mazda_config";

} // namespace

std::unique_ptr<ConfigStoreBackend> make_nvs_config_store_backend() noexcept {
  const esp_err_t init_result = nvs_flash_init();
  if (init_result != ESP_OK)
    return nullptr;

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return nullptr;
  static EspNvsApi api{};
  auto backend = std::unique_ptr<internal::NvsConfigBackend>(
      new (std::nothrow) internal::NvsConfigBackend(api, static_cast<internal::NvsHandle>(handle)));
  if (!backend)
    nvs_close(handle);
  return backend;
}

} // namespace controller_config::persisted

#else

std::unique_ptr<ConfigStoreBackend> make_nvs_config_store_backend() noexcept { return nullptr; }

} // namespace controller_config::persisted

#endif
