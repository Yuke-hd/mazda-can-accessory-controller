#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>

#include "mazda/decoder.hpp"
#include "mazda/state.hpp"
#include "vehicle_profile_runtime.hpp"

namespace {

const vehicle_profile_runtime::DispatchEntry *find_entry(const std::uint32_t identifier) {
  for (const auto &entry : vehicle_profile::kDispatchTable) {
    if (entry.identifier == identifier) {
      return &entry;
    }
  }
  return nullptr;
}

const vehicle_profile_runtime::DecodeResult *
find_result(const std::array<vehicle_profile_runtime::DecodeResult, 4> &results,
            const std::size_t count, const vehicle_signals::SignalId id) {
  for (std::size_t index = 0; index < count; ++index) {
    if (results[index].id == id) {
      return &results[index];
    }
  }
  return nullptr;
}

void write_number(const vehicle_profile_runtime::DecodeResult *result) {
  const bool valid = result != nullptr && result->valid;
  const auto value = valid ? result->value.as_number() : std::nullopt;
  std::cout << (result != nullptr ? 1 : 0) << '\t' << (valid && value.has_value() ? 1 : 0) << '\t'
            << (value.has_value() ? *value : 0.0F) << '\t';
}

void write_enumeration(const vehicle_profile_runtime::DecodeResult *result) {
  const bool valid = result != nullptr && result->valid;
  const auto value = valid ? result->value.as_enumeration() : std::nullopt;
  std::cout << (result != nullptr ? 1 : 0) << '\t' << (valid && value.has_value() ? 1 : 0) << '\t'
            << (value.has_value() ? *value : 0U) << '\t';
}

const char *status_name(const mazda::candidate::DecodeStatus status) {
  switch (status) {
  case mazda::candidate::DecodeStatus::Decoded:
    return "decoded";
  case mazda::candidate::DecodeStatus::Malformed:
    return "malformed";
  case mazda::candidate::DecodeStatus::Ignored:
    return "ignored";
  }
  return "unknown";
}

} // namespace

int main() {
  const auto *const rpm_metadata = vehicle_profile::kCatalog.find("vehicle.engine_rpm");
  const auto *const pedal_metadata =
      vehicle_profile::kCatalog.find("vehicle.accelerator_pedal_position");
  const auto *const selector_metadata = vehicle_profile::kCatalog.find("vehicle.selector_position");
  const auto *const gear_metadata = vehicle_profile::kCatalog.find("vehicle.actual_gear");
  if (rpm_metadata == nullptr || pedal_metadata == nullptr || selector_metadata == nullptr ||
      gear_metadata == nullptr) {
    return 1;
  }

  std::size_t case_index = 0;
  std::uint32_t identifier = 0;
  while (std::cin >> case_index >> identifier) {
    vehicle_core::RawCanFrame frame{};
    frame.identifier = identifier;
    frame.timestamp_us = 0;
    frame.dlc = 8;
    for (auto &byte : frame.data) {
      unsigned int value = 0;
      if (!(std::cin >> value) || value > 0xFFU) {
        return 2;
      }
      byte = static_cast<std::uint8_t>(value);
    }

    const auto *const entry = find_entry(identifier);
    if (entry == nullptr || entry->decode == nullptr) {
      return 3;
    }
    std::array<vehicle_profile_runtime::DecodeResult, 4> generated{};
    std::size_t generated_count = 0;
    entry->decode(frame, generated.data(), generated.size(), generated_count);

    mazda::VehicleState native{};
    mazda::candidate::DecodeStatus native_status = mazda::candidate::DecodeStatus::Ignored;
    if (identifier == 0x202U) {
      native_status = mazda::candidate::decode_engine_data(frame, native);
    } else if (identifier == 0x228U) {
      native_status = mazda::candidate::decode_gear(frame, native);
    } else {
      return 4;
    }

    std::cout << case_index << '\t';
    write_number(find_result(generated, generated_count, rpm_metadata->id));
    write_number(find_result(generated, generated_count, pedal_metadata->id));
    write_enumeration(find_result(generated, generated_count, selector_metadata->id));
    write_enumeration(find_result(generated, generated_count, gear_metadata->id));
    std::cout << status_name(native_status) << '\t' << (native.engine_rpm.is_valid() ? 1 : 0)
              << '\t' << (native.engine_rpm.is_valid() ? native.engine_rpm.value : 0.0F) << '\t'
              << (native.accelerator_pedal_position.is_valid() ? 1 : 0) << '\t'
              << (native.accelerator_pedal_position.is_valid()
                      ? native.accelerator_pedal_position.value
                      : 0.0F)
              << '\t' << (native.selector_position.is_valid() ? 1 : 0) << '\t'
              << (native.selector_position.is_valid()
                      ? static_cast<unsigned int>(native.selector_position.value)
                      : 0U)
              << '\t' << (native.actual_gear.is_valid() ? 1 : 0) << '\t'
              << (native.actual_gear.is_valid()
                      ? static_cast<unsigned int>(native.actual_gear.value)
                      : 0U)
              << '\n';
  }
  return std::cin.eof() ? 0 : 5;
}
