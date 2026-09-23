#pragma once

#include <optional>

#include "mazda/definitions.hpp"
#include "mazda/state.hpp"
#include "vehicle_core/decoder_contracts.hpp"

namespace mazda::candidate {

// Keep the decoder namespace convenient for Mazda callers while using the
// frozen portable ownership/validity vocabulary at the ABI boundary.
using DecodeStatus = vehicle_core::DecodeValidity;

[[nodiscard]] DecodeStatus
decode_engine_data(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                   vehicle_core::DecoderObservation *observation = nullptr,
                   vehicle_core::HealthObservation *health = nullptr) noexcept;
[[nodiscard]] DecodeStatus decode_gear(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                       vehicle_core::DecoderObservation *observation = nullptr,
                                       vehicle_core::HealthObservation *health = nullptr) noexcept;
[[nodiscard]] DecodeStatus decode_doors(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                        vehicle_core::DecoderObservation *observation = nullptr,
                                        vehicle_core::HealthObservation *health = nullptr) noexcept;
[[nodiscard]] DecodeStatus
decode_blink_info(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                  vehicle_core::DecoderObservation *observation = nullptr,
                  vehicle_core::HealthObservation *health = nullptr) noexcept;
[[nodiscard]] DecodeStatus
decode_turn_switch(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                   std::optional<TurnEdgeEvent> *edge = nullptr,
                   vehicle_core::DecoderObservation *observation = nullptr,
                   vehicle_core::HealthObservation *health = nullptr) noexcept;
[[nodiscard]] DecodeStatus
decode_brake_pedal(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                   vehicle_core::DecoderObservation *observation = nullptr,
                   vehicle_core::HealthObservation *health = nullptr) noexcept;
// Dispatches only validated, capture-confirmed messages plus the existing
// speed candidate carried by ENGINE_DATA.
[[nodiscard]] DecodeStatus decode(const vehicle_core::RawCanFrame &frame, VehicleState &state,
                                  std::optional<TurnEdgeEvent> *edge = nullptr,
                                  vehicle_core::DecoderObservation *observation = nullptr,
                                  vehicle_core::HealthObservation *health = nullptr) noexcept;

} // namespace mazda::candidate
