#pragma once

namespace vehicle_core {

template <typename T> struct Reading { T value{}; };

template <typename T> struct Notification { Reading<T> current{}; };

} // namespace vehicle_core
