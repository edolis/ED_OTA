/**
* @file main.cpp
* @brief OTA test using ED_OTA library with MQTT commands, PFREQ, and firmware info on boot.
 *
 * @author Emanuele Dolis (emanuele.dolis@gmail.com)
 * @version GIT_VERSION: v1.1.3-2-g1ca6c6c-dirty
 * @date 2026-05-15
 * @submodules-start
 *   ED_EEPROM : V1.0.0-0-g620932e-dirty
 *   ED_MQTT   : v1.3.0-2-gabaad83
 *   ED_OTA    : v2.0.0-2-gcd9ff99
 *   ED_S_JSON : v1.2.0-0-g89a8ae5
 *   ED_WIFI   : v1.0.0-1-g10b3d09
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
#include <math.h>

#include "ED_MQTT_dispatcher.h"
#include "ED_OTA.h"
#include "ED_sys.h"
#include "ED_wifi.h"
#include "secrets.h"


// ---------------------------------------------------------------------
// loads default pin assignments for the specific board
#define BOARD_VARIANT_ESP32S3_ZERO
#include "ed_board.h"

static const char *TAG = "MAIN_OTA_TEST";

#define NUM_LEDS 1
#define BRIGHTNESS 30

static led_strip_handle_t led_strip = nullptr;


// ---------------------------------------------------------------------
//   AP info provider
// ---------------------------------------------------------------------
static void wifiDiagProvider(ED_S_JSON::StaticJson& diagObj) {
    auto apInfo = ED_wifi::WiFiService::getCurrentAPInfo();
    if (apInfo.has_value()) {
        diagObj.addString("dDGT", "DTW");            // as requested
        diagObj.addString("dS", "Y");            // as requested
        diagObj.addString("d_ssid", apInfo->ssid);
        diagObj.addInt("d_rssi", apInfo->rssi);
    } else {
        diagObj.addString("dS", "N");
        diagObj.addString("d_ssid", "none");
        diagObj.addInt("d_rssi", 0);
    }
}

// ---------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------
static void configure_led(void) {
  led_strip_config_t strip_config = {};
  led_strip_rmt_config_t rmt_config = {};

  strip_config.strip_gpio_num = ED_ONBOARD_LED;
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
  if (!led_strip) return;
  led_strip_set_pixel(led_strip, 0, red, green, blue);
  led_strip_refresh(led_strip);
}

static void clear_led() {
  if (!led_strip) return;
  led_strip_clear(led_strip);
}

// ---------------------------------------------------------------------
// Rainbow colour mapping (patch 0‑9)
// ---------------------------------------------------------------------
static void hue_to_rgb(float hue, uint8_t *r, uint8_t *g, uint8_t *b) {
    hue = fmodf(hue, 360.0f);
    float s = 1.0f, v = 1.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rp, gp, bp;

    if (hue < 60)       { rp = c; gp = x; bp = 0; }
    else if (hue < 120) { rp = x; gp = c; bp = 0; }
    else if (hue < 180) { rp = 0; gp = c; bp = x; }
    else if (hue < 240) { rp = 0; gp = x; bp = c; }
    else if (hue < 300) { rp = x; gp = 0; bp = c; }
    else                { rp = c; gp = 0; bp = x; }

    *r = (uint8_t)((rp + m) * 255);
    *g = (uint8_t)((gp + m) * 255);
    *b = (uint8_t)((bp + m) * 255);
}

static void led_blink_task(void *arg) {
    const char *version = ED_SYS::ESP_std::Firmware::version();
    int patch = ED_SYS::ESP_std::Firmware::patchVersion();

    float hue = (patch % 10) * 36.0f;
    uint8_t r, g, b;
    hue_to_rgb(hue, &r, &g, &b);

    ESP_LOGI(TAG, "Version %s (patch %d, hue %.0f°) -> R=%d G=%d B=%d",
             version, patch, hue, r, g, b);
    configure_led();

    const uint32_t blink_ms = 1000;
    while (1) {
        set_led(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(blink_ms));
        clear_led();
        vTaskDelay(pdMS_TO_TICKS(blink_ms));
    }
}

// ---------------------------------------------------------------------
// Publish firmware details on boot
// ---------------------------------------------------------------------
static void publish_firmware_info(void *arg) {
    // Wait until the MQTT client handle is valid
    esp_mqtt_client_handle_t cl = nullptr;
    while ((cl = ED_MQTT_dispatcher::MQTTdispatcher::getClientHandle()) == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ED_MQTT::MqttClient *client = ED_MQTT::MqttClient::getInstance();
    if (!client) {
        ESP_LOGE(TAG, "No MQTT client instance");
        vTaskDelete(NULL);
        return;
    }

    const char *deviceID = ED_SYS::ESP_std::Device::mqttName();
    const char *version  = ED_SYS::ESP_std::Firmware::version();

    // Simple message
    char simpleMsg[128];
    snprintf(simpleMsg, sizeof(simpleMsg), "%s running %s", deviceID, version);
    client->publish("info/firmware/simple", simpleMsg, 0, false);

    // Full JSON with all version details
    char jsonBuf[512];
    snprintf(jsonBuf, sizeof(jsonBuf),
        "{"
        "\"device\":\"%s\","
        "\"project\":\"%s\","
        "\"version\":\"%s\","
        "\"tag\":\"%s\","
        "\"major\":%d,"
        "\"minor\":%d,"
        "\"patch\":%d,"
        "\"build\":%d,"
        "\"hash_short\":\"%s\","
        "\"hash_full\":\"%s\","
        "\"build_id\":\"%s\","
        "\"dirty\":%s"
        "}",
        deviceID,
        ED_SYS::ESP_std::Firmware::prjName(),
        version,
        ED_SYS::ESP_std::Firmware::tag(),
        ED_SYS::ESP_std::Firmware::majorVersion(),
        ED_SYS::ESP_std::Firmware::minorVersion(),
        ED_SYS::ESP_std::Firmware::patchVersion(),
        ED_SYS::ESP_std::Firmware::buildNumber(),
        ED_SYS::ESP_std::Firmware::shortHash(),
        ED_SYS::ESP_std::Firmware::fullHash(),
        ED_SYS::ESP_std::Firmware::buildId(),
        ED_SYS::ESP_std::Firmware::isDirty() ? "true" : "false"
    );
    client->publish("info/firmware", jsonBuf, 0, false);

    ESP_LOGI(TAG, "Published firmware info");
    vTaskDelete(NULL);
}
// ---------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------
extern "C" void app_main() {
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

    xTaskCreate(led_blink_task, "led_blink", 4096, NULL, 1, NULL);

    ED_wifi::WiFiService::launch();

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

    ED_MQTT_dispatcher::MQTTdispatcher::initialize(&mqtt_cfg);
    ED_MQTT_dispatcher::MQTTdispatcher::registerJsonFieldProvider(wifiDiagProvider);
    ED_MQTT_dispatcher::MQTTdispatcher::run();

    static ED_OTA::OTAmanager otaManager;
    ED_MQTT_dispatcher::MQTTdispatcher::subscribe(&otaManager);

    // Publish firmware info once MQTT is ready
    xTaskCreate(publish_firmware_info, "fw_info_pub", 4096, NULL, 1, NULL);

    ESP_LOGI(TAG, "System ready. MQTT dispatcher running. OTA manager active.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}