#include "companion_protocol/extensible_live_stream.hpp"

namespace companion_protocol {

namespace {

std::uint16_t next_handle(const std::uint16_t handle, const std::uint16_t signal_count) noexcept {
  return handle >= signal_count ? std::uint16_t{1} : static_cast<std::uint16_t>(handle + 1U);
}

bool append_result(
    ExtensibleLiveSignalPacketBuilder &builder, const std::uint16_t handle,
    const vehicle_signals::SignalResult<vehicle_signals::SignalReading> &result) noexcept {
  if (result.ok()) {
    return builder.append_reading(handle, *result.value);
  }
  return builder.append_failure(handle, result.status);
}

} // namespace

bool ExtensibleLiveSignalStream::streaming() const noexcept {
  return catalog_.available() && conditions_.notifications_enabled && conditions_.link_secured &&
         conditions_.att_mtu >= kMinLiveSignalsAttMtu && conditions_.catalog_token != 0;
}

void ExtensibleLiveSignalStream::restart_stream() noexcept {
  accepted_ = {};
  empty_packet_accepted_at_.reset();
  next_sequence_ = 0;
  next_handle_ = 1;
}

template <typename Update>
void ExtensibleLiveSignalStream::update_conditions(Update update) noexcept {
  const bool was_streaming = streaming();
  update(conditions_);
  if (!was_streaming && streaming()) {
    restart_stream();
  }
}

void ExtensibleLiveSignalStream::set_notifications_enabled(const bool enabled) noexcept {
  update_conditions(
      [enabled](Conditions &conditions) { conditions.notifications_enabled = enabled; });
}

void ExtensibleLiveSignalStream::set_link_secured(const bool secured) noexcept {
  update_conditions([secured](Conditions &conditions) { conditions.link_secured = secured; });
}

void ExtensibleLiveSignalStream::set_att_mtu(const std::uint16_t mtu) noexcept {
  update_conditions([mtu](Conditions &conditions) { conditions.att_mtu = mtu; });
}

void ExtensibleLiveSignalStream::set_catalog_token(const std::uint32_t token) noexcept {
  if (conditions_.catalog_token == token) {
    return;
  }
  conditions_.catalog_token = token;
  restart_stream();
}

void ExtensibleLiveSignalStream::reset_connection() noexcept {
  conditions_ = {};
  accepted_ = {};
  last_sample_.reset();
  empty_packet_accepted_at_.reset();
  next_sequence_ = 0;
  next_handle_ = 1;
  config_paused_ = false;
}

bool ExtensibleLiveSignalStream::sample_due(const LiveStreamClock now) const noexcept {
  return !last_sample_.has_value() || now < *last_sample_ ||
         now - *last_sample_ >= kLiveFrameInterval;
}

std::optional<LiveStreamClock>
ExtensibleLiveSignalStream::time_until_sample(const LiveStreamClock now) const noexcept {
  if (!streaming() || config_paused_) {
    return std::nullopt;
  }
  if (sample_due(now)) {
    return LiveStreamClock{0};
  }
  return *last_sample_ + kLiveFrameInterval - now;
}

bool ExtensibleLiveSignalStream::record_due(const std::uint16_t handle,
                                            const ExtensibleLiveSignalPacket &single_record,
                                            const LiveStreamClock now) const noexcept {
  if (handle == 0 || handle > catalog_.signal_count() ||
      single_record.size() <= kExtensibleLiveSignalHeaderBytes) {
    return false;
  }

  const AcceptedRecord &accepted = accepted_[handle - 1U];
  const std::size_t record_size = single_record.size() - kExtensibleLiveSignalHeaderBytes;
  if (!accepted.accepted_at.has_value() || accepted.size != record_size) {
    return true;
  }
  for (std::size_t index = 0; index < record_size; ++index) {
    if (accepted.bytes[index] != single_record[kExtensibleLiveSignalHeaderBytes + index]) {
      return true;
    }
  }
  return now < *accepted.accepted_at || now - *accepted.accepted_at >= kLiveHeartbeatInterval;
}

bool ExtensibleLiveSignalStream::empty_packet_due(const LiveStreamClock now) const noexcept {
  return !empty_packet_accepted_at_.has_value() || now < *empty_packet_accepted_at_ ||
         now - *empty_packet_accepted_at_ >= kLiveHeartbeatInterval;
}

void ExtensibleLiveSignalStream::remember_accepted_records(const ExtensibleLiveSignalPacket &packet,
                                                           const LiveStreamClock now) noexcept {
  std::size_t offset = kExtensibleLiveSignalHeaderBytes;
  while (offset < packet.size()) {
    const std::size_t record_size = 1U + packet[offset];
    if (record_size > kMaxRecordBytes || offset + record_size > packet.size() ||
        record_size < kExtensibleLiveSignalRecordHeaderBytes) {
      return;
    }
    const std::uint16_t handle = static_cast<std::uint16_t>(packet[offset + 1U]) |
                                 static_cast<std::uint16_t>(packet[offset + 2U] << 8U);
    if (handle == 0 || handle > catalog_.signal_count()) {
      return;
    }
    AcceptedRecord &accepted = accepted_[handle - 1U];
    accepted.size = static_cast<std::uint8_t>(record_size);
    for (std::size_t index = 0; index < record_size; ++index) {
      accepted.bytes[index] = packet[offset + index];
    }
    accepted.accepted_at = now;
    offset += record_size;
  }
}

bool ExtensibleLiveSignalStream::offer(const LiveStreamClock now, const bool telemetry_started,
                                       ExtensibleLiveFrameSink &sink) noexcept {
  if (!streaming() || config_paused_ || !sample_due(now)) {
    return false;
  }
  last_sample_ = now;

  ExtensibleLiveSignalPacketBuilder packet_builder{
      catalog_,       provider_,         conditions_.catalog_token,
      next_sequence_, telemetry_started, conditions_.att_mtu};
  if (!packet_builder.valid()) {
    return false;
  }

  if (catalog_.signal_count() == 0) {
    if (!empty_packet_due(now)) {
      return false;
    }
    ++next_sequence_;
    if (sink.try_queue(packet_builder.packet())) {
      empty_packet_accepted_at_ = now;
    }
    return true;
  }

  const std::uint16_t signal_count = catalog_.signal_count();
  std::uint16_t handle = next_handle_;
  std::uint16_t cursor_after_attempt = next_handle_;
  bool appended = false;
  for (std::uint16_t visited = 0; visited < signal_count; ++visited) {
    const auto result = provider_.read(catalog_.provider_id_for_handle(handle));
    ExtensibleLiveSignalPacketBuilder single_record{
        catalog_,       provider_,         conditions_.catalog_token,
        next_sequence_, telemetry_started, conditions_.att_mtu};
    if (append_result(single_record, handle, result) &&
        record_due(handle, single_record.packet(), now)) {
      if (!append_result(packet_builder, handle, result)) {
        cursor_after_attempt = handle;
        break;
      }
      appended = true;
      cursor_after_attempt = next_handle(handle, signal_count);
    }
    handle = next_handle(handle, signal_count);
  }

  if (!appended) {
    return false;
  }

  next_handle_ = cursor_after_attempt;
  ++next_sequence_;
  if (sink.try_queue(packet_builder.packet())) {
    remember_accepted_records(packet_builder.packet(), now);
  }
  return true;
}

} // namespace companion_protocol
