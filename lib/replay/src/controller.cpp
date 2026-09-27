#include "replay/controller.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

#include "action_engine/engine.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"
#include "replay/acquisition_source.hpp"
#include "signal_observer_fan_out.hpp"
#include "vehicle_telemetry/receive.hpp"

namespace replay {
namespace {

class GatedReplaySource final : public vehicle_telemetry::AcquisitionSource {
public:
  enum class Grant : std::uint8_t { Receive, Timeout, Fault };

  explicit GatedReplaySource(ReplayAcquisitionSource &source) noexcept : source_(&source) {}

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    const auto result = source_->start();
    if (!result.ok())
      return result;
    std::lock_guard<std::mutex> lock{mutex_};
    running_ = true;
    cancelled_ = false;
    grant_pending_ = false;
    last_receive_ = vehicle_telemetry::ReceiveStatus::Timeout;
    return result;
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      running_ = false;
      cancelled_ = true;
      grant_pending_ = false;
    }
    ready_.notify_all();
    return source_->stop();
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, std::uint32_t timeout_ms) noexcept override {
    Grant grant = Grant::Timeout;
    {
      std::unique_lock<std::mutex> lock{mutex_};
      ready_.wait(lock, [this] { return grant_pending_ || cancelled_ || !running_; });
      if (!running_)
        return vehicle_telemetry::ReceiveStatus::NotStarted;
      if (cancelled_) {
        last_receive_ = vehicle_telemetry::ReceiveStatus::Timeout;
        return last_receive_;
      }
      grant = grant_;
      grant_pending_ = false;
    }

    vehicle_telemetry::ReceiveStatus result = vehicle_telemetry::ReceiveStatus::Timeout;
    switch (grant) {
    case Grant::Receive:
      result = source_->receive(frame, timeout_ms);
      break;
    case Grant::Timeout:
      result = vehicle_telemetry::ReceiveStatus::Timeout;
      break;
    case Grant::Fault:
      result = vehicle_telemetry::ReceiveStatus::Fault;
      break;
    }
    {
      std::lock_guard<std::mutex> lock{mutex_};
      last_receive_ = result;
    }
    return result;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    return source_->statistics();
  }

  [[nodiscard]] bool grant(const Grant grant) noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!running_ || cancelled_ || grant_pending_)
      return false;
    grant_ = grant;
    grant_pending_ = true;
    ready_.notify_one();
    return true;
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus last_receive() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return last_receive_;
  }

  [[nodiscard]] bool end_of_stream() const noexcept { return source_->end_of_stream(); }

private:
  ReplayAcquisitionSource *source_;
  mutable std::mutex mutex_{};
  std::condition_variable ready_{};
  Grant grant_{Grant::Timeout};
  vehicle_telemetry::ReceiveStatus last_receive_{vehicle_telemetry::ReceiveStatus::Timeout};
  bool running_{false};
  bool cancelled_{false};
  bool grant_pending_{false};
};

class PublicationBarrier final : public mazda::internal::HostPublicationControl {
public:
  void publication_completed() noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    if (cancelled_)
      return;
    const std::uint64_t epoch = ++completed_;
    changed_.notify_all();
    changed_.wait(lock, [this, epoch] { return cancelled_ || released_ >= epoch; });
  }

  [[nodiscard]] std::optional<std::uint64_t> prepare_step() noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    if (cancelled_)
      return std::nullopt;
    if (completed_ > released_) {
      released_ = completed_;
      changed_.notify_all();
    }
    return completed_ + 1;
  }

  [[nodiscard]] bool wait_for(const std::uint64_t epoch,
                              const vehicle_core::Microseconds timeout_us) noexcept {
    std::unique_lock<std::mutex> lock{mutex_};
    const auto timeout = std::chrono::microseconds{timeout_us};
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::steady_clock::time_point::max() - now;
    const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(remaining);
    const auto deadline =
        timeout <= remaining_us
            ? now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)
            : std::chrono::steady_clock::time_point::max();
    return changed_.wait_until(lock, deadline,
                               [this, epoch] { return cancelled_ || completed_ >= epoch; }) &&
           !cancelled_ && completed_ >= epoch;
  }

  void cancel() noexcept {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      cancelled_ = true;
      released_ = completed_;
    }
    changed_.notify_all();
  }

private:
  std::mutex mutex_{};
  std::condition_variable changed_{};
  std::uint64_t completed_{0};
  std::uint64_t released_{0};
  bool cancelled_{false};
};

[[nodiscard]] ReplayControllerStatus map_telemetry_status(const mazda::ResultCode status) noexcept {
  switch (status) {
  case mazda::ResultCode::Ok:
    return ReplayControllerStatus::Ok;
  case mazda::ResultCode::InvalidConfiguration:
    return ReplayControllerStatus::ConfigurationFailed;
  case mazda::ResultCode::InvalidState:
  case mazda::ResultCode::AlreadyRunning:
  case mazda::ResultCode::NotRunning:
    return ReplayControllerStatus::InvalidState;
  case mazda::ResultCode::Timeout:
    return ReplayControllerStatus::SynchronizationTimeout;
  case mazda::ResultCode::CapacityExceeded:
  case mazda::ResultCode::InvalidSubscription:
  case mazda::ResultCode::Faulted:
    return ReplayControllerStatus::TelemetryFault;
  }
  return ReplayControllerStatus::TelemetryFault;
}

[[nodiscard]] bool
valid_synchronization_timeout(const vehicle_core::Microseconds timeout_us) noexcept {
  if (timeout_us == 0)
    return false;

  using TimeoutRep = std::chrono::microseconds::rep;
  using ComparisonType = std::common_type_t<vehicle_core::Microseconds, TimeoutRep>;
  return static_cast<ComparisonType>(timeout_us) <=
         static_cast<ComparisonType>(std::numeric_limits<TimeoutRep>::max());
}

} // namespace

class ReplayController::Implementation final {
public:
  Implementation(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock, OutputStage &output,
                 SignalObservers observers,
                 const vehicle_core::Microseconds synchronization_timeout_us)
      : clock_(&clock), source_(std::move(frames), clock), gated_source_(source_),
        signal_provider_(telemetry_), engine_(signal_provider_),
        observers_(signal_provider_, clock, std::move(observers)), output_(&output),
        synchronization_timeout_us_(synchronization_timeout_us) {
    mazda::internal::VehicleTelemetryAccess::emplace_host_service(
        telemetry_, clock, gated_source_, telemetry_lighting_, {}, {&publication_barrier_, true});
    configured_ = output_->configure(engine_);
  }

  ~Implementation() noexcept {
    if (state_ == State::Running || state_ == State::Failed) {
      // The controlled source and publication wait are both cancelled by
      // stop(). A second bounded attempt covers Runtime's retryable-stop
      // contract without sleeping on replay time.
      if (stop() == ReplayControllerStatus::SynchronizationTimeout)
        (void)stop();
    }
  }

  [[nodiscard]] ReplayControllerStatus start() noexcept {
    if (state_ != State::Ready)
      return ReplayControllerStatus::InvalidState;
    if (!valid_synchronization_timeout(synchronization_timeout_us_))
      return ReplayControllerStatus::ConfigurationFailed;
    state_ = State::Starting;
    if (!output_->start(clock_->now())) {
      // A stage may have changed output or acquired resources before its
      // start failed.
      (void)release_output();
      state_ = State::Stopped;
      return ReplayControllerStatus::OutputFault;
    }
    if (!configured_ || !observers_.configured()) {
      (void)release_output();
      state_ = State::Stopped;
      return ReplayControllerStatus::ConfigurationFailed;
    }

    const auto attach_status = engine_.attach();
    if (attach_status != vehicle_signals::SignalStatus::Ok) {
      if (engine_.attached())
        (void)engine_.detach();
      (void)release_output();
      state_ = State::Stopped;
      return ReplayControllerStatus::TelemetryFault;
    }
    if (!observers_.attach()) {
      cleanup_failed_start();
      return ReplayControllerStatus::TelemetryFault;
    }

    const auto telemetry_status = telemetry_.start();
    if (!telemetry_status.ok()) {
      cleanup_failed_start();
      return map_telemetry_status(telemetry_status.status);
    }

    const auto drain =
        mazda::internal::VehicleTelemetryAccess::drain_host_notifications(telemetry_);
    if (!drain.ok()) {
      cleanup_failed_start();
      return ReplayControllerStatus::TelemetryFault;
    }
    if (tick_current() != ReplayControllerStatus::Ok) {
      cleanup_failed_start();
      return ReplayControllerStatus::OutputFault;
    }
    // Only a started replay reaches the observers: the catalog and the
    // start-time readings held since attach() are delivered now.
    observers_.open();
    state_ = State::Running;
    return ReplayControllerStatus::Ok;
  }

  [[nodiscard]] ReplayStepResult process_next_frame() noexcept {
    if (state_ != State::Running)
      return {};
    const auto next_time = source_.next_frame_time();
    if (next_time.has_value() && *next_time > clock_->now())
      return {ReplayControllerStatus::InvalidState, ReplayInputResult::Timeout};
    return process_input(GatedReplaySource::Grant::Receive);
  }

  [[nodiscard]] ReplayStepResult process_timeout() noexcept {
    return process_input(GatedReplaySource::Grant::Timeout);
  }

  [[nodiscard]] ReplayStepResult process_source_fault() noexcept {
    return process_input(GatedReplaySource::Grant::Fault);
  }

  [[nodiscard]] ReplayControllerStatus sample_polled_rules() noexcept {
    if (state_ != State::Running)
      return ReplayControllerStatus::InvalidState;
    if (engine_.sample_polled_rules() == vehicle_signals::SignalStatus::Ok)
      return ReplayControllerStatus::Ok;
    return fail(ReplayControllerStatus::TelemetryFault);
  }

  [[nodiscard]] ReplayControllerStatus sample_signals() noexcept {
    if (state_ != State::Running)
      return ReplayControllerStatus::InvalidState;
    if (observers_.sample())
      return ReplayControllerStatus::Ok;
    return fail(ReplayControllerStatus::TelemetryFault);
  }

  [[nodiscard]] ReplayControllerStatus tick_output() noexcept {
    if (state_ != State::Running)
      return ReplayControllerStatus::InvalidState;
    const auto status = tick_current();
    if (status == ReplayControllerStatus::Ok)
      return status;
    return fail(status);
  }

  [[nodiscard]] ReplayControllerStatus stop() noexcept {
    if (state_ != State::Running && state_ != State::Failed)
      return ReplayControllerStatus::InvalidState;

    publication_barrier_.cancel();
    const auto telemetry_status = telemetry_.stop();
    const bool telemetry_quiescent =
        telemetry_.diagnostics().lifecycle == mazda::LifecycleState::Stopped;
    if (telemetry_quiescent)
      detach_signal_consumers();

    // Every stop fails the output off before releasing the stage, so a
    // completed replay cannot leave its last output active.
    const auto output_status = release_output();
    if (!telemetry_quiescent) {
      state_ = State::Failed;
      return telemetry_status.status == mazda::ResultCode::Timeout
                 ? ReplayControllerStatus::SynchronizationTimeout
                 : ReplayControllerStatus::TelemetryFault;
    }

    state_ = State::Stopped;
    if (telemetry_status.status == mazda::ResultCode::Faulted)
      return ReplayControllerStatus::TelemetryFault;
    if (!telemetry_status.ok())
      return map_telemetry_status(telemetry_status.status);
    return output_status;
  }

  [[nodiscard]] std::optional<vehicle_core::MonotonicTimestamp> next_frame_time() const noexcept {
    return source_.next_frame_time();
  }

  [[nodiscard]] bool running() const noexcept { return state_ == State::Running; }

private:
  enum class State : std::uint8_t { Ready, Starting, Running, Failed, Stopped };

  // Pinned to vehicle-can-core 0.1.0: each acquisition grant reaches exactly
  // one on_diagnostics callback, which completes one publication epoch. The
  // barrier below relies on that one-grant/one-on_diagnostics relationship.
  [[nodiscard]] ReplayStepResult process_input(const GatedReplaySource::Grant grant) noexcept {
    if (state_ != State::Running)
      return {};
    const auto epoch = publication_barrier_.prepare_step();
    if (!epoch.has_value() || !gated_source_.grant(grant))
      return fail_step(ReplayControllerStatus::TelemetryFault);
    if (!publication_barrier_.wait_for(*epoch, synchronization_timeout_us_))
      return fail_step(ReplayControllerStatus::SynchronizationTimeout);

    const auto drain =
        mazda::internal::VehicleTelemetryAccess::drain_host_notifications(telemetry_);
    if (!drain.ok())
      return fail_step(ReplayControllerStatus::TelemetryFault);

    switch (gated_source_.last_receive()) {
    case vehicle_telemetry::ReceiveStatus::Frame:
      return {ReplayControllerStatus::Ok, ReplayInputResult::Frame};
    case vehicle_telemetry::ReceiveStatus::Timeout:
      return {ReplayControllerStatus::Ok,
              grant == GatedReplaySource::Grant::Receive && gated_source_.end_of_stream()
                  ? ReplayInputResult::EndOfStream
                  : ReplayInputResult::Timeout};
    case vehicle_telemetry::ReceiveStatus::Fault:
    case vehicle_telemetry::ReceiveStatus::NotStarted:
      return fail_step(ReplayControllerStatus::TelemetryFault);
    }
    return fail_step(ReplayControllerStatus::TelemetryFault);
  }

  [[nodiscard]] ReplayControllerStatus fail(const ReplayControllerStatus status) noexcept {
    state_ = State::Failed;
    (void)fail_off_output();
    return status;
  }

  [[nodiscard]] ReplayStepResult fail_step(const ReplayControllerStatus status) noexcept {
    return {fail(status), ReplayInputResult::Fault};
  }

  [[nodiscard]] ReplayControllerStatus tick_current() noexcept {
    return output_->tick(clock_->now()) ? ReplayControllerStatus::Ok
                                        : ReplayControllerStatus::OutputFault;
  }

  [[nodiscard]] ReplayControllerStatus fail_off_output() noexcept {
    return output_->fail_off(clock_->now()) ? ReplayControllerStatus::Ok
                                            : ReplayControllerStatus::OutputFault;
  }

  // Fails the output off, then releases the stage.
  [[nodiscard]] ReplayControllerStatus release_output() noexcept {
    const bool output_off = fail_off_output() == ReplayControllerStatus::Ok;
    const bool output_stopped = output_->stop(clock_->now());
    return output_off && output_stopped ? ReplayControllerStatus::Ok
                                        : ReplayControllerStatus::OutputFault;
  }

  // Subscriptions may only change while telemetry is stopped.
  void detach_signal_consumers() noexcept {
    if (engine_.attached())
      (void)engine_.detach();
    if (observers_.attached())
      (void)observers_.detach();
  }

  // A controller left Failed releases the stage through a later stop() or
  // its destructor; one left Stopped releases it here.
  void cleanup_failed_start() noexcept {
    publication_barrier_.cancel();
    if (telemetry_.diagnostics().lifecycle != mazda::LifecycleState::Stopped)
      (void)telemetry_.stop();
    const bool telemetry_quiescent =
        telemetry_.diagnostics().lifecycle == mazda::LifecycleState::Stopped;
    if (telemetry_quiescent)
      detach_signal_consumers();
    state_ = telemetry_quiescent && !engine_.attached() && !observers_.attached() ? State::Stopped
                                                                                  : State::Failed;
    if (state_ == State::Stopped)
      (void)release_output();
    else
      (void)fail_off_output();
  }

  ReplayClock *clock_;
  ReplayAcquisitionSource source_;
  GatedReplaySource gated_source_;
  PublicationBarrier publication_barrier_{};
  mazda::internal::NullLightingSink telemetry_lighting_{};
  mazda::VehicleTelemetry telemetry_{};
  mazda::MazdaSignalProvider signal_provider_;
  action_engine::ActionEngine engine_;
  SignalObserverFanOut observers_;
  OutputStage *output_;
  vehicle_core::Microseconds synchronization_timeout_us_;
  State state_{State::Ready};
  bool configured_{false};
};

ReplayController::ReplayController(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock,
                                   OutputStage &output,
                                   const vehicle_core::Microseconds synchronization_timeout_us)
    : ReplayController(std::move(frames), clock, output, {}, synchronization_timeout_us) {}

ReplayController::ReplayController(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock,
                                   OutputStage &output, SignalObservers observers,
                                   const vehicle_core::Microseconds synchronization_timeout_us)
    : implementation_(std::make_unique<Implementation>(
          std::move(frames), clock, output, std::move(observers), synchronization_timeout_us)) {}

ReplayController::~ReplayController() noexcept = default;

ReplayControllerStatus ReplayController::start() noexcept { return implementation_->start(); }

ReplayStepResult ReplayController::process_next_frame() noexcept {
  return implementation_->process_next_frame();
}

ReplayStepResult ReplayController::process_timeout() noexcept {
  return implementation_->process_timeout();
}

ReplayStepResult ReplayController::process_source_fault() noexcept {
  return implementation_->process_source_fault();
}

ReplayControllerStatus ReplayController::sample_polled_rules() noexcept {
  return implementation_->sample_polled_rules();
}

ReplayControllerStatus ReplayController::sample_signals() noexcept {
  return implementation_->sample_signals();
}

ReplayControllerStatus ReplayController::tick_output() noexcept {
  return implementation_->tick_output();
}

ReplayControllerStatus ReplayController::stop() noexcept { return implementation_->stop(); }

std::optional<vehicle_core::MonotonicTimestamp> ReplayController::next_frame_time() const noexcept {
  return implementation_->next_frame_time();
}

bool ReplayController::running() const noexcept { return implementation_->running(); }

} // namespace replay
