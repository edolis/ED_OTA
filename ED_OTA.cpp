#include "ED_OTA.h"
#include "ED_sys.h"
#include "ED_sysInfo.h"
#include "esp_crt_bundle.h"
#include <cctype>
#include <cstring>
#include <cstdlib>   // strtoll
#include <cerrno>    // errno
#include <driver/gpio.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/semphr.h>
#include <regex.h>
#include <string>
#include "lz4/lz4.h"          // raw LZ4 (streaming API only)

namespace ED_OTA {

static SemaphoreHandle_t ota_mutex = NULL;
static const char *TAG = "ED_OTA";
static OTAmanager *g_otaManager = nullptr;

static std::string regex_escape(const std::string &s) {
    static const char *meta = ".^$*+?()[{\\|";
    std::string escaped;
    for (char c : s) {
        if (strchr(meta, c))
            escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

bool scanFirmware(FirmwareScanner &scanner, const std::string &url) {
    esp_http_client_config_t config = {
        .url = url.c_str(),
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "failed to open client to %s error: %s", config.url,
                 esp_err_to_name(err));
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers, error: %d", content_length);
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t c_buffer[COMPRESSED_BLOCK_SIZE];
    int bytes_read;
    do {
        bytes_read =
            esp_http_client_read(client, (char *)c_buffer, sizeof(c_buffer));
        if (bytes_read > 0) {
            c_buffer[bytes_read] = '\0';
            scanner.file_scanner_parse_chunk((char *)c_buffer, bytes_read);
        } else if (bytes_read < 0) {
            ESP_LOGE(TAG, "HTTP read error: %d", bytes_read);
            esp_http_client_cleanup(client);
            return false;
        }
    } while (bytes_read > 0);

    esp_http_client_cleanup(client);
    return scanner.targetFwFile() != nullptr;
}

// ---------- FirmwareScanner ----------

FirmwareScanner::FirmwareScanner(const char *curFwarePrj, const char *refFwVer,
                                 UpdateType mode)
    : prjID(curFwarePrj), buffer(""), carryover(""), best_filename(""),
      matchingVersionFound(false) {

        best_version_str[0] = '\0';

    for (int i = 0; i < 4; i++) {
        best_version[i] = 0;
        prefix_locked[i] = false;
    }

    if (refFwVer != nullptr && refFwVer[0] != '\0') {
        int major = 0, minor = 0, patch = 0, build = 0;
        int fields = sscanf(refFwVer, "v%d.%d.%d-%d", &major, &minor, &patch, &build);
        if (fields >= 1) {
            best_version[0] = major;
            prefix_locked[0] = true;
        }
        if (fields >= 2) {
            best_version[1] = minor;
            prefix_locked[1] = true;
        }
        if (fields >= 3) {
            best_version[2] = patch;
            prefix_locked[2] = true;
        }
        if (fields >= 4) {
            best_version[3] = build;
            prefix_locked[3] = true;
        }
    }

    // Reset unlocked components to -1 so any real candidate (>=0) is higher
    for (int i = 0; i < 4; i++) {
        if (!prefix_locked[i]) {
            best_version[i] = -1;
        }
    }

    char pattern[256];
    std::string escaped_prj = regex_escape(prjID);
    snprintf(pattern, sizeof(pattern),
             "href=\\\"(%s_v([[:digit:]]+)\\.([[:digit:]]+)\\.([[:digit:]]+)-([[:digit:]]+)[^\\\"]*\\.bin(\\.[a-z0-9]+)?)\\\"",
             escaped_prj.c_str());
    ESP_LOGI(TAG, "Scanner prjID: %s, regex: %s", prjID, pattern);
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        ESP_LOGE(TAG, "Regex compilation failed for pattern: %s", pattern);
    }
}

FirmwareScanner::~FirmwareScanner() {
    regfree(&regex);
}

void FirmwareScanner::file_scanner_parse_chunk(const char *chunk, size_t chunk_len) {
    size_t carry_len = strlen(carryover);
    memcpy(buffer, carryover, carry_len);
    memcpy(buffer + carry_len, chunk, chunk_len);
    buffer[carry_len + chunk_len] = '\0';

    regmatch_t matches[7];   // 7 for full match + 6 sub-expressions
    char *ptr = buffer;
    while (regexec(&regex, ptr, 7, matches, 0) == 0) {
        int len = matches[1].rm_eo - matches[1].rm_so;
        char filename[MAX_FILENAME_LEN];
        strncpy(filename, ptr + matches[1].rm_so, len);
        filename[len] = '\0';

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

        bool higher = is_version_higher(version);
        ESP_LOGI(TAG, "Candidate: %s (v%d.%d.%d-%d) -> %s",
                 filename, version[0], version[1], version[2], version[3],
                 higher ? "ACCEPTED (new best)" : "rejected");

        if (higher) {
            strncpy(best_filename, filename, MAX_FILENAME_LEN);
            memcpy(best_version, version, sizeof(version));
            matchingVersionFound = true;

            // Extract the full version string from the filename (e.g., "v1.2.3-5")
            const char* ver_start = strchr(filename, '_');
            if (ver_start) {
                ver_start++; // skip the underscore
                const char* ver_end = strstr(ver_start, ".bin");
                if (!ver_end) ver_end = ver_start + strlen(ver_start);
                size_t vlen = ver_end - ver_start;
                if (vlen > 0 && vlen < MAX_FILENAME_LEN) {
                    strncpy(best_version_str, ver_start, vlen);
                    best_version_str[vlen] = '\0';
                } else {
                    best_version_str[0] = '\0';
                }
            } else {
                best_version_str[0] = '\0';
            }
        }

        ptr += matches[0].rm_eo;
    }

    size_t total_len = carry_len + chunk_len;
    if (total_len >= CARRYOVER_SIZE) {
        memcpy(carryover, buffer + total_len - CARRYOVER_SIZE, CARRYOVER_SIZE);
        carryover[CARRYOVER_SIZE] = '\0';
    } else {
        strcpy(carryover, buffer);
    }
}

const char *FirmwareScanner::targetFwFile() {
    return matchingVersionFound ? best_filename : nullptr;
}

bool FirmwareScanner::is_version_higher(int new_v[4]) {
    for (int i = 0; i < 4; i++) {
        if (!prefix_locked[i]) {
            if (new_v[i] > best_version[i])
                return true;
            if (new_v[i] < best_version[i])
                return false;
        } else {
            if (new_v[i] != best_version[i])
                return false;
        }
    }
    return false;
}

// ---------- OTAmanager ----------

static void trampoline_FWUP(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_launchUpdate(cmd);
}
static void trampoline_FWCO(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_otaValidate(cmd);
}
static void trampoline_FWQS(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_getFwStatus(cmd);
}

OTAmanager::OTAmanager()
    : CommandWithRegistry("OTA", "Over-the-Air update commands")
{
    if (ota_mutex == NULL) {
        ota_mutex = xSemaphoreCreateMutex();
    }
    g_otaManager = this;

    ED_MQTT_dispatcher::ctrlCommand cmd(
        "FWUP", "Update firmware via OTA",
        ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL, {{"default", ""}});
    cmd.funcPointer = trampoline_FWUP;
    registerCommand(cmd);

    ED_MQTT_dispatcher::ctrlCommand cmd1(
        "FWCO", "Confirms OTA partition as valid",
        ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL, {});
    cmd1.funcPointer = trampoline_FWCO;
    registerCommand(cmd1);

    ED_MQTT_dispatcher::ctrlCommand cmd2(
        "FWQS", "Query Status of running OTA",
        ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL, {});
    cmd2.funcPointer = trampoline_FWQS;
    registerCommand(cmd2);
}

void OTAmanager::ota_update_task(void *pvParameter) {
    OtaTaskParams *params = static_cast<OtaTaskParams *>(pvParameter);
    const char *verRef = params->versionTarget;
    int64_t msgID = params->msgID;
    delete params;   // params itself is freed, verRef ownership transferred

    uint8_t *c_buffer = nullptr, *d_buffer = nullptr;
    esp_http_client_handle_t client = nullptr;
    esp_ota_handle_t ota_handle = 0;
    LZ4_streamDecode_t *lz4_stream = nullptr;
    FirmwareScanner *fwScanner = nullptr;
    bool ota_data_written = false;
    const char *httpPath = fwStorageUrl;
    const esp_partition_t *update_partition = nullptr;
    esp_err_t err = ESP_OK;
    bool decomp_error = false;

    const int LZ4_DICT_SIZE = 16 * 1024;
    uint8_t *dict_buffer = (uint8_t *)heap_caps_malloc(LZ4_DICT_SIZE, MALLOC_CAP_8BIT);
    int dict_size = 0;

    do {
        c_buffer = (uint8_t *)malloc(COMPRESSED_BLOCK_SIZE);
        d_buffer = (uint8_t *)malloc(DECOMPRESSED_BLOCK_SIZE);
        if (!c_buffer || !d_buffer || !dict_buffer) {
            ESP_LOGE(TAG, "Memory allocation failed");
            break;
        }

        const char *version = (verRef != nullptr)
                                  ? verRef
                                  : ED_SYS::ESP_std::Firmware::version();
        fwScanner = new FirmwareScanner(
            ED_SYS::ESP_std::Firmware::prjName(), version,
            (verRef == nullptr) ? FirmwareScanner::UPDATE_TO_LATEST
                                : FirmwareScanner::UPDATE_TO_SPECIFIC);

        if (!scanFirmware(*fwScanner, httpPath)) {
            ESP_LOGW(TAG, "Trying fallback...");
            httpPath = fwObsUrl;
            if (!scanFirmware(*fwScanner, httpPath)) {
                ESP_LOGE(TAG, "No firmware found");
                break;
            }
        }
        if (fwScanner->targetFwFile() == nullptr) {
            ESP_LOGI(TAG, "No target firmware file");
            break;
        }

        // --- Check if candidate version equals currently running version ---
        const char* currentVer = ED_SYS::ESP_std::Firmware::version();
        const char* candidateVer = fwScanner->getBestVersionStr();
        if (candidateVer && candidateVer[0] != '\0' && strcmp(currentVer, candidateVer) == 0) {
            ESP_LOGI(TAG, "Already running version %s – no OTA needed", currentVer);
            // Send ack indicating no update required
            const char *deviceID = ED_SYS::ESP_std::Device::mqttName();
            char ackPayload[256];
            snprintf(ackPayload, sizeof(ackPayload),
                     "%s already running version %s, no update required",
                     deviceID, currentVer);
            ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(
                msgID, "FWUP",
                ED_MQTT_dispatcher::MQTTdispatcher::ackType::OK,
                ackPayload);
            break;   // skip OTA
        }

        ESP_LOGI(TAG, "Selected: %s (version %s)", fwScanner->targetFwFile(), candidateVer);

        // ── OTA ack (before download) ────────────────────────
        sendOtaAck(msgID, fwScanner->targetFwFile());

        std::string fullUrl = httpPath + std::string(fwScanner->targetFwFile());
        ESP_LOGI(TAG, "Download URL: %s", fullUrl.c_str());

        esp_http_client_config_t config = {};
        config.url = fullUrl.c_str();
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client = esp_http_client_init(&config);
        if (!client) break;

        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            break;
        }
        if (esp_http_client_fetch_headers(client) < 0 ||
            esp_http_client_get_status_code(client) != 200) {
            ESP_LOGE(TAG, "HTTP request failed");
            break;
        }

        update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition) {
            ESP_LOGE(TAG, "No OTA partition");
            break;
        }
        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
            break;
        }

        lz4_stream = LZ4_createStreamDecode();
        if (!lz4_stream) {
            ESP_LOGE(TAG, "LZ4 stream creation failed");
            break;
        }

        ESP_LOGI(TAG, "Starting OTA read loop (raw LZ4 block‑prefixed) ...");
        int chunks = 0;
        while (true) {
            uint32_t block_size = 0;
            int bytes_read = esp_http_client_read(client, (char *)&block_size, sizeof(block_size));
            if (bytes_read == 0) {
                ESP_LOGI(TAG, "End of stream");
                break;
            }
            if (bytes_read != sizeof(block_size)) {
                ESP_LOGE(TAG, "Failed to read block size (got %d)", bytes_read);
                decomp_error = true;
                break;
            }

            if (block_size > COMPRESSED_BLOCK_SIZE) {
                ESP_LOGE(TAG, "Block too large: %u", block_size);
                decomp_error = true;
                break;
            }

            bytes_read = esp_http_client_read(client, (char *)c_buffer, block_size);
            if (bytes_read != (int)block_size) {
                ESP_LOGE(TAG, "Incomplete block read: expected %u, got %d", block_size, bytes_read);
                decomp_error = true;
                break;
            }

            LZ4_setStreamDecode(lz4_stream, (const char *)dict_buffer, dict_size);

            int decompressed = LZ4_decompress_safe_continue(
                lz4_stream, (const char *)c_buffer, (char *)d_buffer,
                bytes_read, DECOMPRESSED_BLOCK_SIZE);
            if (decompressed < 0) {
                ESP_LOGE(TAG, "LZ4 decompression error: %d", decompressed);
                decomp_error = true;
                break;
            }

            err = esp_ota_write(ota_handle, d_buffer, decompressed);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
                decomp_error = true;
                break;
            }
            ota_data_written = true;
            chunks++;

            if (dict_size + decompressed <= LZ4_DICT_SIZE) {
                memcpy(dict_buffer + dict_size, d_buffer, decompressed);
                dict_size += decompressed;
            } else {
                int overflow = dict_size + decompressed - LZ4_DICT_SIZE;
                memmove(dict_buffer, dict_buffer + overflow, dict_size - overflow);
                memcpy(dict_buffer + (LZ4_DICT_SIZE - decompressed), d_buffer, decompressed);
                dict_size = LZ4_DICT_SIZE;
            }
        }

        if (decomp_error) break;
        ESP_LOGI(TAG, "Download complete, chunks: %d", chunks);

        if (!ota_data_written) {
            ESP_LOGE(TAG, "No data written");
            break;
        }

        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
            break;
        }

        ESP_LOGI(TAG, "OTA successful, rebooting...");
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Boot partition set failed: %s", esp_err_to_name(err));
            break;
        }
        esp_restart();

    } while (false);

    // Cleanup
    if (lz4_stream) LZ4_freeStreamDecode(lz4_stream);
    if (dict_buffer) free(dict_buffer);
    if (c_buffer) free(c_buffer);
    if (d_buffer) free(d_buffer);
    if (fwScanner) delete fwScanner;
    if (client) esp_http_client_cleanup(client);
    if (verRef) free((void *)verRef);
    vTaskDelete(NULL);
}


void OTAmanager::sendOtaAck(int64_t msgID, const char *selectedFile) {
    const char *deviceID      = ED_SYS::ESP_std::Device::mqttName();
    const char *runningVer    = ED_SYS::ESP_std::Firmware::version();

    char ackPayload[256];
    if (msgID > 0) {
        // Epoch provided by the server – include it for correlation
        snprintf(ackPayload, sizeof(ackPayload),
                 "%s running %s, starting OTA for %s (epoch=%lld)",
                 deviceID, runningVer, selectedFile, (long long)msgID);
    } else {
        snprintf(ackPayload, sizeof(ackPayload),
                 "%s running %s, starting OTA for %s",
                 deviceID, runningVer, selectedFile);
    }

    ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(
        msgID, "FWUP",
        ED_MQTT_dispatcher::MQTTdispatcher::ackType::OK,
        ackPayload);
}

void OTAmanager::cmd_otaValidate(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    cmd_otaValidate(true);
}

void OTAmanager::cmd_otaValidate(bool otaIsValid) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "pre-OTA validation state: PENDING_VERIFY");
        if (otaIsValid) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware verified, rollback canceled");
        } else {
            ESP_LOGE(TAG, "Diagnostics failed, rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    } else {
        ESP_LOGI(TAG, "pre-OTA validation state: VALID (or not an OTA partition)");
    }
}

void OTAmanager::cmd_launchUpdate(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    const char *target = cmd->getParam("_default");
    if (target && strlen(target) > 0) {
        ESP_LOGI(TAG, "Launching OTA update to version: %s", target);
    } else {
        ESP_LOGI(TAG, "Launching OTA update to latest");
        target = nullptr;
    }

    // Extract msgID for ack
    const char *msgid_str = cmd->getParam("_msgID");
    int64_t msgID = 0;
    if (msgid_str && msgid_str[0] != '\0') {
        char *endptr = nullptr;
        msgID = strtoll(msgid_str, &endptr, 10);
        if (endptr == msgid_str || *endptr != '\0') msgID = 0;
    }

    char *ver_copy = nullptr;
    if (target && strlen(target) > 0)
        ver_copy = strdup(target);

    OtaTaskParams *params = new OtaTaskParams;
    params->versionTarget = ver_copy;
    params->msgID = msgID;

    BaseType_t rc = xTaskCreate(&ED_OTA::OTAmanager::ota_update_task,
                                "ota_task", 8192,
                                (void *)params, 5, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(ver_copy);
        delete params;
    } else {
        ESP_LOGI(TAG, "OTA task created");
    }
}

void OTAmanager::cmd_launchUpdate(const char *versionTarget) {
    // This overload is retained for backwards compatibility.
    // It does not carry an msgID, so no ack will be sent.
    char *ver_copy = nullptr;
    if (versionTarget && strlen(versionTarget) > 0)
        ver_copy = strdup(versionTarget);

    OtaTaskParams *params = new OtaTaskParams;
    params->versionTarget = ver_copy;
    params->msgID = 0;

    BaseType_t rc = xTaskCreate(&ED_OTA::OTAmanager::ota_update_task,
                                "ota_task", 8192,
                                (void *)params, 5, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(ver_copy);
        delete params;
    } else {
        ESP_LOGI(TAG, "OTA task created");
    }
}

void OTAmanager::cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    ESP_LOGI(TAG, "cmd_getFwStatus called");

    const char *msgid_str = cmd->getParam("_msgID");
    ESP_LOGI(TAG, "FWQS: _msgID = '%s'", msgid_str ? msgid_str : "NULL");

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    std::string response = "";

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        switch (ota_state) {
            case ESP_OTA_IMG_PENDING_VERIFY: response = "OTA: Image is PENDING_VERIFY"; break;
            case ESP_OTA_IMG_VALID:          response = "OTA: Image is VALID"; break;
            case ESP_OTA_IMG_INVALID:        response = "OTA: Image is INVALID"; break;
            default:                         response = "OTA: Image state = " + std::to_string(static_cast<int>(ota_state)); break;
        }
    } else {
        ESP_LOGE(TAG, "Failed to get OTA state");
        response = "OTA: Failed to read state";
    }

    ESP_LOGI(TAG, "FWQS response: %s", response.c_str());

    if (msgid_str && strlen(msgid_str) > 0) {
        char *endptr = nullptr;
        errno = 0;
        long long msg_id = strtoll(msgid_str, &endptr, 10);
        if (endptr != msgid_str && *endptr == '\0' && errno == 0) {
            ESP_LOGI(TAG, "Calling ackCommand with msg_id=%lld", msg_id);
            ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(
                msg_id, cmd->cmdID,
                ED_MQTT_dispatcher::MQTTdispatcher::ackType::OK,
                response.c_str());
            ESP_LOGI(TAG, "ackCommand called");
        } else {
            ESP_LOGE(TAG, "Invalid _msgID parameter: %s", msgid_str);
        }
    } else {
        ESP_LOGI(TAG, "No _msgID, skipping ack");
    }
}

} // namespace ED_OTA