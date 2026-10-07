#include "mazda/debug_telemetry_internal.hpp"

#include "mazda/definitions.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace mazda::internal {
namespace {

constexpr std::uint32_t kTurnSwitchIdentifier = candidate::kTurnSwitchId;
// SavvyCAN/GVRET names payload bytes D1..D8. Issue #208's D4 alive nibble is
// therefore RawCanFrame::data[3], not data[4].
constexpr std::size_t kAliveCounterByte = 3;
constexpr std::uint8_t kInvalidCounter = 0xffU;

template <typename T> T non_negative_delta(const T newer, const T older) noexcept {
  return newer >= older ? newer - older : 0;
}

std::int64_t signed_delta(const std::uint64_t newer, const std::uint64_t older) noexcept {
  if (newer >= older) {
    const auto delta = newer - older;
    return delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(delta);
  }
  const auto delta = older - newer;
  return delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::min()
             : -static_cast<std::int64_t>(delta);
}

} // namespace

void DebugRecorder::reset() noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  current_ = {};
  stale_ = {};
  stale_.enabled = true;
  published_current_ = {};
  published_stale_ = stale_;
  pending_ = {};
  pending_frame_ = false;
  frame_count_ = 0;
  counter_gap_count_ = 0;
  update_not_advanced_count_ = 0;
  previous_counter_initialized_ = false;
  last_counter_ = 0;
  last_frame_timestamp_initialized_ = false;
  last_frame_timestamp_us_ = 0;
  availability_initialized_ = false;
  previous_availability_ = Availability::NoData;
  snapshot_read_drops_.store(0, std::memory_order_relaxed);
  recorder_write_contention_.store(0, std::memory_order_relaxed);
}

bool DebugRecorder::record_processed(const vehicle_core::RawCanFrame &frame,
                                     const vehicle_telemetry::ProcessStatus status,
                                     const vehicle_core::MonotonicTimestamp processing_timestamp_us,
                                     const bool update_not_advanced) noexcept {
  if (frame.identifier != kTurnSwitchIdentifier)
    return true;

  pending_ = {};
  pending_frame_ = true;
  ++frame_count_;
  pending_.enabled = true;
  pending_.has_frame = true;
  pending_.frame_count = frame_count_;
  pending_.identifier = frame.identifier;
  pending_.frame_timestamp_us = frame.timestamp_us;
  pending_.processing_timestamp_us = processing_timestamp_us;
  pending_.rx_process_latency_us = non_negative_delta(processing_timestamp_us, frame.timestamp_us);
  pending_.process_status = static_cast<std::uint8_t>(status);

  pending_.has_previous_frame_timestamp = last_frame_timestamp_initialized_;
  pending_.previous_frame_timestamp_us = last_frame_timestamp_us_;
  pending_.interarrival_us = last_frame_timestamp_initialized_
                                 ? non_negative_delta(frame.timestamp_us, last_frame_timestamp_us_)
                                 : 0;
  last_frame_timestamp_initialized_ = true;
  last_frame_timestamp_us_ = frame.timestamp_us;

  // Counter continuity is evidence about a payload-bearing standard CAN
  // frame. Extended identifiers and remote requests can carry an identifier
  // that numerically matches 0x091, but they do not provide the reviewed D4
  // observation and must not create false gaps.
  pending_.counter_valid = frame.identifier_format == vehicle_core::CanIdentifierFormat::Standard &&
                           !frame.remote_request && frame.dlc > kAliveCounterByte;
  pending_.counter_sequential = false;
  if (pending_.counter_valid) {
    const auto counter = static_cast<std::uint8_t>(frame.data[kAliveCounterByte] & 0x0fU);
    pending_.counter = counter;
    pending_.previous_counter = previous_counter_initialized_ ? last_counter_ : kInvalidCounter;
    pending_.expected_counter = previous_counter_initialized_
                                    ? static_cast<std::uint8_t>((last_counter_ + 1U) & 0x0fU)
                                    : kInvalidCounter;
    pending_.counter_sequential =
        previous_counter_initialized_ && counter == pending_.expected_counter;
    if (previous_counter_initialized_ && !pending_.counter_sequential)
      ++counter_gap_count_;
    previous_counter_initialized_ = true;
    last_counter_ = counter;
  } else {
    pending_.counter = 0;
    pending_.previous_counter = kInvalidCounter;
    pending_.expected_counter = kInvalidCounter;
  }

  pending_.counter_gap_count = counter_gap_count_;
  pending_.update_not_advanced = update_not_advanced;
  if (update_not_advanced)
    ++update_not_advanced_count_;
  pending_.update_not_advanced_count = update_not_advanced_count_;
  return true;
}

bool DebugRecorder::record_published(
    const VehicleState &state, const Diagnostics &diagnostics,
    const vehicle_core::MonotonicTimestamp now_us,
    const vehicle_core::MonotonicTimestamp publication_timestamp_us,
    const vehicle_core::MonotonicTimestamp esp_timer_us,
    const vehicle_core::MonotonicTimestamp steady_clock_us,
    const std::optional<vehicle_core::MonotonicTimestamp> transport_last_frame_us,
    const vehicle_core::MonotonicTimestamp diagnostic_sample_timestamp_us,
    const DebugSampleSource diagnostic_sample_source) noexcept {
  current_.enabled = true;
  stale_.enabled = true;
  if (pending_frame_) {
    current_.enabled = pending_.enabled;
    current_.has_frame = pending_.has_frame;
    current_.frame_count = pending_.frame_count;
    current_.identifier = pending_.identifier;
    current_.counter = pending_.counter;
    current_.previous_counter = pending_.previous_counter;
    current_.expected_counter = pending_.expected_counter;
    current_.counter_valid = pending_.counter_valid;
    current_.counter_sequential = pending_.counter_sequential;
    current_.counter_gap_count = pending_.counter_gap_count;
    current_.has_previous_frame_timestamp = pending_.has_previous_frame_timestamp;
    current_.previous_frame_timestamp_us = pending_.previous_frame_timestamp_us;
    current_.frame_timestamp_us = pending_.frame_timestamp_us;
    current_.interarrival_us = pending_.interarrival_us;
    current_.processing_timestamp_us = pending_.processing_timestamp_us;
    current_.rx_process_latency_us = pending_.rx_process_latency_us;
    current_.process_status = pending_.process_status;
    current_.update_not_advanced = pending_.update_not_advanced;
    current_.update_not_advanced_count = pending_.update_not_advanced_count;
    current_.publication_timestamp_us = publication_timestamp_us;
    current_.process_publish_latency_us =
        non_negative_delta(publication_timestamp_us, current_.processing_timestamp_us);
    pending_frame_ = false;
  }
  current_.now_us = now_us;
  current_.esp_timer_us = esp_timer_us;
  current_.steady_clock_us = steady_clock_us;
  current_.clock_delta_us = signed_delta(esp_timer_us, steady_clock_us);

  current_.diagnostics = diagnostics;
  current_.diagnostics_sample_timestamp_us = diagnostic_sample_timestamp_us;
  current_.diagnostics_sample_source = diagnostic_sample_source;
  current_.has_transport_last_frame = transport_last_frame_us.has_value();
  current_.transport_last_frame_age_us = transport_last_frame_us.has_value()
                                             ? non_negative_delta(now_us, *transport_last_frame_us)
                                             : 0;
  ++current_.recorder_publications;

  const auto reading = state.reading_at(state.turn_state, kTurnSwitchIdentifier, now_us,
                                        ValidationStatus::Reference, diagnostics.transport);
  current_.availability = reading.availability;
  current_.signal_has_update = state.turn_state.has_value;
  current_.signal_last_update_us = state.turn_state.last_update_us;
  current_.signal_age_us =
      state.turn_state.has_value ? non_negative_delta(now_us, state.turn_state.last_update_us) : 0;
  current_.has_freshness_timeout = state.turn_state.freshness_timeout_us.has_value();
  current_.freshness_timeout_us = state.turn_state.freshness_timeout_us.value_or(0);

  const bool stale_transition = availability_initialized_ &&
                                previous_availability_ == Availability::Fresh &&
                                current_.availability == Availability::Stale;
  const bool recovered = availability_initialized_ &&
                         previous_availability_ == Availability::Stale &&
                         current_.availability == Availability::Fresh;
  availability_initialized_ = true;
  previous_availability_ = current_.availability;
  if (stale_transition) {
    ++current_.stale_transition_count;
    stale_ = current_;
  }
  if (recovered)
    ++current_.recovery_count;
  return publish_cache();
}

bool DebugRecorder::publish_cache() noexcept {
  std::unique_lock<std::mutex> lock{mutex_, std::try_to_lock};
  if (!lock.owns_lock()) {
    recorder_write_contention_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  published_current_ = current_;
  published_stale_ = stale_;
  return true;
}

DebugSnapshot DebugRecorder::snapshot() const noexcept {
  std::unique_lock<std::mutex> lock{mutex_, std::try_to_lock};
  if (!lock.owns_lock()) {
    snapshot_read_drops_.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  auto result = published_current_;
  result.snapshot_read_drops = snapshot_read_drops_.load(std::memory_order_relaxed);
  result.recorder_write_contention = recorder_write_contention_.load(std::memory_order_relaxed);
  return result;
}

DebugSnapshot DebugRecorder::stale_snapshot() const noexcept {
  std::unique_lock<std::mutex> lock{mutex_, std::try_to_lock};
  if (!lock.owns_lock()) {
    snapshot_read_drops_.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  auto result = published_stale_;
  result.snapshot_read_drops = snapshot_read_drops_.load(std::memory_order_relaxed);
  result.recorder_write_contention = recorder_write_contention_.load(std::memory_order_relaxed);
  return result;
}

} // namespace mazda::internal
