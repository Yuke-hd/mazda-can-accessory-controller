#include "gvret/replay_acquisition_source.hpp"

#include <utility>

namespace gvret {

ReplayAcquisitionSource::ReplayAcquisitionSource(std::vector<TimedCanFrame> frames,
                                                 ReplayClock &clock)
    : frames_(std::move(frames)), clock_(clock) {}

vehicle_telemetry::StatusResult ReplayAcquisitionSource::start() noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  if (running_) {
    return {vehicle_telemetry::ResultCode::AlreadyRunning};
  }
  next_frame_ = 0;
  statistics_ = {};
  running_ = true;
  return {vehicle_telemetry::ResultCode::Ok};
}

vehicle_telemetry::StatusResult ReplayAcquisitionSource::stop() noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  running_ = false;
  return {vehicle_telemetry::ResultCode::Ok};
}

vehicle_telemetry::ReceiveStatus
ReplayAcquisitionSource::receive(vehicle_core::RawCanFrame &frame,
                                 const std::uint32_t /* timeout_ms */) noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  if (!running_) {
    return vehicle_telemetry::ReceiveStatus::NotStarted;
  }
  if (next_frame_ == frames_.size() || frames_[next_frame_].relative_time_us > clock_.now()) {
    return vehicle_telemetry::ReceiveStatus::Timeout;
  }
  frame = frames_[next_frame_].frame;
  ++next_frame_;
  ++statistics_.frames_received;
  return vehicle_telemetry::ReceiveStatus::Frame;
}

vehicle_telemetry::AcquisitionStatistics ReplayAcquisitionSource::statistics() const noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  return statistics_;
}

bool ReplayAcquisitionSource::end_of_stream() const noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  return next_frame_ == frames_.size();
}

} // namespace gvret
