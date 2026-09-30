#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "controller_config/persisted/config_store.hpp"

namespace controller_config::persisted::internal {

using NvsHandle = std::uint32_t;
using NvsResult = std::int32_t;

inline constexpr NvsResult kNvsOk = 0;
inline constexpr NvsResult kNvsNotFound = 1;

// A tiny seam around the NVS operations used by the adapter. The ESP-IDF
// implementation stays in nvs_config_store.cpp; host tests can model NVS's
// immediate set/erase behavior and commit failures without exposing handles in
// the public configuration API.
class NvsApi {
public:
  virtual ~NvsApi() = default;

  [[nodiscard]] virtual NvsResult get_u8(NvsHandle handle, const char *key,
                                         std::uint8_t &value) = 0;
  [[nodiscard]] virtual NvsResult set_u8(NvsHandle handle, const char *key, std::uint8_t value) = 0;
  [[nodiscard]] virtual NvsResult get_blob(NvsHandle handle, const char *key, void *buffer,
                                           std::size_t capacity, std::size_t &length) = 0;
  [[nodiscard]] virtual NvsResult set_blob(NvsHandle handle, const char *key, const void *value,
                                           std::size_t length) = 0;
  [[nodiscard]] virtual NvsResult erase_key(NvsHandle handle, const char *key) = 0;
  [[nodiscard]] virtual NvsResult commit(NvsHandle handle) = 0;
  virtual void close(NvsHandle handle) noexcept = 0;
  [[nodiscard]] virtual const char *error_name(NvsResult result) const noexcept = 0;
};

class NvsConfigBackend final : public ConfigStoreBackend {
public:
  NvsConfigBackend(NvsApi &api, NvsHandle handle) noexcept : api_{api}, handle_{handle} {}

  ~NvsConfigBackend() override;

  [[nodiscard]] ReadResult read_override() override;
  [[nodiscard]] WriteResult write_override(std::string_view canonical_json) override;
  [[nodiscard]] ClearResult clear_override() override;

private:
  struct BlobRead final {
    enum class Status : std::uint8_t { Missing, Present, Failed };

    Status status{Status::Missing};
    std::string value{};
    std::string message{};
  };

  struct RestoreResult final {
    bool ok{false};
    std::string message{};
  };

  [[nodiscard]] std::optional<std::uint8_t> read_active_slot();
  [[nodiscard]] BlobRead read_blob(const char *key) const;
  [[nodiscard]] RestoreResult restore_marker(std::optional<std::uint8_t> previous_slot,
                                             std::string message);
  [[nodiscard]] std::string error_message(const char *operation, NvsResult result) const;

  NvsApi &api_;
  NvsHandle handle_;
  std::optional<std::string> active_slot_error_{};
};

} // namespace controller_config::persisted::internal
