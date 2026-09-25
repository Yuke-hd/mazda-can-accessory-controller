#include "action_engine/sink_fan_out.hpp"

namespace action_engine {

ConfigStatus SinkFanOut::add(ActionSink &sink) noexcept {
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

} // namespace action_engine
