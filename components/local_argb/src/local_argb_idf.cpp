#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"

#include "board/board_config.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_spi.h"
#include "vehicle_core/signal.hpp"

#include <cstddef>
#include <cstdint>

namespace local_argb {
namespace {

constexpr char kTag[] = "local_argb";
constexpr std::uint32_t kRmtResolutionHz = 10'000'000;
// SPI3 is dedicated to the vehicle strip. The led_strip SPI backend owns the
// whole bus because it has no chip-select concept.
constexpr spi_host_device_t kVehicleStripSpiBus = SPI3_HOST;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 2;
// The guard performs no LED/RMT work and is above can_rx (MAX-2), so a stuck
// lower-priority LED call cannot prevent the configured reset bound.
constexpr UBaseType_t kSupervisorPriority = configMAX_PRIORITIES - 1;
constexpr std::uint32_t kWorkerStackDepth = 4096;
constexpr std::uint32_t kSupervisorStackDepth = 2048;
constexpr TickType_t kWorkerPollTicks =
    pdMS_TO_TICKS(kSupervisorPollUs / 1'000) == 0 ? 1 : pdMS_TO_TICKS(kSupervisorPollUs / 1'000);

internal::DriverWatchdog g_driver_watchdog{};
internal::WorkerLease g_worker_lease{};
portMUX_TYPE g_watchdog_lock = portMUX_INITIALIZER_UNLOCKED;

vehicle_core::MonotonicTimestamp now_us() noexcept {
  return static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
}

void begin_driver_write() noexcept {
  const auto started_us = now_us();
  taskENTER_CRITICAL(&g_watchdog_lock);
  g_driver_watchdog.begin(started_us);
  taskEXIT_CRITICAL(&g_watchdog_lock);
}

void end_driver_write() noexcept {
  taskENTER_CRITICAL(&g_watchdog_lock);
  g_driver_watchdog.end();
  taskEXIT_CRITICAL(&g_watchdog_lock);
}

void arm_worker_lease() noexcept {
  const auto started_us = now_us();
  taskENTER_CRITICAL(&g_watchdog_lock);
  g_worker_lease.arm(started_us);
  taskEXIT_CRITICAL(&g_watchdog_lock);
}

void heartbeat_worker() noexcept {
  const auto progress_us = now_us();
  taskENTER_CRITICAL(&g_watchdog_lock);
  g_worker_lease.heartbeat(progress_us);
  taskEXIT_CRITICAL(&g_watchdog_lock);
}

void disarm_worker_lease() noexcept {
  taskENTER_CRITICAL(&g_watchdog_lock);
  g_worker_lease.disarm();
  taskEXIT_CRITICAL(&g_watchdog_lock);
}

class LedStripSink final : public PixelSink, public PixelFrameSink {
public:
  void set_handles(const led_strip_handle_t onboard_handle,
                   const led_strip_handle_t vehicle_handle) noexcept {
    onboard_handle_ = onboard_handle;
    vehicle_handle_ = vehicle_handle;
  }

  bool write(const Rgb color) noexcept override {
    PixelFrame frame{};
    frame.fill(color);
    return write(frame);
  }

  bool write(const PixelFrame &frame) noexcept override {
    begin_driver_write();
    const Rgb status = internal::onboard_status_color(frame);
    bool vehicle_success = vehicle_handle_ != nullptr;
    for (std::size_t index = 0; vehicle_success && index < frame.size(); ++index) {
      const auto &pixel = frame[index];
      vehicle_success =
          led_strip_set_pixel(vehicle_handle_, index, pixel.red, pixel.green, pixel.blue) == ESP_OK;
    }
    vehicle_success = vehicle_success && led_strip_refresh(vehicle_handle_) == ESP_OK;

    const bool onboard_success =
        onboard_handle_ != nullptr &&
        led_strip_set_pixel(onboard_handle_, 0, status.red, status.green, status.blue) == ESP_OK;
    const bool onboard_refreshed = onboard_success && led_strip_refresh(onboard_handle_) == ESP_OK;
    end_driver_write();
    return vehicle_success && onboard_refreshed;
  }

private:
  led_strip_handle_t onboard_handle_{nullptr};
  led_strip_handle_t vehicle_handle_{nullptr};
};

LedStripSink g_sink;
internal::RendererController g_controller{g_sink, internal::kCenterOutFillAnimation};
class QueueSink final : public internal::LightingSink {
public:
  bool publish(const internal::LightingCommand &command) noexcept override;
};

QueueSink g_queue_sink;
led_strip_handle_t g_onboard_strip{nullptr};
led_strip_handle_t g_vehicle_strip{nullptr};
StaticQueue_t g_queue_storage{};
std::uint8_t g_queue_buffer[sizeof(internal::LightingCommand)]{};
QueueHandle_t g_queue{nullptr};
TaskHandle_t g_worker{nullptr};
TaskHandle_t g_supervisor{nullptr};
bool g_started{false};

void worker(void *) noexcept {
  for (;;) {
    heartbeat_worker();
    internal::LightingCommand command{};
    if (xQueueReceive(g_queue, &command, kWorkerPollTicks) == pdTRUE) {
      if (!g_controller.apply(command, now_us())) {
        ESP_LOGE(kTag, "pixel write failed; fail-off clear scheduled for retry");
      }
    } else if (!g_controller.tick(now_us())) {
      ESP_LOGE(kTag, "pixel fail-off clear retry failed");
    }
  }
}

void supervisor(void *) noexcept {
  for (;;) {
    vTaskDelay(kWorkerPollTicks);
    const auto checked_us = now_us();
    taskENTER_CRITICAL(&g_watchdog_lock);
    const bool restart_due =
        g_driver_watchdog.restart_due(checked_us) || g_worker_lease.restart_due(checked_us);
    taskEXIT_CRITICAL(&g_watchdog_lock);
    if (restart_due) {
      // led_strip 3.0.3 can wait indefinitely for peripheral completion. A
      // reset is the only bounded recovery available without access to its
      // private channel or bus; boot safe-defaults and startup black run again
      // before CAN can restart.
      esp_restart();
    }
  }
}

void stop_supervisor() noexcept {
  if (g_supervisor != nullptr) {
    vTaskDelete(g_supervisor);
    g_supervisor = nullptr;
  }
}

} // namespace

bool QueueSink::publish(const internal::LightingCommand &command) noexcept {
  // xQueueOverwrite never waits: a fresh command replaces obsolete pending
  // state while the worker remains the sole owner of LED driver calls.
  return g_started && g_queue != nullptr && xQueueOverwrite(g_queue, &command) == pdPASS;
}

namespace internal {

LightingSink &sink() noexcept { return g_queue_sink; }

} // namespace internal

bool start() noexcept {
  if (g_started) {
    return true;
  }
  static_assert(board::kWeActCan485V11.onboard_rgb.data == 4,
                "local ARGB is fixed to the GPIO4 onboard LED");
  static_assert(board::kWeActCan485V11.onboard_rgb.pixel_count == board::kOnboardRgbPixelCount,
                "local ARGB physical sink is configured for one onboard pixel");
  static_assert(board::kOnboardRgbPixelCount == 1,
                "local ARGB physical sink must remain a single-pixel status output");
  static_assert(board::kWeActCan485V11.vehicle_light_strip.data == 16,
                "local ARGB vehicle strip is fixed to GPIO16");
  static_assert(board::kWeActCan485V11.vehicle_light_strip.pixel_count ==
                    board::kVehicleLightStripPixelCount,
                "local ARGB vehicle strip must expose the full 100-pixel frame");
  static_assert(kLedCount == board::kVehicleLightStripPixelCount,
                "local ARGB logical renderer must match the 100-pixel vehicle strip");

  if (xTaskCreate(supervisor, "argb_guard", kSupervisorStackDepth, nullptr, kSupervisorPriority,
                  &g_supervisor) != pdPASS) {
    return false;
  }

  led_strip_config_t strip_config{};
  strip_config.strip_gpio_num = board::kWeActCan485V11.onboard_rgb.data;
  strip_config.max_leds = board::kWeActCan485V11.onboard_rgb.pixel_count;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config{};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = kRmtResolutionHz;
  rmt_config.mem_block_symbols = 64;
  rmt_config.flags.with_dma = false;
  if (led_strip_new_rmt_device(&strip_config, &rmt_config, &g_onboard_strip) != ESP_OK) {
    stop_supervisor();
    return false;
  }

  strip_config.strip_gpio_num = board::kWeActCan485V11.vehicle_light_strip.data;
  strip_config.max_leds = board::kWeActCan485V11.vehicle_light_strip.pixel_count;
  led_strip_spi_config_t spi_config{};
  spi_config.clk_src = SPI_CLK_SRC_DEFAULT;
  spi_config.spi_bus = kVehicleStripSpiBus;
  spi_config.flags.with_dma = true;
  if (led_strip_new_spi_device(&strip_config, &spi_config, &g_vehicle_strip) != ESP_OK) {
    (void)led_strip_del(g_onboard_strip);
    g_onboard_strip = nullptr;
    stop_supervisor();
    return false;
  }
  g_sink.set_handles(g_onboard_strip, g_vehicle_strip);

  // Send a physical black frame before CAN starts. Retry twice if the driver
  // reports a transient failure; GPIO-low alone cannot clear a latched pixel.
  bool cleared = g_controller.start();
  for (std::uint8_t retry = 0; !cleared && retry < 2; ++retry) {
    cleared = g_controller.tick(now_us());
  }
  if (!cleared) {
    (void)led_strip_del(g_vehicle_strip);
    (void)led_strip_del(g_onboard_strip);
    g_vehicle_strip = nullptr;
    g_onboard_strip = nullptr;
    g_sink.set_handles(nullptr, nullptr);
    stop_supervisor();
    return false;
  }

  g_queue =
      xQueueCreateStatic(1, sizeof(internal::LightingCommand), g_queue_buffer, &g_queue_storage);
  const BaseType_t worker_created = g_queue == nullptr
                                        ? pdFAIL
                                        : xTaskCreate(worker, "local_argb", kWorkerStackDepth,
                                                      nullptr, kWorkerPriority, &g_worker);
  if (worker_created == pdPASS) {
    // Task creation and startup black are complete before monitoring begins,
    // so initialization cannot be mistaken for a worker stall.
    arm_worker_lease();
  }
  if (worker_created != pdPASS) {
    disarm_worker_lease();
    (void)g_sink.write(kBlack);
    (void)led_strip_del(g_vehicle_strip);
    (void)led_strip_del(g_onboard_strip);
    g_vehicle_strip = nullptr;
    g_onboard_strip = nullptr;
    g_sink.set_handles(nullptr, nullptr);
    g_queue = nullptr;
    stop_supervisor();
    return false;
  }
  g_started = true;
  return true;
}

void fail_off() noexcept {
  const internal::LightingCommand command{};
  if (!g_started || g_queue == nullptr || xQueueOverwrite(g_queue, &command) != pdPASS) {
    (void)g_sink.write(kBlack);
  }
}

} // namespace local_argb
