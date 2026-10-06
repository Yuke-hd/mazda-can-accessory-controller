#include "companion_protocol/live_signal_stream.hpp"

namespace companion_protocol {

bool LiveSignalStream::Conditions::hold() const noexcept {
  return notifications_enabled && link_secured && att_mtu >= kMinLiveSignalsAttMtu;
}

// Streaming (re)started: the sequence restarts at 0 and the baseline is
// cleared, so the first frame is sent even if it matches an earlier one. The
// sample and attempt times stay, so the rate cap holds across a re-enable on
// the same connection.
void LiveSignalStream::Pacing::restart() noexcept {
  next_sequence = 0;
  accepted.reset();
}

// A clock that stepped back counts as due, so the stream never stalls.
bool LiveSignalStream::Pacing::sample_due(const LiveStreamClock now) const noexcept {
  return !last_sample.has_value() || now < *last_sample || now - *last_sample >= kLiveFrameInterval;
}

bool LiveSignalStream::Pacing::frame_due(const LiveStreamClock now,
                                         const LiveSignalContent &content) const noexcept {
  if (!accepted.has_value() || *accepted != content)
    return true;
  return !last_attempt.has_value() || now < *last_attempt ||
         now - *last_attempt >= kLiveHeartbeatInterval;
}

template <typename Update> void LiveSignalStream::update_conditions(Update update) noexcept {
  const bool was_streaming = conditions_.hold();
  update(conditions_);
  if (!was_streaming && conditions_.hold())
    pacing_.restart();
}

void LiveSignalStream::set_notifications_enabled(const bool enabled) noexcept {
  update_conditions(
      [enabled](Conditions &conditions) { conditions.notifications_enabled = enabled; });
}

void LiveSignalStream::set_link_secured(const bool secured) noexcept {
  update_conditions([secured](Conditions &conditions) { conditions.link_secured = secured; });
}

void LiveSignalStream::set_att_mtu(const std::uint16_t mtu) noexcept {
  update_conditions([mtu](Conditions &conditions) { conditions.att_mtu = mtu; });
}

void LiveSignalStream::reset_connection() noexcept {
  conditions_ = Conditions{};
  pacing_ = Pacing{};
}

bool LiveSignalStream::streaming() const noexcept { return conditions_.hold(); }

std::optional<LiveStreamClock>
LiveSignalStream::time_until_sample(const LiveStreamClock now) const noexcept {
  if (!streaming())
    return std::nullopt;
  if (pacing_.sample_due(now))
    return LiveStreamClock{0};
  return *pacing_.last_sample + kLiveFrameInterval - now;
}

bool LiveSignalStream::offer(const LiveStreamClock now, const LiveSignalContent &content,
                             LiveFrameSink &sink) noexcept {
  if (!streaming() || !pacing_.sample_due(now))
    return false;
  pacing_.last_sample = now;
  if (!pacing_.frame_due(now, content))
    return false;
  pacing_.last_attempt = now;
  const std::uint8_t sequence = pacing_.next_sequence++;
  if (sink.try_queue(content.frame(sequence)))
    pacing_.accepted = content;
  return true;
}

} // namespace companion_protocol
