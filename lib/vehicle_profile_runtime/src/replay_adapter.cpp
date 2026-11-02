#include "vehicle_profile_runtime/replay_adapter.hpp"

#include <atomic>
#include <mutex>
#include <thread>

#include "provider_access.hpp"
#include "state.hpp"

namespace vehicle_profile_runtime {
namespace {

void enter_publication(void *const context) noexcept { static_cast<std::mutex *>(context)->lock(); }

void leave_publication(void *const context) noexcept {
  static_cast<std::mutex *>(context)->unlock();
}

bool lifecycle_owner(void *const context) noexcept {
  const auto &owner = *static_cast<const std::thread::id *>(context);
  return std::this_thread::get_id() == owner;
}

} // namespace

class ReplayAdapter::Implementation final {
public:
  Implementation(const ProfileTables tables, vehicle_core::MonotonicClock &clock) noexcept
      : owner_(std::this_thread::get_id()),
        provider_(tables, clock,
                  PublicationSynchronization{&publication_mutex_, enter_publication,
                                             leave_publication, &owner_, lifecycle_owner}) {}

  [[nodiscard]] vehicle_signals::SignalProvider &provider() noexcept { return provider_; }

  [[nodiscard]] ReplayAdapterStatus start() noexcept {
    if (started_ || started_once_ || !provider_.catalog().well_formed() || !on_owner_thread())
      return ReplayAdapterStatus::InvalidState;

    ProviderAccess::set_running(provider_, true);
    if (!ProviderAccess::state(provider_).notifications.running.load(std::memory_order_acquire))
      return ReplayAdapterStatus::Faulted;
    started_ = true;
    started_once_ = true;
    ProviderAccess::dispatch_pending(provider_);
    return ReplayAdapterStatus::Ok;
  }

  [[nodiscard]] ReplayAdapterStatus process(const vehicle_core::RawCanFrame &frame) noexcept {
    if (!started_ || !on_owner_thread() || ProviderAccess::callback_active(provider_))
      return ReplayAdapterStatus::InvalidState;
    return deliver(ProviderAccess::process(provider_, frame));
  }

  [[nodiscard]] ReplayAdapterStatus service() noexcept {
    if (!started_ || !on_owner_thread() || ProviderAccess::callback_active(provider_))
      return ReplayAdapterStatus::InvalidState;
    return deliver(ProviderAccess::service(provider_));
  }

  [[nodiscard]] ReplayAdapterStatus source_fault() noexcept {
    if (!started_ || !on_owner_thread() || ProviderAccess::callback_active(provider_))
      return ReplayAdapterStatus::InvalidState;
    return deliver(ProviderAccess::fault(provider_));
  }

  [[nodiscard]] ReplayAdapterStatus stop() noexcept {
    if (!started_ || !on_owner_thread() || ProviderAccess::callback_active(provider_))
      return ReplayAdapterStatus::InvalidState;

    ProviderAccess::deliver(provider_, ProviderAccess::fault(provider_));
    ProviderAccess::set_running(provider_, false);
    if (ProviderAccess::state(provider_).notifications.running.load(std::memory_order_acquire))
      return ReplayAdapterStatus::Faulted;

    const ProviderProcessResult reset = ProviderAccess::reset(provider_);
    if (reset.status == ProviderProcessStatus::Faulted)
      return ReplayAdapterStatus::Faulted;
    started_ = false;
    return ReplayAdapterStatus::Ok;
  }

private:
  [[nodiscard]] bool on_owner_thread() const noexcept {
    return std::this_thread::get_id() == owner_;
  }

  [[nodiscard]] ReplayAdapterStatus deliver(const ProviderProcessResult &result) noexcept {
    ProviderAccess::deliver(provider_, result);
    return result.status == ProviderProcessStatus::Faulted ? ReplayAdapterStatus::Faulted
                                                           : ReplayAdapterStatus::Ok;
  }

  std::mutex publication_mutex_{};
  std::thread::id owner_;
  ProfileProvider provider_;
  bool started_{false};
  bool started_once_{false};
};

ReplayAdapter::ReplayAdapter(const ProfileTables tables,
                             vehicle_core::MonotonicClock &clock) noexcept
    : implementation_(std::make_unique<Implementation>(tables, clock)) {}

ReplayAdapter::~ReplayAdapter() noexcept = default;

vehicle_signals::SignalProvider &ReplayAdapter::provider() noexcept {
  return implementation_->provider();
}

ReplayAdapterStatus ReplayAdapter::start() noexcept { return implementation_->start(); }

ReplayAdapterStatus ReplayAdapter::process(const vehicle_core::RawCanFrame &frame) noexcept {
  return implementation_->process(frame);
}

ReplayAdapterStatus ReplayAdapter::service() noexcept { return implementation_->service(); }

ReplayAdapterStatus ReplayAdapter::source_fault() noexcept {
  return implementation_->source_fault();
}

ReplayAdapterStatus ReplayAdapter::stop() noexcept { return implementation_->stop(); }

} // namespace vehicle_profile_runtime
