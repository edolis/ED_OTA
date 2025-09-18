#include "ED_OTA.h"
#include "ED_sysInfo.h"
#include "ED_sysstd.h"
#include <driver/gpio.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <regex.h>
#include <string>
#include "esp_crt_bundle.h"

namespace ED_OTA {
static const char *TAG = "ED_OTA";
// *** notice! replaced by bundle to avoid using RAM
// extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
// extern const uint8_t ca_crt_end[] asm("_binary_ca_crt_end");

bool scanFirmware(FirmwareScanner &scanner, const std::string &url) {
  int bytes_read;
  uint8_t c_buffer[BUFFER_SIZE];
  // ESP_LOGI(TAG, "Step_scanfirmware");
  esp_http_client_config_t config = {
      .url = url.c_str(),
      // .disable_auto_redirect = false,
      // .cert_pem = (const char *)ca_crt_start,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .crt_bundle_attach = esp_crt_bundle_attach,

  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  // ESP_LOGI(TAG, "Step_1");
  esp_err_t err = esp_http_client_open(client, 0);
  // ESP_LOGI(TAG, "Step_2");
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    ESP_LOGE(TAG, "failed to open client to %s error: %s ", config.url,
             esp_err_to_name(err));
    return false;
  }
  esp_http_client_fetch_headers(client); //required for proper fucntioning of read
  // ESP_LOGI(TAG, "Step_3");
  // ESP_LOGI(TAG, "Content length: %d", content_length);
  do {
    bytes_read =
        esp_http_client_read(client, (char *)c_buffer, BUFFER_SIZE - 1);
    // ESP_LOGI(TAG, "Step_ bytes read %d", bytes_read);
    if (bytes_read > 0) {
      c_buffer[bytes_read] = '\0'; // Null-terminated for safe logging
      // ESP_LOGI(TAG, "Step_parsing: %.*s", bytes_read, c_buffer);
      scanner.file_scanner_parse_chunk((char *)c_buffer, bytes_read);
    }
  } while (bytes_read > 0);

  esp_http_client_cleanup(client);
  return scanner.targetFwFile() != nullptr;
}

// #region FirmwareScanner

FirmwareScanner::FirmwareScanner(const char *curFwarePrj, const char *refFwVer,
                                 UpdateType mode)
    : prjID(curFwarePrj), buffer(""), carryover(""), best_filename(""),
      matchingVersionFound(false) {
  // ESP_LOGI(TAG, "Step_in FirmwareScanner");

  for (size_t i = 0; i < 4; i++)indexLock[i] = false;

  regex_t regex;
  regmatch_t matches[5];
  const char *pattern =
      "v([[:digit:]]+)[.]([[:digit:]]+)[.]([[:digit:]]+)-([[:digit:]]+)";

  if (regcomp(&regex, pattern, REG_EXTENDED) == 0) {
    if (regexec(&regex, refFwVer, 5, matches, 0) == 0) {
      for (int i = 0; i < 4; i++) {
        int start = matches[i + 1].rm_so;
        int end = matches[i + 1].rm_eo;
        if (start != -1 && end != -1) {
          char temp[16];
          int vlen = end - start;
          strncpy(temp, refFwVer + start, vlen);
          temp[vlen] = '\0';
          best_version[i] = atoi(temp);
          if (mode == UpdateType::UPDATE_TO_SPECIFIC)
            indexLock[i] = true;
        } else {
          best_version[i] = -1;
        }
      }
    }
    regfree(&regex);
  }
}

bool FirmwareScanner::is_version_higher(int new_v[4], int best_v[4]) {

  for (int i = 0; i < 4; i++) {
    if (!indexLock[i]) {
      if (new_v[i] > best_v[i])
        return true;
      if (new_v[i] < best_v[i])
        return false;
    } else {
      if (new_v[i] != best_v[i])
        return false;
    }
  }
  return false;
}

void FirmwareScanner::file_scanner_parse_chunk(const char *chunk,
                                               size_t chunk_len) {
  // Merge carryover + new chunk
  // ESP_LOGI(TAG, "Step_in filescanner");
  size_t carry_len = strlen(carryover);
  memcpy(buffer, carryover, carry_len);
  memcpy(buffer + carry_len, chunk, chunk_len);
  buffer[carry_len + chunk_len] = '\0';

  // Compile regex
  regex_t regex;
  regmatch_t matches[6];
  char pattern[128];
  snprintf(pattern, sizeof(pattern),
           "href=\\\"(%s_v([[:digit:]]+)\\.([[:digit:]]+)\\.([[:digit:]]+)[^"
           "\\\"]*\\.bin(\\.lz4)?)\\\"",
           prjID);

  if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
    ESP_LOGE(TAG, "could not compile regex pattern for firmware dat");
    return;
  }

  char *ptr = buffer;
  while (regexec(&regex, ptr, 6, matches, 0) == 0) {
    // Extract full filename
    int len = matches[1].rm_eo - matches[1].rm_so;
    char filename[MAX_FILENAME_LEN];
    strncpy(filename, ptr + matches[1].rm_so, len);
    filename[len] = '\0';
    // ESP_LOGI(TAG, "Step_ filename %s", filename);

    // Extract version components
    int version[4];
    for (int i = 0; i < 4; i++) {
      int start = matches[i + 2].rm_so;
      int end = matches[i + 2].rm_eo;
      char temp[16];
      int vlen = end - start;
      strncpy(temp, ptr + start, vlen);
      temp[vlen] = '\0';
      version[i] = atoi(temp);
    }
    ESP_LOGI(TAG, "Evaluating candidate: %s → %d.%d.%d-%d", filename,
             version[0], version[1], version[2], version[3]);

    if (is_version_higher(version, best_version)) {
      strncpy(best_filename, filename, MAX_FILENAME_LEN);
      memcpy(best_version, version, sizeof(version));
      matchingVersionFound = true;
    }

    ptr += matches[0].rm_eo;
  }

  regfree(&regex);

  // Save last CARRYOVER_SIZE bytes for next chunk
  size_t total_len = carry_len + chunk_len;
  if (total_len >= CARRYOVER_SIZE) {
    memcpy(carryover, buffer + total_len - CARRYOVER_SIZE, CARRYOVER_SIZE);
    carryover[CARRYOVER_SIZE] = '\0';
  } else {
    strcpy(carryover, buffer);
  }
}

const char *FirmwareScanner::targetFwFile() {
  if (matchingVersionFound)
    return best_filename;
  else
    return nullptr;
};



// #endregion FirmwareScanner


//#region OTAmanager


OTAmanager::OTAmanager( )
   {
  ED_MQTT_dispatcher::ctrlCommand cmd(
      "FWUP", "Update firmware via OTA",
      ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL,
      {{"default", ""}} // default parameter: empty string
  );
  cmd.funcPointer = [this](ED_MQTT_dispatcher::ctrlCommand *cmd) {
    this->cmd_launchUpdate(cmd);
  };
  registerCommand(cmd);
  ED_MQTT_dispatcher::ctrlCommand cmd1(
      "FWCO", "Confirms OTA partition as valid",
      ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL, {});
  cmd1.funcPointer = [this](ED_MQTT_dispatcher::ctrlCommand *cmd1) {
    this->cmd_otaValidate(cmd1);
  };
  registerCommand(cmd1);

  ED_MQTT_dispatcher::ctrlCommand cmd2(
      "FWQS", "Query Status of running OTA",
      ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL, {});
  cmd2.funcPointer = [this](ED_MQTT_dispatcher::ctrlCommand *cmd2) {
    this->cmd_getFwStatus(cmd2);
  };
  registerCommand(cmd2);
};

void OTAmanager::ota_update_task(void *pvParameter) {
  // --- All variables declared at top ---
  const char *verRef = static_cast<const char *>(pvParameter);
  uint8_t *c_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  uint8_t *d_buffer = (uint8_t *)malloc(BUFFER_SIZE);
  esp_http_client_config_t config = {};
  esp_http_client_handle_t client = nullptr;
  esp_ota_handle_t ota_handle = 0;
  LZ4_streamDecode_t *lz4_stream = nullptr;
  std::string fullUrl;
  size_t bytes_read = 0;
  esp_err_t err = ESP_OK;
  bool ota_data_written = false;
  int status = 0;
  const char *version = "";
  const char *httpPath = "";
  const esp_partition_t *update_partition = nullptr;
  FirmwareScanner *fwScanner = nullptr;

  // --- Dynamic dictionary allocation (16 KB) ---
  // needs to be in synch with the compressor utility in python
  const int LZ4_DICT_SIZE = 16 * 1024;
  uint8_t *dict_buffer =
      (uint8_t *)heap_caps_malloc(LZ4_DICT_SIZE, MALLOC_CAP_8BIT);
  int dict_size = 0;

  if (!c_buffer || !d_buffer || !dict_buffer) {
    ESP_LOGE(TAG, "Memory allocation failed");
    goto exit;
  }

  version = (verRef == nullptr) ? ED_sysstd::ESP_std::fwVer() : verRef;

  fwScanner = new FirmwareScanner(ED_sysstd::ESP_std::fwPrjName(), version,
                                  (verRef == nullptr)
                                      ? FirmwareScanner::UPDATE_TO_LATEST
                                      : FirmwareScanner::UPDATE_TO_SPECIFIC);

  // ESP_LOGI(TAG, "Step_prescanfirmware");
  httpPath = fwStorageUrl;
  if (!scanFirmware(*fwScanner, fwStorageUrl)) {
    ESP_LOGW(TAG, "Primary scan failed, trying fallback...");
    httpPath = fwObsUrl;
    scanFirmware(*fwScanner, fwObsUrl);
  }

  if (fwScanner->targetFwFile() == nullptr) {
    ESP_LOGI(TAG, "No target firmware found");
    goto exit;
  }

  fullUrl = httpPath + std::string(fwScanner->targetFwFile());
  ESP_LOGI(TAG, "OTA: launching update with file <%s>",
           fwScanner->targetFwFile());
  // ESP_LOGI(TAG, "httpPath is %s, setting url: %s", httpPath, fullUrl.c_str());

  config = {
      .url = fullUrl.c_str(),
      // .cert_pem = (const char *)ca_crt_start,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  client = esp_http_client_init(&config);
  err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not open HTTP connection: %s", esp_err_to_name(err));
    goto exit;
  }

  esp_http_client_fetch_headers(client);
  status = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP status code: %d", status);
  if (status != 200) {
    ESP_LOGE(TAG, "Unexpected HTTP status — aborting");
    goto exit;
  }

  // ESP_LOGI(TAG, "Step_a");
  update_partition = esp_ota_get_next_update_partition(NULL);
  if ((err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle)) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    goto exit;
  }

  lz4_stream = LZ4_createStreamDecode();
  if (!lz4_stream) {
    ESP_LOGE(TAG, "Failed to create LZ4 stream decoder");
    goto exit;
  }

  ESP_LOGI(TAG, "Starting OTA read loop...");

  while (true) {
    uint32_t block_size = 0;
    bytes_read =
        esp_http_client_read(client, (char *)&block_size, sizeof(block_size));
    if (bytes_read == 0) {
      ESP_LOGI(TAG, "End of OTA data stream");
      break;
    }
    if (bytes_read != sizeof(block_size)) {
      ESP_LOGE(TAG, "Failed to read block size");
      goto exit;
    }

    if (block_size > BUFFER_SIZE) {
      ESP_LOGE(TAG, "Block too large: %u bytes", block_size);
      goto exit;
    }

    bytes_read = esp_http_client_read(client, (char *)c_buffer, block_size);
    if (bytes_read != block_size) {
      ESP_LOGE(TAG, "Incomplete block read: expected %u, got %u", block_size,
               bytes_read);
      goto exit;
    }

    // Set dictionary from rolling buffer
    LZ4_setStreamDecode(lz4_stream, (const char *)dict_buffer, dict_size);

    int decompressed_bytes =
        LZ4_decompress_safe_continue(lz4_stream, (const char *)c_buffer,
                                     (char *)d_buffer, bytes_read, BUFFER_SIZE);

    if (decompressed_bytes < 0) {
      ESP_LOGE(TAG, "LZ4 decompression failed with code %d",
               decompressed_bytes);
      goto exit;
    }

    err = esp_ota_write(ota_handle, d_buffer, decompressed_bytes);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write OTA chunk: %s", esp_err_to_name(err));
      goto exit;
    }

    ota_data_written = true;

    // Update rolling dictionary (max LZ4_DICT_SIZE)
    if (dict_size + decompressed_bytes <= LZ4_DICT_SIZE) {
      memcpy(dict_buffer + dict_size, d_buffer, decompressed_bytes);
      dict_size += decompressed_bytes;
    } else {
      int overflow = dict_size + decompressed_bytes - LZ4_DICT_SIZE;
      memmove(dict_buffer, dict_buffer + overflow, dict_size - overflow);
      memcpy(dict_buffer + (LZ4_DICT_SIZE - decompressed_bytes), d_buffer,
             decompressed_bytes);
      dict_size = LZ4_DICT_SIZE;
    }
  }

  if (!ota_data_written) {
    ESP_LOGE(TAG, "No OTA data was written — aborting");
    goto exit;
  }

  if ((err = esp_ota_end(ota_handle)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to complete OTA: %s", esp_err_to_name(err));
    goto exit;
  }

  ESP_LOGI(TAG,
           "OTA update successful. Switching new partition as boot default and rebooting...");

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
             esp_err_to_name(err));
    goto exit;
  }
  esp_restart();

exit:
  if(client)
    esp_http_client_cleanup(client);
  if (fwScanner)
    delete fwScanner;
  if (c_buffer)
    free(c_buffer);
  if (d_buffer)
    free(d_buffer);
  if (dict_buffer)
    free(dict_buffer);
  if (lz4_stream)
    LZ4_freeStreamDecode(lz4_stream);
  if (pvParameter != nullptr)
    free((void *)pvParameter);
  esp_http_client_cleanup(client);
  vTaskDelete(NULL);
}

void OTAmanager::cmd_otaValidate(ED_MQTT_dispatcher::ctrlCommand *cmd) {
  cmd_otaValidate(true);
}

void OTAmanager::cmd_otaValidate(bool otaIsValid) {

  /* The validation is performed automathically by ESP after
CONFIG_BOOTLOADER_WDT_TIME_MS=9000
so this call will not be required.
to control the transition from PENDING to VALID state manually, need to modify the sdkconfig setting
CONFIG_BOOTLOADER_WDT_TIME_MS=-1

  */
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  ESP_LOGI(TAG, "pre-OTA validation state: %s",
           (ota_state == ESP_OTA_IMG_PENDING_VERIFY) ? "PENDING_VERIFY"
                                                     : "VALID");
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
      ota_state == ESP_OTA_IMG_PENDING_VERIFY) {

    if (otaIsValid) {
      esp_ota_mark_app_valid_cancel_rollback();
      ESP_LOGI(TAG, "Firmware verified, rollback canceled");
    } else {
      ESP_LOGE(TAG, "Diagnostics failed, rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
};

void OTAmanager::cmd_launchUpdate(ED_MQTT_dispatcher::ctrlCommand *cmd) {
  cmd_launchUpdate(cmd->optParam["default"].c_str());
}
void OTAmanager::cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand *cmd) {
  const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    // ESP_LOGI(TAG, "Step_in check ota state");
    std::string response="";
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
      switch (ota_state) {
      case ESP_OTA_IMG_PENDING_VERIFY:
        response="OTA: Image is PENDING_VERIFY";
        // Run your self-tests here, then either:
        // On success:
        esp_ota_mark_app_valid_cancel_rollback();
        // On failure:
        // esp_ota_mark_app_invalid_rollback_and_reboot();
        break;
      case ESP_OTA_IMG_VALID:
        response="OTA: Image is VALID";
        break;
      case ESP_OTA_IMG_INVALID:
        response="OTA: Image is INVALID";
        break;
      default:
        response= "OTA: Image state = " +  ota_state;
        break;
      }
    } else {
      ESP_LOGE(TAG, "Failed to get OTA state");




      ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(std::stoll(cmd->optParam["_msgID"]),cmd->cmdID,
      ED_MQTT_dispatcher::MQTTdispatcher::ackType::FAIL,"");
    }
    ESP_LOGI(TAG,"Step msgid_%s",cmd->optParam["_msgID"].c_str());
    ESP_LOGI(TAG,"Step cmdid_%s",cmd->cmdID.c_str());
    ESP_LOGI(TAG,"** %s",response.c_str());
   ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(std::stoll(cmd->optParam["_msgID"]),cmd->cmdID,
   ED_MQTT_dispatcher::MQTTdispatcher::ackType::OK,response);
ESP_LOGI(TAG,"Step_endgetfwstatus");
  }

void OTAmanager::cmd_launchUpdate(const char *versionTarget) {
  // ESP_LOGI(TAG, "Step_in launchupdate");

  char *ver_copy = nullptr;
  if (versionTarget != nullptr && strlen(versionTarget) > 0)
    ver_copy = strdup(versionTarget); // this must be coming from MQTT command,
                                      // copy required to avoid out of scope
  // ESP_LOGI(TAG, "Step_in launchupdate creating task");

  xTaskCreate(&ED_OTA::OTAmanager::ota_update_task, "ota_task", 8192,
              (void *)ver_copy, 5, NULL);
}
/*
to be defined. how hook the right led GPIO?
*/

//#endregion OTAmanager

// void indicate_ota_success(gpio_num_t OTA_LED) {
//   gpio_set_direction(OTA_LED, GPIO_MODE_OUTPUT);
//   for (int i = 0; i < 5; i++) {
//     gpio_set_level(OTA_LED, 1);
//     vTaskDelay(pdMS_TO_TICKS(500));
//     gpio_set_level(OTA_LED, 0);
//     vTaskDelay(pdMS_TO_TICKS(500));
//   }
// }

} // namespace ED_OTA