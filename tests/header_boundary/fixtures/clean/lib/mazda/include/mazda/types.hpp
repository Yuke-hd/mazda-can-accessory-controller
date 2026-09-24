#pragma once

#include <cstdint>

// The typed facade legitimately reaches Mazda value types; the generic
// provider must not.
namespace mazda {

enum class TurnState : std::uint8_t { Unknown, Off, Left, Right, Hazard };

} // namespace mazda
