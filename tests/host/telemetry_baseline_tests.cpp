#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "mazda/definitions.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace candidate = mazda::candidate;
using HostClock = std::chrono::steady_clock;

constexpr std::uint32_t kUnrelatedId = 0x7ff;
constexpr std::size_t kExpectedNotificationEvaluations = 18;
constexpr std::size_t kSourceCapacity = mazda::internal::HostAcquisitionSource::kCapacity;
constexpr std::size_t kContinuousFrameCount = 256;
constexpr std::size_t kMalformedFrameCount = 128;
constexpr std::chrono::microseconds kContinuousInterFrame{500};

class AtomicClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return now_us_.load(std::memory_order_relaxed);
  }

  void set(const vehicle_core::MonotonicTimestamp timestamp_us) noexcept {
    now_us_.store(timestamp_us, std::memory_order_relaxed);
  }

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_{0};
};

struct StageStats final {
  std::uint64_t count{0};
  std::uint64_t total_ns{0};
  std::uint64_t maximum_ns{0};

  void add(const std::uint64_t duration_ns) noexcept {
    ++count;
    total_ns += duration_ns;
    if (duration_ns > maximum_ns)
      maximum_ns = duration_ns;
  }
};

struct AtomicStageStats final {
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> total_ns{0};
  std::atomic<std::uint64_t> maximum_ns{0};

  void add(const std::uint64_t duration_ns) noexcept {
    count.fetch_add(1, std::memory_order_relaxed);
    total_ns.fetch_add(duration_ns, std::memory_order_relaxed);
    auto maximum = maximum_ns.load(std::memory_order_relaxed);
    while (maximum < duration_ns &&
           !maximum_ns.compare_exchange_weak(maximum, duration_ns, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
    }
  }

  [[nodiscard]] StageStats snapshot() const noexcept {
    return {count.load(std::memory_order_relaxed), total_ns.load(std::memory_order_relaxed),
            maximum_ns.load(std::memory_order_relaxed)};
  }
};

struct PerIdCount final {
  std::uint32_t identifier{0};
  std::uint64_t total{0};
  std::uint64_t ignored{0};
  std::uint64_t decoded{0};
  std::uint64_t malformed{0};
  std::uint64_t faults{0};
  std::uint64_t decode_ns{0};
  std::uint64_t decode_max_ns{0};
};

struct BaselineReport final {
  static constexpr std::size_t kMaxIds = 24;

  std::string_view scenario{};
  bool profiler_enabled{false};
  std::string_view traffic_rate{};
  std::uint64_t synthetic_duration_us{0};
  std::uint64_t wall_schedule_us{0};
  std::uint64_t observed_accepted_rate_hz{0};
  std::uint64_t wall_ns{0};
  std::uint64_t attempted{0};
  std::uint64_t accepted{0};
  std::uint64_t source_received{0};
  std::uint64_t dequeued{0};
  std::uint64_t dropped{0};
  std::uint64_t queue_overflows{0};
  std::uint64_t queue_depth_max{0};
  bool queue_depth_exact{false};
  std::uint64_t receive_waits{0};
  std::uint64_t receive_timeouts{0};
  std::uint64_t ignored{0};
  std::uint64_t decoded{0};
  std::uint64_t malformed{0};
  std::uint64_t faults{0};
  std::uint64_t frame_processed{0};
  std::uint64_t state_copies{0};
  std::uint64_t notification_evaluations{0};
  std::uint64_t notification_dispatches{0};
  std::uint64_t callbacks{0};
  std::uint64_t led_publishes{0};
  std::uint64_t receive_process_clock_us{0};
  std::uint64_t callback_queue_age_us{0};
  StageStats receive_wait{};
  StageStats enqueue_to_process{};
  StageStats dequeue_to_process{};
  StageStats decode{};
  StageStats diagnostics{};
  StageStats publication{};
  StageStats notification_evaluation{};
  StageStats notification_dispatch{};
  StageStats action{};
  StageStats callback_latency{};
  std::uint64_t latency_callbacks{0};
  std::array<PerIdCount, kMaxIds> per_id{};
  std::size_t per_id_count{0};
};

class Aggregate final {
public:
  static constexpr std::size_t kReceiptSlots = 512;

  Aggregate() noexcept {
    hooks_.context = this;
    hooks_.decode_begin = &decode_begin;
    hooks_.decode_end = &decode_end;
    hooks_.frame_processed = &frame_processed;
    hooks_.diagnostics_begin = &diagnostics_begin;
    hooks_.diagnostics_end = &diagnostics_end;
    hooks_.publication_begin = &publication_begin;
    hooks_.publication_end = &publication_end;
    hooks_.state_copy = &state_copy;
    hooks_.notification_evaluation_begin = &notification_evaluation_begin;
    hooks_.notification_evaluation_end = &notification_evaluation_end;
    hooks_.notification_dispatch_begin = &notification_dispatch_begin;
    hooks_.notification_dispatch_end = &notification_dispatch_end;
  }

  [[nodiscard]] mazda::internal::TelemetryProfilerHooks &hooks() noexcept { return hooks_; }

  void frame_enqueued(const vehicle_core::RawCanFrame &frame) noexcept {
    auto &receipt = receipts_[frame.timestamp_us % kReceiptSlots];
    receipt.enqueue_host_ns.store(host_now_ns(), std::memory_order_relaxed);
    receipt.timestamp_us.store(frame.timestamp_us, std::memory_order_release);
  }

  void frame_received(const vehicle_core::RawCanFrame &frame) noexcept {
    auto &receipt = receipts_[frame.timestamp_us % kReceiptSlots];
    if (receipt.timestamp_us.load(std::memory_order_acquire) == frame.timestamp_us)
      receipt.dequeue_host_ns.store(host_now_ns(), std::memory_order_release);
  }

  void dequeued_frame() noexcept { dequeued_.fetch_add(1, std::memory_order_relaxed); }

  void set_measurement_enabled(const bool enabled) noexcept {
    measurement_enabled_.store(enabled, std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t dequeued_count() const noexcept {
    return dequeued_.load(std::memory_order_relaxed);
  }

  void receive_wait(const HostClock::time_point started,
                    const vehicle_telemetry::ReceiveStatus status) noexcept {
    receive_wait_.add(elapsed_ns(started));
    if (status == vehicle_telemetry::ReceiveStatus::Timeout)
      receive_timeouts_.fetch_add(1, std::memory_order_relaxed);
  }

  void injected(const bool accepted) noexcept {
    attempted_.fetch_add(1, std::memory_order_relaxed);
    if (accepted)
      accepted_.fetch_add(1, std::memory_order_relaxed);
  }

  void queue_depth(const std::uint64_t depth) noexcept {
    auto maximum = queue_depth_max_.load(std::memory_order_relaxed);
    while (maximum < depth &&
           !queue_depth_max_.compare_exchange_weak(maximum, depth, std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
    }
  }

  void action(const HostClock::time_point started) noexcept {
    action_.add(elapsed_ns(started));
    callbacks_.fetch_add(1, std::memory_order_relaxed);
  }

  void led_published() noexcept { led_publishes_.fetch_add(1, std::memory_order_relaxed); }

  void observation_received(const vehicle_core::RawCanFrame &frame,
                            const vehicle_telemetry::ProcessStatus status) noexcept {
    if (status != vehicle_telemetry::ProcessStatus::Processed ||
        frame.identifier != candidate::kTurnSwitchId)
      return;
    last_observation_host_ns_.store(host_now_ns(), std::memory_order_relaxed);
    last_observation_clock_us_.store(frame.timestamp_us, std::memory_order_relaxed);
  }

  void callback_entered() noexcept {
    latency_callbacks_.fetch_add(1, std::memory_order_relaxed);
    const auto observed_host_ns = last_observation_host_ns_.load(std::memory_order_relaxed);
    if (observed_host_ns != 0) {
      const auto now_ns = host_now_ns();
      if (now_ns >= observed_host_ns)
        callback_latency_.add(now_ns - observed_host_ns);
    }
    const auto observed_clock_us = last_observation_clock_us_.load(std::memory_order_relaxed);
    const auto now_us = injected_clock_us_.load(std::memory_order_relaxed);
    if (now_us >= observed_clock_us)
      callback_queue_age_us_.fetch_add(now_us - observed_clock_us, std::memory_order_relaxed);
  }

  void set_wall_time(const std::uint64_t duration_ns) noexcept { wall_ns_ = duration_ns; }

  [[nodiscard]] BaselineReport
  report(const std::string_view scenario, const bool profiler_enabled,
         const vehicle_telemetry::AcquisitionStatistics &acquisition) const noexcept {
    BaselineReport result{};
    result.scenario = scenario;
    result.profiler_enabled = profiler_enabled;
    result.wall_ns = wall_ns_;
    result.attempted = attempted_.load(std::memory_order_relaxed);
    result.accepted = accepted_.load(std::memory_order_relaxed);
    result.observed_accepted_rate_hz =
        wall_ns_ == 0 ? 0 : (result.accepted * 1'000'000'000ULL) / wall_ns_;
    result.source_received = acquisition.frames_received;
    result.dequeued = dequeued_.load(std::memory_order_relaxed);
    result.dropped = acquisition.frames_dropped;
    result.queue_overflows = acquisition.queue_overflows;
    result.queue_depth_max = queue_depth_max_.load(std::memory_order_relaxed);
    result.receive_waits = receive_wait_.count.load(std::memory_order_relaxed);
    result.receive_timeouts = receive_timeouts_.load(std::memory_order_relaxed);
    result.ignored = ignored_.load(std::memory_order_relaxed);
    result.decoded = decoded_.load(std::memory_order_relaxed);
    result.malformed = malformed_.load(std::memory_order_relaxed);
    result.faults = faults_.load(std::memory_order_relaxed);
    result.frame_processed = frame_processed_.load(std::memory_order_relaxed);
    result.state_copies = state_copies_.load(std::memory_order_relaxed);
    result.notification_evaluations = notification_evaluations_.load(std::memory_order_relaxed);
    result.notification_dispatches = notification_dispatches_.load(std::memory_order_relaxed);
    result.callbacks = callbacks_.load(std::memory_order_relaxed);
    result.led_publishes = led_publishes_.load(std::memory_order_relaxed);
    result.receive_process_clock_us = receive_process_clock_us_.load(std::memory_order_relaxed);
    result.callback_queue_age_us = callback_queue_age_us_.load(std::memory_order_relaxed);
    result.receive_wait = receive_wait_.snapshot();
    result.enqueue_to_process = enqueue_to_process_.snapshot();
    result.dequeue_to_process = dequeue_to_process_.snapshot();
    result.decode = decode_.snapshot();
    result.diagnostics = diagnostics_.snapshot();
    result.publication = publication_.snapshot();
    result.notification_evaluation = notification_evaluation_.snapshot();
    result.notification_dispatch = notification_dispatch_.snapshot();
    result.action = action_.snapshot();
    result.callback_latency = callback_latency_.snapshot();
    result.latency_callbacks = latency_callbacks_.load(std::memory_order_relaxed);
    result.per_id_count = id_count_;
    for (std::size_t index = 0; index < id_count_; ++index)
      result.per_id[index] = ids_[index];
    return result;
  }

private:
  struct Receipt final {
    std::atomic<std::uint64_t> timestamp_us{0};
    std::atomic<std::uint64_t> enqueue_host_ns{0};
    std::atomic<std::uint64_t> dequeue_host_ns{0};
  };

  static std::uint64_t elapsed_ns(const HostClock::time_point started) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now() - started).count());
  }

  static std::uint64_t host_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now().time_since_epoch())
            .count());
  }

  PerIdCount *find_id(const std::uint32_t identifier) noexcept {
    for (std::size_t index = 0; index < id_count_; ++index) {
      if (ids_[index].identifier == identifier)
        return &ids_[index];
    }
    if (id_count_ == ids_.size())
      return nullptr;
    ids_[id_count_] = {identifier, 0, 0, 0, 0, 0, 0, 0};
    return &ids_[id_count_++];
  }

  void decode_end(const vehicle_core::RawCanFrame &frame,
                  const vehicle_telemetry::ProcessStatus status) noexcept {
    const bool measure = measurement_enabled_.load(std::memory_order_relaxed);
    const auto duration_ns = measure ? elapsed_ns(decode_started_) : 0;
    if (measure)
      decode_.add(duration_ns);
    auto *id = find_id(frame.identifier);
    if (id != nullptr) {
      ++id->total;
      id->decode_ns += duration_ns;
      if (duration_ns > id->decode_max_ns)
        id->decode_max_ns = duration_ns;
    }
    switch (status) {
    case vehicle_telemetry::ProcessStatus::Processed:
      decoded_.fetch_add(1, std::memory_order_relaxed);
      if (id != nullptr)
        ++id->decoded;
      break;
    case vehicle_telemetry::ProcessStatus::Ignored:
      ignored_.fetch_add(1, std::memory_order_relaxed);
      if (id != nullptr)
        ++id->ignored;
      break;
    case vehicle_telemetry::ProcessStatus::Malformed:
      malformed_.fetch_add(1, std::memory_order_relaxed);
      if (id != nullptr)
        ++id->malformed;
      break;
    case vehicle_telemetry::ProcessStatus::Fault:
      faults_.fetch_add(1, std::memory_order_relaxed);
      if (id != nullptr)
        ++id->faults;
      break;
    }
  }

  void on_frame_processed(const vehicle_core::RawCanFrame &frame,
                          const vehicle_telemetry::ProcessStatus status) noexcept {
    frame_processed_.fetch_add(1, std::memory_order_relaxed);
    const auto &receipt = receipts_[frame.timestamp_us % kReceiptSlots];
    if (receipt.timestamp_us.load(std::memory_order_acquire) == frame.timestamp_us) {
      const auto now_ns = host_now_ns();
      const auto enqueue_ns = receipt.enqueue_host_ns.load(std::memory_order_relaxed);
      const auto dequeue_ns = receipt.dequeue_host_ns.load(std::memory_order_acquire);
      if (now_ns >= enqueue_ns)
        enqueue_to_process_.add(now_ns - enqueue_ns);
      if (now_ns >= dequeue_ns)
        dequeue_to_process_.add(now_ns - dequeue_ns);
    }
    // Both values are in the injected MonotonicTimestamp domain. This is a
    // queue-age diagnostic, not a correction between host and ESP clocks.
    const auto now_us = injected_clock_us_.load(std::memory_order_relaxed);
    if (now_us >= frame.timestamp_us)
      receive_process_clock_us_.fetch_add(now_us - frame.timestamp_us, std::memory_order_relaxed);
    observation_received(frame, status);
  }

public:
  void diagnostics_completed() noexcept {
    {
      const std::lock_guard<std::mutex> lock{published_mutex_};
      published_frames_.store(frame_processed_.load(std::memory_order_acquire),
                              std::memory_order_release);
    }
    published_condition_.notify_all();
  }

  [[nodiscard]] std::uint64_t frame_processed_count() const noexcept {
    return frame_processed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool wait_for_published_frames(const std::uint64_t target) noexcept {
    std::unique_lock<std::mutex> lock{published_mutex_};
    return published_condition_.wait_for(lock, std::chrono::seconds{2}, [this, target] {
      return published_frames_.load(std::memory_order_acquire) >= target;
    });
  }

  static void decode_begin(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->decode_started_ = HostClock::now();
  }
  static void decode_end(void *context, const vehicle_core::RawCanFrame &frame,
                         const vehicle_telemetry::ProcessStatus status) noexcept {
    static_cast<Aggregate *>(context)->decode_end(frame, status);
  }
  static void frame_processed(void *context, const vehicle_core::RawCanFrame &frame,
                              const vehicle_telemetry::ProcessStatus status) noexcept {
    static_cast<Aggregate *>(context)->on_frame_processed(frame, status);
  }
  static void diagnostics_begin(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->diagnostics_started_ = HostClock::now();
  }
  static void diagnostics_end(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->diagnostics_.add(elapsed_ns(aggregate->diagnostics_started_));
    aggregate->diagnostics_completed();
  }
  static void publication_begin(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->publication_started_ = HostClock::now();
  }
  static void publication_end(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->publication_.add(elapsed_ns(aggregate->publication_started_));
  }
  static void state_copy(void *context) noexcept {
    static_cast<Aggregate *>(context)->state_copies_.fetch_add(1, std::memory_order_relaxed);
  }
  static void notification_evaluation_begin(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->notification_evaluation_started_ = HostClock::now();
  }
  static void notification_evaluation_end(void *context, const std::size_t evaluations) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    aggregate->notification_evaluations_.fetch_add(evaluations, std::memory_order_relaxed);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->notification_evaluation_.add(
          elapsed_ns(aggregate->notification_evaluation_started_));
  }
  static void notification_dispatch_begin(void *context) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->notification_dispatch_started_ = HostClock::now();
  }
  static void notification_dispatch_end(void *context, const std::size_t) noexcept {
    auto *aggregate = static_cast<Aggregate *>(context);
    aggregate->notification_dispatches_.fetch_add(1, std::memory_order_relaxed);
    if (aggregate->measurement_enabled_.load(std::memory_order_relaxed))
      aggregate->notification_dispatch_.add(elapsed_ns(aggregate->notification_dispatch_started_));
  }

  mazda::internal::TelemetryProfilerHooks hooks_{};
  std::array<Receipt, kReceiptSlots> receipts_{};
  std::array<PerIdCount, BaselineReport::kMaxIds> ids_{};
  std::size_t id_count_{0};
  HostClock::time_point decode_started_{};
  HostClock::time_point diagnostics_started_{};
  HostClock::time_point publication_started_{};
  HostClock::time_point notification_evaluation_started_{};
  HostClock::time_point notification_dispatch_started_{};
  std::uint64_t wall_ns_{0};
  AtomicStageStats receive_wait_{};
  AtomicStageStats enqueue_to_process_{};
  AtomicStageStats dequeue_to_process_{};
  AtomicStageStats decode_{};
  AtomicStageStats diagnostics_{};
  AtomicStageStats publication_{};
  AtomicStageStats notification_evaluation_{};
  AtomicStageStats notification_dispatch_{};
  AtomicStageStats action_{};
  AtomicStageStats callback_latency_{};
  std::atomic<std::uint64_t> attempted_{0};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> queue_depth_max_{0};
  std::atomic<std::uint64_t> receive_timeouts_{0};
  std::atomic<std::uint64_t> ignored_{0};
  std::atomic<std::uint64_t> decoded_{0};
  std::atomic<std::uint64_t> malformed_{0};
  std::atomic<std::uint64_t> faults_{0};
  std::atomic<std::uint64_t> frame_processed_{0};
  std::atomic<std::uint64_t> state_copies_{0};
  std::atomic<std::uint64_t> notification_evaluations_{0};
  std::atomic<std::uint64_t> notification_dispatches_{0};
  std::atomic<std::uint64_t> callbacks_{0};
  std::atomic<std::uint64_t> led_publishes_{0};
  std::atomic<std::uint64_t> dequeued_{0};
  std::atomic<bool> measurement_enabled_{false};
  std::atomic<std::uint64_t> injected_clock_us_{0};
  std::atomic<std::uint64_t> receive_process_clock_us_{0};
  std::atomic<std::uint64_t> latency_callbacks_{0};
  std::atomic<std::uint64_t> last_observation_host_ns_{0};
  std::atomic<std::uint64_t> last_observation_clock_us_{0};
  std::atomic<std::uint64_t> callback_queue_age_us_{0};
  std::atomic<std::uint64_t> published_frames_{0};
  mutable std::mutex published_mutex_{};
  std::condition_variable published_condition_{};

public:
  void set_injected_clock(const vehicle_core::MonotonicTimestamp now_us) noexcept {
    injected_clock_us_.store(now_us, std::memory_order_relaxed);
  }
};

void latency_observer(void *context, const mazda::Notification<mazda::TurnState> &) noexcept {
  static_cast<Aggregate *>(context)->callback_entered();
}

class TimedSource final : public vehicle_telemetry::AcquisitionSource {
public:
  TimedSource(Aggregate &aggregate, AtomicClock &clock, const bool ready_mode) noexcept
      : aggregate_(&aggregate), clock_(&clock), ready_mode_(ready_mode) {}

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    pending_.store(0, std::memory_order_relaxed);
    if (ready_mode_) {
      const std::lock_guard<std::mutex> lock{ready_mutex_};
      ready_running_ = true;
      return {vehicle_telemetry::ResultCode::Ok};
    }
    return source_.start();
  }
  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    if (ready_mode_) {
      {
        const std::lock_guard<std::mutex> lock{ready_mutex_};
        ready_running_ = false;
      }
      ready_condition_.notify_all();
      return {vehicle_telemetry::ResultCode::Ok};
    }
    return source_.stop();
  }
  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, const std::uint32_t timeout_ms) noexcept override {
    const auto started = HostClock::now();
    if (ready_mode_) {
      std::unique_lock<std::mutex> lock{ready_mutex_};
      if (ready_index_ == ready_count_ && ready_running_)
        ready_condition_.wait_for(lock, std::chrono::milliseconds{timeout_ms}, [this] {
          return ready_index_ < ready_count_ || !ready_running_;
        });
      if (ready_index_ < ready_count_) {
        frame = ready_frames_[ready_index_++];
        lock.unlock();
        aggregate_->receive_wait(started, vehicle_telemetry::ReceiveStatus::Frame);
        aggregate_->frame_received(frame);
        aggregate_->dequeued_frame();
        return vehicle_telemetry::ReceiveStatus::Frame;
      }
      lock.unlock();
      aggregate_->receive_wait(started, vehicle_telemetry::ReceiveStatus::Timeout);
      return vehicle_telemetry::ReceiveStatus::Timeout;
    }
    const auto status = source_.receive(frame, timeout_ms);
    aggregate_->receive_wait(started, status);
    if (status == vehicle_telemetry::ReceiveStatus::Frame) {
      pending_.fetch_sub(1, std::memory_order_relaxed);
      aggregate_->frame_received(frame);
      aggregate_->dequeued_frame();
    }
    return status;
  }
  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    if (ready_mode_) {
      const std::lock_guard<std::mutex> lock{ready_mutex_};
      return ready_statistics_;
    }
    return source_.statistics();
  }

  [[nodiscard]] bool load_ready(const std::vector<vehicle_core::RawCanFrame> &frames) noexcept {
    if (frames.size() > ready_frames_.size())
      return false;
    for (const auto &ready_frame : frames) {
      clock_->set(ready_frame.timestamp_us);
      aggregate_->set_injected_clock(ready_frame.timestamp_us);
      aggregate_->frame_enqueued(ready_frame);
      aggregate_->injected(true);
    }
    {
      const std::lock_guard<std::mutex> lock{ready_mutex_};
      for (std::size_t index = 0; index < frames.size(); ++index)
        ready_frames_[index] = frames[index];
      aggregate_->queue_depth(frames.size());
      ready_count_ = frames.size();
      ready_index_ = 0;
      ready_statistics_ = {};
      ready_statistics_.frames_received = frames.size();
    }
    ready_condition_.notify_all();
    return true;
  }

  [[nodiscard]] bool inject(const vehicle_core::RawCanFrame &frame) noexcept {
    clock_->set(frame.timestamp_us);
    aggregate_->set_injected_clock(frame.timestamp_us);
    aggregate_->frame_enqueued(frame);
    const auto pending = pending_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto status = source_.inject(frame);
    const bool accepted = status == mazda::ResultCode::Ok;
    if (!accepted)
      pending_.fetch_sub(1, std::memory_order_relaxed);
    else
      aggregate_->queue_depth(pending);
    aggregate_->injected(accepted);
    return accepted;
  }

private:
  static constexpr std::size_t kReadyFrameCapacity = 512;
  mazda::internal::HostAcquisitionSource source_{};
  Aggregate *aggregate_{nullptr};
  AtomicClock *clock_{nullptr};
  const bool ready_mode_{false};
  std::atomic<std::uint64_t> pending_{0};
  mutable std::mutex ready_mutex_{};
  std::condition_variable ready_condition_{};
  std::array<vehicle_core::RawCanFrame, kReadyFrameCapacity> ready_frames_{};
  std::size_t ready_count_{0};
  std::size_t ready_index_{0};
  bool ready_running_{false};
  vehicle_telemetry::AcquisitionStatistics ready_statistics_{};
};

class PublicationGate final : public mazda::internal::HostPublicationControl {
public:
  void publication_completed() noexcept override {
    std::unique_lock<std::mutex> lock{mutex_};
    block_entered_ = block_next_ && (target_aggregate_ == nullptr ||
                                     target_aggregate_->frame_processed_count() >= target_frame_);
    condition_.notify_all();
    if (block_entered_)
      condition_.wait(lock, [this] { return !block_next_; });
  }

  void arm_block(Aggregate &aggregate, const std::uint64_t target_frame) noexcept {
    const std::lock_guard<std::mutex> lock{mutex_};
    block_next_ = true;
    target_aggregate_ = &aggregate;
    target_frame_ = target_frame;
  }

  void release() noexcept {
    {
      const std::lock_guard<std::mutex> lock{mutex_};
      block_next_ = false;
      block_entered_ = false;
      target_aggregate_ = nullptr;
      target_frame_ = 0;
    }
    condition_.notify_all();
  }

  [[nodiscard]] bool wait_until_blocked() noexcept {
    std::unique_lock<std::mutex> lock{mutex_};
    return condition_.wait_for(lock, std::chrono::seconds{2}, [this] { return block_entered_; });
  }

private:
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  bool block_next_{false};
  bool block_entered_{false};
  Aggregate *target_aggregate_{nullptr};
  std::uint64_t target_frame_{0};
};

class FakeLightingSink final : public local_argb::internal::LightingSink {
public:
  explicit FakeLightingSink(Aggregate &aggregate) noexcept : aggregate_(&aggregate) {}

  [[nodiscard]] bool publish(const local_argb::internal::LightingCommand &) noexcept override {
    aggregate_->led_published();
    return true;
  }

private:
  Aggregate *aggregate_{nullptr};
};

class TimedActionSink final : public action_engine::ActionSink {
public:
  TimedActionSink(local_argb_actions::LedActionSink &sink, Aggregate &aggregate) noexcept
      : sink_(&sink), aggregate_(&aggregate) {}

  void execute(const action_engine::ActionCommand &command) noexcept override {
    const auto started = HostClock::now();
    sink_->execute(command);
    aggregate_->action(started);
  }

private:
  local_argb_actions::LedActionSink *sink_{nullptr};
  Aggregate *aggregate_{nullptr};
};

vehicle_core::RawCanFrame frame(const std::uint32_t identifier,
                                const vehicle_core::MonotonicTimestamp timestamp_us,
                                std::initializer_list<std::uint8_t> bytes) {
  vehicle_core::RawCanFrame result{};
  result.identifier = identifier;
  result.timestamp_us = timestamp_us;
  result.dlc = static_cast<std::uint8_t>(bytes.size());
  std::size_t index = 0;
  for (const auto byte : bytes)
    result.data[index++] = byte;
  return result;
}

constexpr std::initializer_list<std::uint8_t> kTurnLeft{0, 0x20, 0, 0, 0, 0, 0, 0};
constexpr std::initializer_list<std::uint8_t> kTurnRight{0, 0x08, 0, 0, 0, 0, 0, 0};
constexpr std::initializer_list<std::uint8_t> kEngine{0x09, 0x5b, 0, 0, 0, 0, 0, 0};
constexpr std::initializer_list<std::uint8_t> kBrake{0x10, 0, 0, 0, 0, 0, 0, 0};

class Harness final {
public:
  Harness(Aggregate &aggregate, const bool profiler_enabled, const bool ready_mode = false)
      : source_(aggregate, clock_, ready_mode), gate_{}, lighting_(aggregate),
        led_sink_(lighting_, clock_), action_sink_(led_sink_, aggregate), provider_(telemetry_),
        engine_(provider_) {
    aggregate.set_measurement_enabled(profiler_enabled);
    mazda::TelemetryConfig config{};
    config.availability_service_target_us = 1'000;
    config.callback_stop_timeout_us = 500'000;
    mazda::internal::HostServiceOptions options{};
    options.publication_control = &gate_;
    options.manual_notification_dispatch = true;
    // Hooks remain attached in both modes so accounting stays equivalent.
    // The aggregate suppresses host timing reads when profiler_enabled is false.
    options.profiler = &aggregate.hooks();
    mazda::internal::VehicleTelemetryAccess::emplace_host_service(
        telemetry_, clock_, source_, legacy_lighting_, config, options);

    REQUIRE(led_sink_.bind(action_engine::ActionId{1}, local_argb_actions::LedEffect::LeftTurn) ==
            local_argb_actions::BindingStatus::Ok);
    const auto latency_subscription =
        telemetry_.on_turn_state_changed(&latency_observer, &aggregate);
    REQUIRE(latency_subscription.ok());
    latency_subscription_ = *latency_subscription.value;
    latency_subscribed_ = true;
    REQUIRE(engine_.add_sink(action_sink_) == action_engine::ConfigStatus::Ok);
    REQUIRE(engine_.add_state_rule(action_engine::StateRuleConfig{
                {"vehicle.turn_state", action_engine::Comparison::Equal,
                 action_engine::RuleOperand::choice("left")},
                action_engine::ActionId{1}}) == action_engine::ConfigStatus::Ok);
    attached_ = engine_.attach() == vehicle_signals::SignalStatus::Ok;
    REQUIRE(attached_);
    REQUIRE(telemetry_.start().ok());
    started_ = true;
  }

  ~Harness() {
    gate_.release();
    if (started_)
      REQUIRE(telemetry_.stop().ok());
    if (latency_subscribed_)
      REQUIRE(telemetry_.unsubscribe(latency_subscription_).ok());
    if (attached_)
      REQUIRE(engine_.detach() == vehicle_signals::SignalStatus::Ok);
  }

  void stop_and_detach() noexcept {
    gate_.release();
    if (started_) {
      REQUIRE(telemetry_.stop().ok());
      started_ = false;
    }
    if (attached_) {
      REQUIRE(engine_.detach() == vehicle_signals::SignalStatus::Ok);
      attached_ = false;
    }
    if (latency_subscribed_) {
      REQUIRE(telemetry_.unsubscribe(latency_subscription_).ok());
      latency_subscribed_ = false;
    }
  }

  [[nodiscard]] TimedSource &source() noexcept { return source_; }
  [[nodiscard]] PublicationGate &gate() noexcept { return gate_; }
  [[nodiscard]] mazda::VehicleTelemetry &telemetry() noexcept { return telemetry_; }

private:
  AtomicClock clock_{};
  TimedSource source_;
  PublicationGate gate_{};
  FakeLightingSink lighting_;
  mazda::internal::NullLightingSink legacy_lighting_{};
  local_argb_actions::LedActionSink led_sink_;
  TimedActionSink action_sink_;
  mazda::VehicleTelemetry telemetry_{};
  mazda::MazdaSignalProvider provider_;
  action_engine::ActionEngine engine_;
  mazda::Subscription latency_subscription_{};
  bool started_{false};
  bool attached_{false};
  bool latency_subscribed_{false};
};

enum class Scenario : std::uint8_t { Silence, Unrelated, Supported, Malformed, Burst };

std::string_view scenario_name(const Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::Silence:
    return "silence";
  case Scenario::Unrelated:
    return "continuously-ready-unrelated";
  case Scenario::Supported:
    return "supported-mix";
  case Scenario::Malformed:
    return "malformed";
  case Scenario::Burst:
    return "finite-fifo-burst";
  }
  return "unknown";
}

std::string_view traffic_rate(const Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::Silence:
    return "0";
  case Scenario::Burst:
    return "immediate";
  case Scenario::Unrelated:
    return "ready-backlog";
  case Scenario::Supported:
  case Scenario::Malformed:
    return "paced-target-2000fps";
  }
  return "unknown";
}

std::uint64_t synthetic_duration_us(const Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::Silence:
    return 8'000;
  case Scenario::Unrelated:
  case Scenario::Supported:
    return kContinuousFrameCount - 1;
  case Scenario::Malformed:
    return kMalformedFrameCount - 1;
  case Scenario::Burst:
    return 0;
  }
  return 0;
}

std::uint64_t wall_schedule_us(const Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::Silence:
    return 8'000;
  case Scenario::Supported:
  case Scenario::Malformed:
    return static_cast<std::uint64_t>(
               (scenario == Scenario::Malformed ? kMalformedFrameCount : kContinuousFrameCount) -
               1) *
           static_cast<std::uint64_t>(kContinuousInterFrame.count());
  case Scenario::Unrelated:
  case Scenario::Burst:
    return 0;
  }
  return 0;
}

void wait_for_published_frames(Aggregate &aggregate, const std::uint64_t target) {
  REQUIRE(aggregate.wait_for_published_frames(target));
}

BaselineReport run_scenario(const Scenario scenario, const bool profiler_enabled) {
  Aggregate aggregate{};
  Harness harness{aggregate, profiler_enabled, scenario == Scenario::Unrelated};
  const auto started = HostClock::now();
  std::uint64_t timestamp = 100;
  const auto frame_baseline = aggregate.frame_processed_count();

  switch (scenario) {
  case Scenario::Silence:
    std::this_thread::sleep_for(std::chrono::milliseconds{8});
    break;
  case Scenario::Unrelated: {
    std::vector<vehicle_core::RawCanFrame> ready_frames;
    ready_frames.reserve(kContinuousFrameCount);
    for (std::size_t index = 0; index < kContinuousFrameCount; ++index) {
      ready_frames.push_back(frame(kUnrelatedId, timestamp++, {0, 0, 0, 0, 0, 0, 0, 0}));
    }
    REQUIRE(harness.source().load_ready(ready_frames));
    const auto stats = harness.source().statistics();
    wait_for_published_frames(aggregate,
                              frame_baseline + stats.frames_received - stats.frames_dropped);
    REQUIRE(mazda::internal::VehicleTelemetryAccess::drain_host_notifications(harness.telemetry())
                .ok());
  } break;
  case Scenario::Supported: {
    for (std::size_t index = 0; index < kContinuousFrameCount; ++index) {
      const auto remainder = index % 3;
      if (remainder == 0)
        CHECK(harness.source().inject(frame(candidate::kTurnSwitchId, timestamp++, kTurnLeft)));
      else if (remainder == 1)
        CHECK(harness.source().inject(frame(candidate::kTurnSwitchId, timestamp++, kTurnRight)));
      else
        CHECK(harness.source().inject(frame(candidate::kEngineDataId, timestamp++, kEngine)));
      REQUIRE(aggregate.wait_for_published_frames(frame_baseline + index + 1));
      REQUIRE(mazda::internal::VehicleTelemetryAccess::drain_host_notifications(harness.telemetry())
                  .ok());
      std::this_thread::sleep_for(kContinuousInterFrame);
    }
    const auto stats = harness.source().statistics();
    wait_for_published_frames(aggregate,
                              frame_baseline + stats.frames_received - stats.frames_dropped);
    REQUIRE(mazda::internal::VehicleTelemetryAccess::drain_host_notifications(harness.telemetry())
                .ok());
  } break;
  case Scenario::Malformed: {
    for (std::size_t index = 0; index < kMalformedFrameCount; ++index) {
      CHECK(harness.source().inject(frame(candidate::kTurnSwitchId, timestamp++, {0})));
      std::this_thread::sleep_for(kContinuousInterFrame);
    }
    const auto stats = harness.source().statistics();
    wait_for_published_frames(aggregate,
                              frame_baseline + stats.frames_received - stats.frames_dropped);
    REQUIRE(mazda::internal::VehicleTelemetryAccess::drain_host_notifications(harness.telemetry())
                .ok());
  } break;
  case Scenario::Burst: {
    // Hold the first publication after receive. The source queue is then
    // filled while the Runtime worker is parked, making drop-newest counts
    // deterministic without bypassing Runtime or Mazda decoding.
    harness.gate().arm_block(aggregate, frame_baseline + 1);
    // Let several one-millisecond receive periods elapse before the first
    // frame. Timeout diagnostics must not consume the frame-specific gate.
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    CHECK(harness.source().inject(frame(candidate::kBrakePedalId, timestamp++, kBrake)));
    REQUIRE(aggregate.wait_for_published_frames(frame_baseline + 1));
    REQUIRE(harness.gate().wait_until_blocked());
    for (std::size_t index = 1; index < 512; ++index)
      (void)harness.source().inject(frame(candidate::kBrakePedalId, timestamp++, kBrake));
    const auto stats = harness.source().statistics();
    const auto accepted = stats.frames_received - stats.frames_dropped;
    harness.gate().release();
    wait_for_published_frames(aggregate, frame_baseline + accepted);
    REQUIRE(mazda::internal::VehicleTelemetryAccess::drain_host_notifications(harness.telemetry())
                .ok());
    break;
  }
  }

  const auto stats_before_stop = harness.source().statistics();
  aggregate.set_wall_time(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now() - started).count()));
  harness.stop_and_detach();
  auto report = aggregate.report(scenario_name(scenario), profiler_enabled, stats_before_stop);
  report.traffic_rate = traffic_rate(scenario);
  report.synthetic_duration_us = synthetic_duration_us(scenario);
  report.wall_schedule_us = wall_schedule_us(scenario);
  report.queue_depth_exact = scenario == Scenario::Unrelated || scenario == Scenario::Burst;
  return report;
}

void print_report(const BaselineReport &report, const std::size_t repeat) {
  std::cout << "telemetry_baseline scenario=" << report.scenario << " repeat=" << repeat
            << " profiler=" << (report.profiler_enabled ? "stage-timers" : "counters-only")
            << " build_revision=" << MAZDA_BASELINE_BUILD_REVISION
            << " build_type=" << MAZDA_BASELINE_BUILD_TYPE
            << " source_dirty=" << MAZDA_BASELINE_SOURCE_DIRTY << " cxx_flags='"
            << MAZDA_BASELINE_CXX_FLAGS << "'"
            << " target_flags='" << MAZDA_BASELINE_TARGET_FLAGS << "'"
            << " compiler=" << __VERSION__ << " cxx_std=17 ble=off"
            << " subscriptions=turn_state->ActionEngine->LedActionSink+latency_observer"
            << " wall_ns=" << report.wall_ns << "\n"
            << "  traffic attempted=" << report.attempted << " accepted=" << report.accepted
            << " source_received=" << report.source_received << " dequeued=" << report.dequeued
            << " dropped=" << report.dropped << " queue_overflows=" << report.queue_overflows
            << " queue_depth_max=" << report.queue_depth_max
            << " queue_depth_mode=" << (report.queue_depth_exact ? "exact" : "shadow-pending")
            << " receive_waits=" << report.receive_waits
            << " receive_timeouts=" << report.receive_timeouts << "\n"
            << "  traffic_rate=" << report.traffic_rate
            << " synthetic_duration_us=" << report.synthetic_duration_us
            << " wall_schedule_us=" << report.wall_schedule_us
            << " observed_accepted_rate_hz=" << report.observed_accepted_rate_hz << "\n"
            << "  outcomes ignored=" << report.ignored << " decoded=" << report.decoded
            << " malformed=" << report.malformed << " faults=" << report.faults
            << " frame_processed=" << report.frame_processed
            << " state_copies=" << report.state_copies
            << " notification_evaluations=" << report.notification_evaluations
            << " dispatches=" << report.notification_dispatches
            << " action_callbacks=" << report.callbacks
            << " latency_callbacks=" << report.latency_callbacks
            << " led_publishes=" << report.led_publishes
            << " injected_clock_queue_age_us=" << report.receive_process_clock_us
            << " callback_queue_age_us=" << report.callback_queue_age_us << "\n"
            << "  stage_ns receive_wait=" << report.receive_wait.total_ns
            << " enqueue_to_process=" << report.enqueue_to_process.total_ns
            << " dequeue_to_process=" << report.dequeue_to_process.total_ns
            << " decode=" << report.decode.total_ns
            << " diagnostics=" << report.diagnostics.total_ns
            << " publication=" << report.publication.total_ns
            << " notification_evaluation=" << report.notification_evaluation.total_ns
            << " notification_dispatch=" << report.notification_dispatch.total_ns
            << " action_sink=" << report.action.total_ns
            << " callback_delivery=" << report.callback_latency.total_ns << "\n";
  std::cout << "  ids";
  for (std::size_t index = 0; index < report.per_id_count; ++index) {
    const auto &id = report.per_id[index];
    std::cout << " 0x" << std::hex << id.identifier << std::dec << "=total:" << id.total
              << ",ignored:" << id.ignored << ",decoded:" << id.decoded
              << ",malformed:" << id.malformed << ",faults:" << id.faults
              << ",decode_ns:" << id.decode_ns << ",decode_max_ns:" << id.decode_max_ns;
  }
  std::cout << "\n";
}

} // namespace

TEST_CASE("synthetic telemetry baseline records production path stages") {
  const auto off = run_scenario(Scenario::Supported, false);
  const auto on = run_scenario(Scenario::Supported, true);
  CHECK(off.attempted == kContinuousFrameCount);
  CHECK(off.accepted == kContinuousFrameCount);
  CHECK(off.source_received == kContinuousFrameCount);
  CHECK(off.dequeued == kContinuousFrameCount);
  CHECK(off.decoded == kContinuousFrameCount);
  CHECK(off.decode.count == 0);
  CHECK(on.decode.count == on.decoded);
  CHECK(on.enqueue_to_process.count == on.frame_processed);
  CHECK(on.dequeue_to_process.count == on.frame_processed);
  CHECK(on.state_copies == on.publication.count * 2);
  CHECK(on.notification_evaluations ==
        on.notification_evaluation.count * kExpectedNotificationEvaluations);
  CHECK(on.callbacks > 0);
  CHECK(on.action.count == on.callbacks);
  CHECK(on.latency_callbacks > 0);
  CHECK(on.callback_latency.count <= on.latency_callbacks);
  print_report(off, 0);
  print_report(on, 0);
}

TEST_CASE("synthetic baseline covers deterministic traffic mix and repeats") {
  constexpr std::array scenarios{Scenario::Silence, Scenario::Unrelated, Scenario::Supported,
                                 Scenario::Malformed, Scenario::Burst};
  for (const auto scenario : scenarios) {
    const auto first_off = run_scenario(scenario, false);
    const auto first = run_scenario(scenario, true);
    const auto second_off = run_scenario(scenario, false);
    const auto second = run_scenario(scenario, true);
    CHECK(first.scenario == second.scenario);
    CHECK(first_off.attempted == first.attempted);
    CHECK(first_off.source_received == first.source_received);
    CHECK(first_off.dequeued == first.dequeued);
    CHECK(first_off.dropped == first.dropped);
    CHECK(first_off.ignored == first.ignored);
    CHECK(first_off.decoded == first.decoded);
    CHECK(first_off.malformed == first.malformed);
    CHECK(first.attempted == second.attempted);
    CHECK(first.accepted == second.accepted);
    CHECK(first.dropped == second.dropped);
    CHECK(first.ignored == second.ignored);
    CHECK(first.decoded == second.decoded);
    CHECK(first.malformed == second.malformed);
    CHECK(first.source_received == first.attempted);
    CHECK(first.dequeued == first.accepted);
    CHECK(first.accepted + first.dropped == first.attempted);
    CHECK(first.queue_overflows == first.dropped);
    CHECK(second_off.attempted == second.attempted);
    CHECK(second_off.source_received == second.source_received);
    CHECK(second_off.dequeued == second.dequeued);
    CHECK(second_off.dropped == second.dropped);
    CHECK(second_off.ignored == second.ignored);
    CHECK(second_off.decoded == second.decoded);
    CHECK(second_off.malformed == second.malformed);
    if (scenario == Scenario::Silence)
      CHECK(first.attempted == 0);
    if (scenario == Scenario::Unrelated)
      CHECK(first.ignored == first.dequeued);
    if (scenario == Scenario::Unrelated) {
      CHECK(first.queue_depth_max == kContinuousFrameCount);
      CHECK(first.queue_depth_exact);
    }
    if (scenario == Scenario::Supported)
      CHECK(first.decoded == first.dequeued);
    if (scenario == Scenario::Malformed)
      CHECK(first.malformed == first.dequeued);
    if (scenario == Scenario::Burst) {
      CHECK(first.attempted == 512);
      CHECK(first.accepted == kSourceCapacity + 1);
      CHECK(first.dropped == 512 - (kSourceCapacity + 1));
      CHECK(first.queue_depth_max == kSourceCapacity);
    }
    print_report(first_off, 1);
    print_report(first, 1);
    print_report(second_off, 2);
    print_report(second, 2);
  }
}
