#pragma once

namespace companion_ble::internal {

// Connectable, undirected advertising of the companion service, as the
// protocol spec defines it: flags and the complete 128-bit service UUID in the
// advertising data, the complete local name in the scan response. Advertising
// stops while a central is connected (one connection at a time) and resumes on
// disconnect or after a failed connection. Runs on the NimBLE host task.
void start_advertising() noexcept;

} // namespace companion_ble::internal
