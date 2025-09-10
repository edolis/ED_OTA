// #region StdManifest
/**
 * @file main_Dispatcher.cpp
 * @brief shown the use of Dispatcher, commands and OTA leveraging MQTT sent command to trigger and confirm partition as valid
 * Working with TSL (MQTT) and SSH (file transfer and directory queries)
 * 
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version GIT_VERSION: v1.0.0-0-dirty
 * @tagged as: SNTP-core
 * @commit hash: g5d100c9 [5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1]
 * @build ID: P20250910-154350-5d100c9
 *  @compiledSizeInfo begin

    .iram0.text      85 874    .dram0.data  12 852
    .flash.text     860 588    .dram0.bss   21 128
    .flash.appdesc      256    ―――――――――――――――――――
    .flash.rodata   146 972    total        33 980
    ―――――――――――――――――――――――
    subtotal        1 093 690

    @compiledSizeInfo end
 * @date 2025-08-28
 */

static const char *TAG = "ESP_main_loop";

// #region BuildInfo
namespace ED_SYSINFO {
// compile time GIT status
struct GIT_fwInfo {
  static constexpr const char *GIT_VERSION = "v1.0.0.0-0-dirty";
  static constexpr const char *GIT_TAG = "SNTP-core";
  static constexpr const char *GIT_HASH = "g5d100c9";
  static constexpr const char *FULL_HASH =
      "5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1";
  static constexpr const char *BUILD_ID = "P20250828-122422-5d100c9";
};
} // namespace ED_SYSINFO
// #endregion
// #endregion

#include "ED_JSON.h"
#include "ED_sysInfo.h"
#include "ED_sysstd.h"
#include "ED_wifi.h"
#include "ED_mqtt.h"
#include "ED_MQTT_dispatcher.h"
#include "ED_OTA.h"
#include <string.h>

#ifdef DEBUG_BUILD
#endif
#include <map>
#include <esp_log.h>
#include <esp_ota_ops.h>

using namespace ED_JSON;
using namespace ED_SYSINFO;
// using namespace ED_MQTT_dispatcher;

class TestCmdReceiver: public ED_MQTT_dispatcher::CommandWithRegistry{

public:


static void EXECCommand(ED_MQTT_dispatcher::ctrlCommand * ctrcomd){

ESP_LOGI("TestCmdReceive>GrabCommandr", "executing cmd [%s] cmd Help [%s]", ctrcomd->cmdID.c_str(), ED_MQTT_dispatcher::ctrlCommand::toHelpString(*ctrcomd).c_str());

};

 void init(){

  ED_MQTT_dispatcher::ctrlCommand sdpiCmd("SDPI","Set data polling interval",
    ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL,{});

sdpiCmd.funcPointer =
    static_cast<void(*)(ED_MQTT_dispatcher::ctrlCommand*)>(&TestCmdReceiver::EXECCommand);;
ESP_LOGI(TAG,"Step_ funcpointer is null? %d",sdpiCmd.funcPointer==nullptr);

  registerCommand(sdpiCmd);
};
};

// ED_MQTT_dispatcher::CommandRegistry TestCmdReceiver::registry;

void check_ota_state_on_boot() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    ESP_LOGI(TAG,"Step_in check ota state");
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        switch (ota_state) {
            case ESP_OTA_IMG_PENDING_VERIFY:
                ESP_LOGI(TAG, "OTA: Image is PENDING_VERIFY");
                // Run your self-tests here, then either:
                // On success:
                esp_ota_mark_app_valid_cancel_rollback();
                // On failure:
                // esp_ota_mark_app_invalid_rollback_and_reboot();
                break;
            case ESP_OTA_IMG_VALID:
                ESP_LOGI(TAG, "OTA: Image is VALID");
                break;
            case ESP_OTA_IMG_INVALID:
                ESP_LOGW(TAG, "OTA: Image is INVALID");
                break;
            default:
                ESP_LOGI(TAG, "OTA: Image state = %d", ota_state);
                break;
        }
    } else {
        ESP_LOGE(TAG, "Failed to get OTA state");
    }
}

extern "C" void app_main(void) {

#ifdef DEBUG_BUILD

#endif
std::map<int, MacAddress> testMap;




  // char buffer[18] = "";

  ED_JSON::JsonEncoder encoder;
  ED_JSON::JsonEncoder encoderMAC;

  for (const auto &pair : ESP_MACstorage::getMacMap()) {
    esp_mac_type_t type = pair.first;
    const MacAddress &mac = pair.second;

    char buffer[18];
    std::string macStr = std::string(mac.toString(buffer, sizeof(buffer)));

    encoderMAC.add(std::string(esp_mac_type_str[type]), macStr);
  }
  encoder.add("deviceMACs", encoderMAC);
  encoder.add("intKey", 42);
  encoder.add("boolKey", true);
  encoder.add("nullKey", nullptr);
  encoder.add("arrayKey", std::vector<std::string>{"item1", "item2", "item3"});
static TestCmdReceiver crec;
crec.init();

ED_OTA::OTAmanager otaUpdater;

ED_SYSINFO::dump_ca_cert(ca_crt_start,ca_crt_end);


ED_wifi::WiFiService::subscribeToIPReady([&]() {
    ED_MQTT_dispatcher::MQTTdispatcher::initialize();

    ED_MQTT_dispatcher::MQTTdispatcher::subscribe(&crec);
    ED_MQTT_dispatcher::MQTTdispatcher::subscribe(&otaUpdater);
  });
  ED_wifi::WiFiService::launch();

ESP_LOGI(TAG,"***App name: %s\n", ED_sysstd::ESP_std::fwPrjName());
ESP_LOGI(TAG,"***Version: %s\n", ED_sysstd::ESP_std::fwVer());
check_ota_state_on_boot();

  // Optional hardware setup
  // gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT);

#ifdef DEBUG_BUILD

#endif

  while (true) {
    // shows the results of the first calls which will happen when network not
    // initialized, afterwards calls will get froper feedback
  // uint8_t mockMac[6] = {0x98, 0x3D, 0xAE, 0x41, 0x2F, 0x6C};
  // // ESP_LOGI(TAG,"start test 1");
  // MacAddress m(mockMac);
  // // ESP_LOGI(TAG,"start test 2");
  // testMap[0] = m;
  // ESP_LOGI(TAG,"start test 3");

  // MacAddress mad=ESP_MACstorage::getMac(ESP_MAC_BASE);
  // char buffer[18]="";
  // ESP_LOGI(TAG,"start test %s", mad.toString(buffer,sizeof(buffer),':'));
  // ESP_LOGI(TAG,"stdmqttname %s", ED_sysstd::ESP_std::mqttName());

    vTaskDelay(3000 / portTICK_PERIOD_MS);
    // ESP_LOGI(TAG, "JSON Output: %s", encoder.getJson().c_str());
  }
}
