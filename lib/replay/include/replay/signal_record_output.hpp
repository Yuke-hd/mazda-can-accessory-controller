#pragma once

#include <ostream>

#include "replay/signal_observer.hpp"

namespace replay {

// Writes each observed reading as one "signal" record of the replay JSONL
// stream. Records carry only replay-relative time, the catalog key, and the
// reading, spelled out without reinterpretation:
//
//   {"type":"signal","timestamp_us":N,"signal":"<key>","value":V,
//    "unit":U,"freshness":F,"availability":A,"validation":S}
//
// V is true/false, a number, the enum choice key (the raw number when it names
// no choice), or null without a value or for a non-finite number. U is "rpm",
// "km/h" or null. F is "fresh", "stale", "unverified" or null when the reading
// has no freshness (no_data, unavailable). A and S name the availability and
// validation status. Output is locale-independent and byte-stable.
class JsonlSignalRecordWriter final : public SignalObserver {
public:
  explicit JsonlSignalRecordWriter(std::ostream &output) noexcept : output_(&output) {}

  void on_catalog(vehicle_signals::SignalCatalogView catalog) noexcept override;
  void on_reading(vehicle_core::MonotonicTimestamp time_us,
                  const vehicle_signals::SignalMetadata &signal,
                  const vehicle_signals::SignalReading &reading) noexcept override;

  // False once a record could not be formatted or written.
  [[nodiscard]] bool good() const noexcept { return good_ && output_->good(); }

private:
  std::ostream *output_;
  bool good_{true};
};

} // namespace replay
