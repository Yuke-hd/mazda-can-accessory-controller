#pragma once

// Issue #161 resource-budget spike only. Not a companion BLE implementation.
namespace spike {

// Starts a connectable NimBLE advertiser with no companion GATT service when
// CONFIG_BT_NIMBLE_ENABLED is set; otherwise does nothing. Returns false only
// when NimBLE is enabled and failed to start.
bool start_ble_advertiser() noexcept;

// Logs heap and task figures when CONFIG_FREERTOS_USE_TRACE_FACILITY is set
// and a report is due; otherwise does nothing. Call from the app_main loop.
void report_resources_if_due() noexcept;

} // namespace spike
