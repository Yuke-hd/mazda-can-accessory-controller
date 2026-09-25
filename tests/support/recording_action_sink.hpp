#pragma once

#include <mutex>
#include <vector>

#include "action_engine/action.hpp"

namespace test_support {

// Host test sink that records every command it receives, in order. Recording
// is guarded by a mutex so a test thread can inspect commands delivered on a
// provider's dispatcher thread. Test-only: it allocates while recording.
class RecordingActionSink final : public action_engine::ActionSink {
public:
  void execute(const action_engine::ActionCommand &command) noexcept override {
    const std::lock_guard<std::mutex> lock{mutex_};
    commands_.push_back(command);
  }

  [[nodiscard]] std::vector<action_engine::ActionCommand> commands() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return commands_;
  }
  // Returns the recorded commands and starts a new recording.
  [[nodiscard]] std::vector<action_engine::ActionCommand> take() {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<action_engine::ActionCommand> taken{};
    taken.swap(commands_);
    return taken;
  }

private:
  mutable std::mutex mutex_{};
  std::vector<action_engine::ActionCommand> commands_{};
};

} // namespace test_support
