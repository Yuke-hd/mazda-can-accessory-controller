#include "companion_ble/advertising.hpp"

#include "companion_ble/nimble_uuid.hpp"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#include <cstdint>
#include <cstring>

namespace companion_ble::internal {
namespace {

constexpr const char *kTag = "companion_ble";

const ble_uuid128_t advertised_service_uuid = nimble_uuid(CompanionAttribute::Service);

int handle_gap_event(ble_gap_event *event, void *argument);

int set_advertising_fields() noexcept {
  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &advertised_service_uuid;
  fields.num_uuids128 = 1U;
  fields.uuids128_is_complete = 1U;
  return ble_gap_adv_set_fields(&fields);
}

int set_scan_response_fields() noexcept {
  const char *const name = ble_svc_gap_device_name();
  ble_hs_adv_fields fields{};
  fields.name = reinterpret_cast<const std::uint8_t *>(name);
  fields.name_len = static_cast<std::uint8_t>(std::strlen(name));
  fields.name_is_complete = 1U;
  return ble_gap_adv_rsp_set_fields(&fields);
}

int begin_advertising() noexcept {
  std::uint8_t own_address_type = 0U;
  int rc = ble_hs_id_infer_auto(0, &own_address_type);
  if (rc != 0)
    return rc;
  rc = set_advertising_fields();
  if (rc != 0)
    return rc;
  rc = set_scan_response_fields();
  if (rc != 0)
    return rc;
  ble_gap_adv_params parameters{};
  parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
  parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
  return ble_gap_adv_start(own_address_type, nullptr, BLE_HS_FOREVER, &parameters, handle_gap_event,
                           nullptr);
}

int handle_gap_event(ble_gap_event *const event, void * /*argument*/) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    // A connected central stops advertising; a failed attempt resumes it.
    if (event->connect.status != 0)
      start_advertising();
    return 0;
  case BLE_GAP_EVENT_DISCONNECT:
  case BLE_GAP_EVENT_ADV_COMPLETE:
    start_advertising();
    return 0;
  default:
    return 0;
  }
}

} // namespace

void start_advertising() noexcept {
  const int rc = begin_advertising();
  if (rc != 0)
    ESP_LOGW(kTag, "advertising did not start (rc=%d)", rc);
}

} // namespace companion_ble::internal
