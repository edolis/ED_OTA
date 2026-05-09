/**
 * @file main_OTA_test.cpp
 * @brief OTA update test using ED_OTA library with MQTT commands.
 *
 * @author Emanuele Dolis (emanuele.dolis@gmail.com)
 * @version GIT_VERSION: v0.0.3-0-gbb20eb4-dirty
 * @date 2026-05-09
 * @submodules-start
 *   ED_MQTT   : v1.2.0-0-gd7baea1
 *   ED_OTA    : v1.0.0-0-g7c85c31-dirty
 *   ED_S_JSON : v1.1.0-0-g62ddf73
 *   ED_WIFI   : v1.0.0-0-g2f08383
 * @submodules-end
 */

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mqtt_client.h"
#include <cstdio>
#include <cstring>

#include "ED_MQTT_dispatcher.h"
#include "ED_OTA.h"
#include "ED_sys.h"
#include "ED_wifi.h"
#include "secrets.h"

static const char *TAG = "MAIN_OTA_TEST";

// ESP32‑S3 Zero built‑in WS2812 LED on GPIO21
#define PIN_NEOPIXEL 21
#define NUM_LEDS 1
#define BRIGHTNESS 50

static led_strip_handle_t led_strip = nullptr;

// ---------------------------------------------------------------------
// LED strip configuration (no designated initializers)
// ---------------------------------------------------------------------
static void configure_led(void) {
  led_strip_config_t strip_config = {};
  led_strip_rmt_config_t rmt_config = {};

  strip_config.strip_gpio_num = PIN_NEOPIXEL;
  strip_config.max_leds = NUM_LEDS;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  strip_config.flags.invert_out = false;

  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10 * 1000 * 1000;
  rmt_config.mem_block_symbols = 64;
  rmt_config.flags.with_dma = false;

  ESP_ERROR_CHECK(
      led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
  led_strip_clear(led_strip);
}

static void set_led(uint8_t red, uint8_t green, uint8_t blue) {
  if (!led_strip)
    return;
  led_strip_set_pixel(led_strip, 0, red, green, blue);
  led_strip_refresh(led_strip);
}

static void clear_led() {
  if (!led_strip)
    return;
  led_strip_clear(led_strip);
}

static void led_blink_task(void *arg) {
  // Use the new Firmware API to get version components
  const char *version = ED_SYS::ESP_std::Firmware::version();
  int patch = ED_SYS::ESP_std::Firmware::patchVersion();

  uint8_t red = 0, green = 0, blue = 0;
  if (patch == 1)
    red = BRIGHTNESS;
  else if (patch == 2)
    blue = BRIGHTNESS;
  else if (patch == 3)
    green = BRIGHTNESS;
  else {
    red = green = blue = BRIGHTNESS;
  }

  ESP_LOGI(TAG, "Version %s (patch %d) -> R=%d G=%d B=%d", version, patch, red,
           green, blue);
  configure_led();

  const uint32_t blink_ms = 1000;
  while (1) {
    set_led(red, green, blue);
    vTaskDelay(pdMS_TO_TICKS(blink_ms));
    clear_led();
    vTaskDelay(pdMS_TO_TICKS(blink_ms));
  }
}

// ---------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------
extern "C" void app_main() {
  // ---- Debug: print firmware version details from the new API ----
  ESP_LOGI(TAG, "Firmware version details:");
  ESP_LOGI(TAG, "  Full version: %s", ED_SYS::ESP_std::Firmware::version());
  ESP_LOGI(TAG, "  Tag:          %s", ED_SYS::ESP_std::Firmware::tag());
  ESP_LOGI(TAG, "  Major:        %d", ED_SYS::ESP_std::Firmware::majorVersion());
  ESP_LOGI(TAG, "  Minor:        %d", ED_SYS::ESP_std::Firmware::minorVersion());
  ESP_LOGI(TAG, "  Patch:        %d", ED_SYS::ESP_std::Firmware::patchVersion());
  ESP_LOGI(TAG, "  Build number: %d", ED_SYS::ESP_std::Firmware::buildNumber());
  ESP_LOGI(TAG, "  Short hash:   %s", ED_SYS::ESP_std::Firmware::shortHash());
  ESP_LOGI(TAG, "  Full hash:    %s", ED_SYS::ESP_std::Firmware::fullHash());
  ESP_LOGI(TAG, "  Build ID:     %s", ED_SYS::ESP_std::Firmware::buildId());
  ESP_LOGI(TAG, "  Dirty:        %s", ED_SYS::ESP_std::Firmware::isDirty() ? "yes" : "no");

  // Start LED blink task
  xTaskCreate(led_blink_task, "led_blink", 4096, NULL, 1, NULL);

  // Start WiFi
  ED_wifi::WiFiService::launch();

  // ---- MQTT dispatcher configuration (required for OTA commands) ----
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = "mqtts://raspi00:8883";
  mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  mqtt_cfg.credentials.username = ED_MQTT_USERNAME;
  mqtt_cfg.credentials.client_id = ED_SYS::ESP_std::Device::mqttName();
  mqtt_cfg.credentials.authentication.password = ED_MQTT_PASSWORD;
  mqtt_cfg.session.last_will.topic = "test";
  mqtt_cfg.session.last_will.msg = "last will message";
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = true;
  mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_5;

  // Initialize and run the dispatcher (starts MQTT when IP is ready)
  ED_MQTT_dispatcher::MQTTdispatcher::initialize(&mqtt_cfg);
  ED_MQTT_dispatcher::MQTTdispatcher::run();

  // ---- OTA manager ----
  static ED_OTA::OTAmanager otaManager;
  // Subscribe OTA manager to the dispatcher (receives colon commands)
  ED_MQTT_dispatcher::MQTTdispatcher::subscribe(&otaManager);

  ESP_LOGI(TAG, "System ready. MQTT dispatcher running. OTA manager active.");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}