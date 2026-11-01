#include "vehicle_profile_runtime/provider.hpp"

#include <cmath>
#include <limits>
#include <new>

#include "provider_access.hpp"
#include "state.hpp"

namespace vehicle_profile_runtime {
namespace {

using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;

[[nodiscard]] bool has_flag(const SignalState &sample, const std::uint8_t flag) noexcept {
  return (sample.flags & flag) != 0U;
}

void set_flag(SignalState &sample, const std::uint8_t flag) noexcept {
  sample.flags = static_cast<std::uint8_t>(sample.flags | flag);
}

void clear_flag(SignalState &sample, const std::uint8_t flag) noexcept {
  sample.flags = static_cast<std::uint8_t>(sample.flags & static_cast<std::uint8_t>(~flag));
}

[[nodiscard]] const SignalFreshnessPolicy *
freshness_for(const ProfileTables &tables, const vehicle_signals::SignalId id) noexcept {
  for (std::size_t index = 0; index < tables.freshness_policy_count; ++index) {
    if (tables.freshness_policies[index].id == id) {
      return &tables.freshness_policies[index];
    }
  }
  return nullptr;
}

[[nodiscard]] std::size_t signal_index(const ProfileTables &tables,
                                       const vehicle_signals::SignalId id) noexcept {
  const SignalMetadata *const metadata = tables.catalog.find(id);
  if (metadata == nullptr) {
    return kMaxRuntimeSignals;
  }
  return static_cast<std::size_t>(metadata - tables.catalog.begin());
}

[[nodiscard]] bool table_well_formed(const ProfileTables &tables) noexcept {
  if (tables.catalog.size() > kMaxRuntimeSignals || !tables.catalog.well_formed() ||
      tables.freshness_policy_count != tables.catalog.size() ||
      (tables.freshness_policy_count != 0U && tables.freshness_policies == nullptr) ||
      tables.dispatch_count > kMaxRuntimeMessages ||
      (tables.dispatch_count != 0U && tables.dispatch == nullptr) ||
      tables.max_decode_results > kMaxRuntimeSignals ||
      (tables.dispatch_count != 0U && tables.max_decode_results == 0U)) {
    return false;
  }

  std::size_t notify_count = 0;
  for (std::size_t index = 0; index < tables.catalog.size(); ++index) {
    const SignalMetadata *const metadata = tables.catalog.at(index);
    if (metadata->capabilities.has(SignalCapability::Notify) &&
        ++notify_count > kMaxNotifyBindings) {
      return false;
    }
  }

  for (std::size_t index = 0; index < tables.freshness_policy_count; ++index) {
    const SignalFreshnessPolicy &policy = tables.freshness_policies[index];
    if (tables.catalog.find(policy.id) == nullptr ||
        (policy.declared ? policy.timeout_ms == 0U : policy.timeout_ms != 0U)) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (tables.freshness_policies[previous].id == policy.id) {
        return false;
      }
    }
  }

  for (std::size_t index = 0; index < tables.dispatch_count; ++index) {
    const DispatchEntry &entry = tables.dispatch[index];
    if (entry.identifier > 0x7ffU || entry.expected_dlc > vehicle_core::kCanClassicPayloadBytes ||
        entry.decode == nullptr || entry.output_ids == nullptr || entry.output_count == 0U ||
        entry.output_count > tables.max_decode_results) {
      return false;
    }
    for (std::size_t output = 0; output < entry.output_count; ++output) {
      if (tables.catalog.find(entry.output_ids[output]) == nullptr ||
          freshness_for(tables, entry.output_ids[output]) == nullptr) {
        return false;
      }
      for (std::size_t previous = 0; previous < output; ++previous) {
        if (entry.output_ids[previous] == entry.output_ids[output]) {
          return false;
        }
      }
      for (std::size_t prior_entry = 0; prior_entry < index; ++prior_entry) {
        const DispatchEntry &prior = tables.dispatch[prior_entry];
        if (prior.identifier == entry.identifier) {
          return false;
        }
        for (std::size_t prior_output = 0; prior_output < prior.output_count; ++prior_output) {
          if (prior.output_ids[prior_output] == entry.output_ids[output]) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

[[nodiscard]] const DispatchEntry *find_dispatch(const ProfileTables &tables,
                                                 const std::uint32_t identifier) noexcept {
  for (std::size_t index = 0; index < tables.dispatch_count; ++index) {
    if (tables.dispatch[index].identifier == identifier) {
      return &tables.dispatch[index];
    }
  }
  return nullptr;
}

[[nodiscard]] vehicle_core::MonotonicTimestamp
freshness_deadline(const vehicle_core::MonotonicTimestamp timestamp_us,
                   const SignalFreshnessPolicy &policy) noexcept {
  using Timestamp = vehicle_core::MonotonicTimestamp;
  const Timestamp timeout_us = static_cast<Timestamp>(policy.timeout_ms) * 1000;
  const Timestamp maximum = std::numeric_limits<Timestamp>::max();
  if (timestamp_us > maximum - timeout_us) {
    return maximum;
  }
  return timestamp_us + timeout_us;
}

[[nodiscard]] Availability availability_at(const SignalState &sample,
                                           const SignalFreshnessPolicy &policy,
                                           const vehicle_core::MonotonicTimestamp now_us) noexcept {
  if (has_flag(sample, kInvalid)) {
    return Availability::Unavailable;
  }
  if (!has_flag(sample, kHasValue)) {
    return Availability::NoData;
  }
  if (!policy.declared) {
    return Availability::FreshnessUnverified;
  }
  return now_us > freshness_deadline(sample.sample_timestamp_us, policy) ? Availability::Stale
                                                                         : Availability::Fresh;
}

void append_unique(std::array<vehicle_signals::SignalId, kMaxRuntimeSignals> &values,
                   std::size_t &count, const vehicle_signals::SignalId id) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (values[index] == id) {
      return;
    }
  }
  if (count < values.size()) {
    values[count++] = id;
  }
}

[[nodiscard]] bool update_watermark(SignalState &sample,
                                    const vehicle_core::MonotonicTimestamp timestamp_us) noexcept {
  if (has_flag(sample, kHasWatermark) && timestamp_us <= sample.watermark_us) {
    return false;
  }
  sample.watermark_us = timestamp_us;
  set_flag(sample, kHasWatermark);
  return true;
}

void mark_invalid(ProfileState &state, const vehicle_signals::SignalId id,
                  const vehicle_core::MonotonicTimestamp timestamp_us,
                  ProviderProcessResult &result) noexcept {
  const std::size_t index = signal_index(state.tables, id);
  if (index >= state.tables.catalog.size() || index >= state.signals.size()) {
    return;
  }
  SignalState &sample = state.signals[index];
  if (!update_watermark(sample, timestamp_us)) {
    return;
  }
  set_flag(sample, kInvalid);
  append_unique(result.changed, result.changed_count, id);
  ++state.generation;
}

void service_due_locked(ProfileState &state, const vehicle_core::MonotonicTimestamp now_us,
                        ProviderProcessResult &result) noexcept {
  bool changed = false;
  for (std::size_t index = 0; index < state.tables.catalog.size(); ++index) {
    const SignalMetadata *const metadata = state.tables.catalog.at(index);
    const SignalFreshnessPolicy *const policy = freshness_for(state.tables, metadata->id);
    SignalState &sample = state.signals[index];
    if (policy == nullptr || !policy->declared || !has_flag(sample, kHasValue) ||
        has_flag(sample, kInvalid) || has_flag(sample, kDueReported) ||
        now_us <= freshness_deadline(sample.sample_timestamp_us, *policy)) {
      continue;
    }
    set_flag(sample, kDueReported);
    append_unique(result.due, result.due_count, metadata->id);
    changed = true;
  }
  if (changed) {
    ++state.generation;
  }
}

[[nodiscard]] bool result_list_well_formed(const ProfileState &state, const DispatchEntry &entry,
                                           const std::size_t result_count) noexcept {
  if (result_count != entry.output_count || result_count > state.tables.max_decode_results) {
    return false;
  }
  std::array<bool, kMaxRuntimeSignals> seen{};
  for (std::size_t index = 0; index < result_count; ++index) {
    const vehicle_signals::SignalId id = state.decode_results[index].id;
    std::size_t output_index = entry.output_count;
    for (std::size_t output = 0; output < entry.output_count; ++output) {
      if (entry.output_ids[output] == id) {
        output_index = output;
        break;
      }
    }
    if (output_index == entry.output_count || seen[output_index]) {
      return false;
    }
    seen[output_index] = true;
  }
  return true;
}

[[nodiscard]] bool accepted_result(const SignalMetadata &metadata,
                                   const DecodeResult &result) noexcept {
  if (!result.valid || !metadata.accepts(result.value)) {
    return !result.valid;
  }
  const auto number = result.value.as_number();
  return !number.has_value() || std::isfinite(*number);
}

void commit_valid(ProfileState &state, const SignalMetadata &metadata, const DecodeResult &result,
                  const vehicle_core::MonotonicTimestamp timestamp_us,
                  const vehicle_core::MonotonicTimestamp now_us,
                  ProviderProcessResult &outcome) noexcept {
  const std::size_t index = signal_index(state.tables, metadata.id);
  if (index >= state.tables.catalog.size() || index >= state.signals.size()) {
    return;
  }
  SignalState &sample = state.signals[index];
  if (!update_watermark(sample, timestamp_us)) {
    return;
  }

  const SignalFreshnessPolicy *const policy = freshness_for(state.tables, metadata.id);
  const bool previously_unusable =
      has_flag(sample, kInvalid) || has_flag(sample, kDueReported) ||
      (has_flag(sample, kHasValue) && policy != nullptr &&
       availability_at(sample, *policy, now_us) == Availability::Stale);
  sample.value = result.value;
  sample.sample_timestamp_us = timestamp_us;
  set_flag(sample, kHasValue);
  clear_flag(sample, kInvalid | kDueReported);
  append_unique(outcome.changed, outcome.changed_count, metadata.id);

  if (previously_unusable && policy != nullptr &&
      availability_at(sample, *policy, now_us) != Availability::Stale) {
    append_unique(outcome.recovered, outcome.recovered_count, metadata.id);
  }
  ++state.generation;
}

} // namespace

ProfileState &ProviderAccess::state(ProfileProvider &provider) noexcept {
  return *std::launder(reinterpret_cast<ProfileState *>(provider.storage_));
}

const ProfileState &ProviderAccess::state(const ProfileProvider &provider) noexcept {
  return *std::launder(reinterpret_cast<const ProfileState *>(provider.storage_));
}

ProfileProvider::ProfileProvider(const ProfileTables tables, vehicle_core::MonotonicClock &clock,
                                 const PublicationSynchronization synchronization) noexcept {
  static_assert(sizeof(ProfileState) <= kProviderStorageBytes,
                "profile provider private storage budget is too small");
  (void)new (static_cast<void *>(storage_)) ProfileState{tables, clock, synchronization};
  ProviderAccess::state(*this).valid_tables =
      table_well_formed(tables) && synchronization.well_formed();
  if (ProviderAccess::state(*this).valid_tables) {
    initialize_notification_channels(ProviderAccess::state(*this));
  }
}

ProfileProvider::~ProfileProvider() noexcept { ProviderAccess::state(*this).~ProfileState(); }

vehicle_signals::SignalCatalogView ProfileProvider::catalog() const noexcept {
  return ProviderAccess::state(*this).tables.catalog;
}

vehicle_signals::SignalResult<SignalReading>
ProfileProvider::read(const vehicle_signals::SignalId id) const noexcept {
  using Result = vehicle_signals::SignalResult<SignalReading>;
  const ProfileState &provider_state = ProviderAccess::state(*this);
  const SignalMetadata *const metadata = provider_state.tables.catalog.find(id);
  if (metadata == nullptr) {
    return Result::failure(SignalStatus::InvalidSignal);
  }
  if (!metadata->capabilities.has(SignalCapability::Read)) {
    return Result::failure(SignalStatus::UnsupportedCapability);
  }
  if (!provider_state.valid_tables) {
    return Result::failure(SignalStatus::Faulted);
  }

  const std::size_t index = signal_index(provider_state.tables, id);
  if (index >= provider_state.tables.catalog.size() || index >= provider_state.signals.size()) {
    return Result::failure(SignalStatus::Faulted);
  }

  SignalState sample{};
  {
    const PublicationGuard lock{provider_state.synchronization};
    sample = provider_state.signals[index];
  }
  const vehicle_core::MonotonicTimestamp now_us = provider_state.clock->now();
  const SignalFreshnessPolicy *const policy = freshness_for(provider_state.tables, id);
  if (policy == nullptr) {
    return Result::failure(SignalStatus::Faulted);
  }

  SignalReading reading{};
  reading.validation = metadata->validation;
  reading.availability = availability_at(sample, *policy, now_us);
  if (has_flag(sample, kHasValue)) {
    reading.value = sample.value;
  }
  return Result::success(reading);
}

ProviderProcessResult ProviderAccess::service(ProfileProvider &provider) noexcept {
  ProviderProcessResult result{};
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables) {
    result.status = ProviderProcessStatus::Faulted;
    return result;
  }
  const vehicle_core::MonotonicTimestamp now_us = provider_state.clock->now();
  const PublicationGuard lock{provider_state.synchronization};
  service_due_locked(provider_state, now_us, result);
  return result;
}

ProviderProcessResult ProviderAccess::fault(ProfileProvider &provider) noexcept {
  ProviderProcessResult result{};
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables) {
    result.status = ProviderProcessStatus::Faulted;
    return result;
  }
  const vehicle_core::MonotonicTimestamp now_us = provider_state.clock->now();
  const PublicationGuard lock{provider_state.synchronization};
  service_due_locked(provider_state, now_us, result);
  for (std::size_t index = 0; index < provider_state.tables.catalog.size(); ++index) {
    const SignalMetadata *const metadata = provider_state.tables.catalog.at(index);
    SignalState &sample = provider_state.signals[index];
    const bool was_invalid = has_flag(sample, kInvalid);
    if (!has_flag(sample, kHasWatermark) || now_us > sample.watermark_us) {
      sample.watermark_us = now_us;
      set_flag(sample, kHasWatermark);
    }
    set_flag(sample, kInvalid);
    if (!was_invalid) {
      append_unique(result.changed, result.changed_count, metadata->id);
      ++provider_state.generation;
    }
  }
  result.status = ProviderProcessStatus::Faulted;
  return result;
}

ProviderProcessResult ProviderAccess::reset(ProfileProvider &provider) noexcept {
  ProviderProcessResult result{};
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables) {
    result.status = ProviderProcessStatus::Faulted;
    return result;
  }
  const vehicle_core::MonotonicTimestamp now_us = provider_state.clock->now();
  const PublicationGuard lock{provider_state.synchronization};
  for (std::size_t index = 0; index < provider_state.tables.catalog.size(); ++index) {
    const SignalMetadata *const metadata = provider_state.tables.catalog.at(index);
    SignalState &sample = provider_state.signals[index];
    sample.value = {};
    sample.sample_timestamp_us = 0;
    if (!has_flag(sample, kHasWatermark) || now_us > sample.watermark_us) {
      sample.watermark_us = now_us;
    }
    sample.flags = kHasWatermark;
    append_unique(result.changed, result.changed_count, metadata->id);
  }
  provider_state.has_receive_watermark = false;
  ++provider_state.generation;
  result.status = ProviderProcessStatus::Applied;
  return result;
}

ProviderProcessResult ProviderAccess::process(ProfileProvider &provider,
                                              const vehicle_core::RawCanFrame &frame) noexcept {
  ProviderProcessResult result{};
  ProfileState &provider_state = state(provider);
  if (!provider_state.valid_tables) {
    result.status = ProviderProcessStatus::Faulted;
    return result;
  }

  const vehicle_core::MonotonicTimestamp receive_now_us = provider_state.clock->now();
  const DispatchEntry *const entry = find_dispatch(provider_state.tables, frame.identifier);
  {
    const PublicationGuard lock{provider_state.synchronization};
    if (!provider_state.has_receive_watermark ||
        receive_now_us > provider_state.receive_watermark_us) {
      provider_state.receive_watermark_us = receive_now_us;
      provider_state.has_receive_watermark = true;
    }
    service_due_locked(provider_state, receive_now_us, result);
  }
  if (entry == nullptr) {
    result.status = ProviderProcessStatus::Ignored;
    return result;
  }

  const bool valid_shape =
      frame.is_valid() && frame.identifier_format == vehicle_core::CanIdentifierFormat::Standard &&
      !frame.remote_request && frame.identifier <= 0x7ffU && frame.dlc == entry->expected_dlc;
  if (!valid_shape) {
    const PublicationGuard lock{provider_state.synchronization};
    for (std::size_t output = 0; output < entry->output_count; ++output) {
      mark_invalid(provider_state, entry->output_ids[output], frame.timestamp_us, result);
    }
    result.status = ProviderProcessStatus::Malformed;
    return result;
  }

  provider_state.decode_results.fill({});
  std::size_t result_count = 0;
  entry->decode(frame, provider_state.decode_results.data(),
                provider_state.tables.max_decode_results, result_count);
  if (!result_list_well_formed(provider_state, *entry, result_count)) {
    const PublicationGuard lock{provider_state.synchronization};
    for (std::size_t output = 0; output < entry->output_count; ++output) {
      mark_invalid(provider_state, entry->output_ids[output], frame.timestamp_us, result);
    }
    result.status = ProviderProcessStatus::InvalidResults;
    return result;
  }

  const PublicationGuard lock{provider_state.synchronization};
  bool invalid_result = false;
  for (std::size_t index = 0; index < result_count; ++index) {
    const DecodeResult &decoded = provider_state.decode_results[index];
    const SignalMetadata *const metadata = provider_state.tables.catalog.find(decoded.id);
    if (metadata == nullptr) {
      invalid_result = true;
      continue;
    }
    if (!accepted_result(*metadata, decoded)) {
      mark_invalid(provider_state, decoded.id, frame.timestamp_us, result);
      invalid_result = true;
      continue;
    }
    if (decoded.valid) {
      commit_valid(provider_state, *metadata, decoded, frame.timestamp_us, receive_now_us, result);
    } else {
      mark_invalid(provider_state, decoded.id, frame.timestamp_us, result);
    }
  }
  result.status =
      invalid_result ? ProviderProcessStatus::InvalidResults : ProviderProcessStatus::Applied;
  return result;
}

} // namespace vehicle_profile_runtime
