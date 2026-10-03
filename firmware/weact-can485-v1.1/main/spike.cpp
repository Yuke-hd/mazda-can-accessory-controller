#include "spike.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdint>
#include <cstring>

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);
#endif

namespace spike {
namespace {
constexpr char kTag[] = "spike161";

#if CONFIG_BT_NIMBLE_ENABLED
// ab490000-09b6-4509-bf8e-2790253baf98, little-endian byte order.
const ble_uuid128_t kCompanionServiceUuid = BLE_UUID128_INIT(
    0x98, 0xaf, 0x3b, 0x25, 0x90, 0x27, 0x8e, 0xbf, 0x09, 0x45, 0xb6, 0x09, 0x00, 0x00, 0x49, 0xab);
std::uint8_t own_address_type = 0;

void advertise() noexcept;

int gap_event(ble_gap_event *const event, void *) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    ESP_LOGI(kTag, "connect status=%d", event->connect.status);
    if (event->connect.status != 0)
      advertise();
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(kTag, "disconnect reason=0x%x", event->disconnect.reason);
    advertise();
    break;
  case BLE_GAP_EVENT_ADV_COMPLETE:
    advertise();
    break;
  default:
    break;
  }
  return 0;
}

void advertise() noexcept {
  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &kCompanionServiceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(kTag, "adv fields rc=%d", rc);
    return;
  }

  ble_hs_adv_fields response{};
  const char *const name = ble_svc_gap_device_name();
  response.name = reinterpret_cast<const std::uint8_t *>(name);
  response.name_len = static_cast<std::uint8_t>(strlen(name));
  response.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&response);
  if (rc != 0) {
    ESP_LOGE(kTag, "scan response rc=%d", rc);
    return;
  }

  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(own_address_type, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
  if (rc != 0)
    ESP_LOGE(kTag, "adv start rc=%d", rc);
}

void on_sync() {
  if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &own_address_type) != 0) {
    ESP_LOGE(kTag, "no usable identity address");
    return;
  }
  ESP_LOGI(kTag, "host synced; advertising");
  advertise();
}

void on_reset(const int reason) { ESP_LOGW(kTag, "host reset reason=%d", reason); }

void host_task(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}
#endif

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
constexpr std::int64_t kFirstReportUs = 10'000'000;
constexpr std::int64_t kReportPeriodUs = 30'000'000;
std::int64_t next_report_us = kFirstReportUs;
TaskStatus_t task_status[32];

const char *core_name(const BaseType_t core) noexcept {
  switch (core) {
  case 0:
    return "0";
  case 1:
    return "1";
  default:
    return "any";
  }
}
#endif
} // namespace

bool start_ble_advertiser() noexcept {
#if CONFIG_BT_NIMBLE_ENABLED
  const esp_err_t error = nimble_port_init();
  if (error != ESP_OK) {
    ESP_LOGE(kTag, "nimble_port_init: %s", esp_err_to_name(error));
    return false;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_svc_gap_device_name_set("Mazda CAN Controller") != 0) {
    ESP_LOGE(kTag, "device name rejected");
    return false;
  }
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
  return true;
#else
  return true;
#endif
}

void report_resources_if_due() noexcept {
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  const std::int64_t now_us = esp_timer_get_time();
  if (now_us < next_report_us)
    return;
  next_report_us = now_us + kReportPeriodUs;

  ESP_LOGI(kTag,
           "heap t=%llds internal_free=%u internal_min=%u internal_largest=%u "
           "dram8_free=%u total_free=%u",
           static_cast<long long>(now_us / 1'000'000),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
           static_cast<unsigned>(esp_get_free_heap_size()));

  const UBaseType_t count =
      uxTaskGetSystemState(task_status, sizeof(task_status) / sizeof(task_status[0]), nullptr);
  for (UBaseType_t index = 0; index < count; ++index) {
    const TaskStatus_t &task = task_status[index];
    ESP_LOGI(kTag, "task %-16s prio=%2u core=%s stack_hwm=%u", task.pcTaskName,
             static_cast<unsigned>(task.uxCurrentPriority), core_name(xTaskGetCoreID(task.xHandle)),
             static_cast<unsigned>(task.usStackHighWaterMark));
  }
#endif
}

} // namespace spike
