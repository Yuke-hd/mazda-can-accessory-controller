#include "action_engine/sink_fan_out.hpp"

namespace action_engine {

ConfigStatus SinkFanOut::add(ActionSink &sink) noexcept {
  if (contains(sink)) {
    return ConfigStatus::DuplicateSink;
  }
  if (count_ == kCapacity) {
    return ConfigStatus::CapacityExceeded;
  }
  sinks_[count_++] = &sink;
  return ConfigStatus::Ok;
}

void SinkFanOut::execute(const ActionCommand &command) noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    sinks_[index]->execute(command);
  }
}

bool SinkFanOut::contains(const ActionSink &sink) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (sinks_[index] == &sink) {
      return true;
    }
  }
  return false;
}

} // namespace action_engine
