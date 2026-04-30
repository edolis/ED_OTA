#include "ED_OTA.h"
#include "ED_sys.h"
#include "ED_sysInfo.h"
#include "esp_crt_bundle.h"
#include <cctype>
#include <cstring>
#include <driver/gpio.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <freertos/semphr.h>
#include <regex.h>
#include <string>

namespace ED_OTA {

static SemaphoreHandle_t ota_mutex = NULL;
static const char *TAG = "ED_OTA";
static OTAmanager *g_otaManager = nullptr;   // for static trampolines

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

void FirmwareScanner::parse_version_string(const char *ver_str, int out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    regex_t re;
    if (regcomp(&re,
                "v([[:digit:]]+)\\.([[:digit:]]+)\\.([[:digit:]]+)-([[:digit:]]+)",
                REG_EXTENDED) == 0) {
        regmatch_t matches[5];
        if (regexec(&re, ver_str, 5, matches, 0) == 0) {
            for (int i = 0; i < 4; i++) {
                int start = matches[i + 1].rm_so;
                int end = matches[i + 1].rm_eo;
                if (start != -1 && end != -1) {
                    char temp[16];
                    int len = end - start;
                    strncpy(temp, ver_str + start, len);
                    temp[len] = '\0';
                    out[i] = atoi(temp);
                }
            }
        }
        regfree(&re);
    }
}

bool FirmwareScanner::matches_prefix(const int cand[4]) {
    for (int i = 0; i < 4; i++) {
        if (prefix_locked[i] && cand[i] != best_version[i])
            return false;
    }
    return true;
}

bool FirmwareScanner::is_version_higher(int new_v[4]) {
    for (int i = 0; i < 4; i++) {
        if (new_v[i] > best_version[i])
            return true;
        if (new_v[i] < best_version[i])
            return false;
    }
    return false;
}

FirmwareScanner::FirmwareScanner(const char *FwarePrj, const char *curFwareVer,
                                 UpdateType mode)
    : prjID(FwarePrj), updateMode(mode), matchingVersionFound(false) {
    best_version[0] = best_version[1] = best_version[2] = best_version[3] = 0;
    for (int i = 0; i < 4; i++)
        prefix_locked[i] = false;

    parse_version_string(curFwareVer, best_version);

    if (mode == UPDATE_TO_SPECIFIC) {
        regex_t re;
        regmatch_t m[5];
        if (regcomp(&re,
                    "v([[:digit:]]+)(?:\\.([[:digit:]]+))?(?:\\.([[:digit:]]+))?(?:-([[:digit:]]+))?",
                    REG_EXTENDED) == 0) {
            if (regexec(&re, curFwareVer, 5, m, 0) == 0) {
                for (int i = 0; i < 4; i++) {
                    if (m[i + 1].rm_so != -1)
                        prefix_locked[i] = true;
                }
            }
            regfree(&re);
        }
    }

    char pattern[256];
    std::string escaped_prj = regex_escape(prjID);
    snprintf(pattern, sizeof(pattern),
             "href=\\\"(%s_v([[:digit:]]+)\\.([[:digit:]]+)\\.([[:digit:]]+)[^\\\"]*\\.bin(?:\\.[a-z0-9]+)?)\\\"",
             escaped_prj.c_str());
    int ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret != 0) {
        ESP_LOGE(TAG, "Regex compilation failed: %d", ret);
    }
}

FirmwareScanner::~FirmwareScanner() { regfree(&regex); }

void FirmwareScanner::file_scanner_parse_chunk(const char *chunk,
                                               size_t chunk_len) {
    size_t carry_len = strlen(carryover);
    memcpy(buffer, carryover, carry_len);
    memcpy(buffer + carry_len, chunk, chunk_len);
    buffer[carry_len + chunk_len] = '\0';

    regmatch_t matches[6];
    char *ptr = buffer;
    while (regexec(&regex, ptr, 6, matches, 0) == 0) {
        int len = matches[1].rm_eo - matches[1].rm_so;
        char filename[MAX_FILENAME_LEN];
        strncpy(filename, ptr + matches[1].rm_so, len);
        filename[len] = '\0';

        int version[4] = {0, 0, 0, 0};
        for (int i = 0; i < 3; i++) {
            int start = matches[i + 2].rm_so;
            int end = matches[i + 2].rm_eo;
            if (start != -1 && end != -1) {
                char temp[16];
                int vlen = end - start;
                strncpy(temp, ptr + start, vlen);
                temp[vlen] = '\0';
                version[i] = atoi(temp);
            }
        }
        version[3] = 0;

        ESP_LOGV(TAG, "Candidate: %s → %d.%d.%d", filename, version[0], version[1],
                 version[2]);

        bool accept = false;
        if (updateMode == UPDATE_TO_SPECIFIC) {
            if (matches_prefix(version))
                accept = is_version_higher(version);
        } else {
            accept = is_version_higher(version);
        }

        if (accept) {
            snprintf(best_filename, MAX_FILENAME_LEN, "%s", filename);
            memcpy(best_version, version, sizeof(version));
            matchingVersionFound = true;
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

// ---------- OTAmanager ----------

// Static trampolines for command callbacks
static void trampoline_FWUP(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_launchUpdate(cmd);
}
static void trampoline_FWCO(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_otaValidate(cmd);
}
static void trampoline_FWQS(ED_MQTT_dispatcher::ctrlCommand *cmd) {
    if (g_otaManager) g_otaManager->cmd_getFwStatus(cmd);
}

OTAmanager::OTAmanager() {
    if (ota_mutex == NULL) {
        ota_mutex = xSemaphoreCreateMutex();
    }
    g_otaManager = this;   // set singleton pointer

    // Register MQTT commands – use static trampolines (no lambda capture)
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
    if (ota_mutex == NULL || xSemaphoreTake(ota_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take OTA mutex");
        if (pvParameter)
            free(pvParameter);
        vTaskDelete(NULL);
        return;
    }

    // All variables with non‑trivial constructors declared at top (no goto crossing)
    const char *verRef = static_cast<const char *>(pvParameter);
    uint8_t *c_buffer = (uint8_t *)malloc(COMPRESSED_BLOCK_SIZE);
    uint8_t *d_buffer = (uint8_t *)malloc(DECOMPRESSED_BLOCK_SIZE);
    esp_http_client_config_t config = {};
    esp_http_client_handle_t client = nullptr;
    esp_ota_handle_t ota_handle = 0;
    LZ4_streamDecode_t *lz4_stream = nullptr;
    std::string fullUrl;   // non‑trivial, but declared before any goto
    esp_err_t err = ESP_OK;
    bool ota_data_written = false;
    int status = 0;
    const char *version = "";
    const char *httpPath = "";
    const esp_partition_t *update_partition = nullptr;
    FirmwareScanner *fwScanner = nullptr;
    size_t total_compressed_read = 0;

    const int LZ4_DICT_SIZE = 16 * 1024;
    uint8_t *dict_buffer =
        (uint8_t *)heap_caps_malloc(LZ4_DICT_SIZE, MALLOC_CAP_8BIT);
    int dict_size = 0;

    bool error = false;

    do {   // single‑iteration loop to allow `break` instead of `goto`
        if (!c_buffer || !d_buffer || !dict_buffer) {
            ESP_LOGE(TAG, "Memory allocation failed");
            error = true;
            break;
        }

        version = (verRef == nullptr) ? ED_SYS::ESP_std::Firmware::version() : verRef;

        fwScanner = new FirmwareScanner(ED_SYS::ESP_std::Firmware::prjName(), version,
                                        (verRef == nullptr) ? FirmwareScanner::UPDATE_TO_LATEST
                                                            : FirmwareScanner::UPDATE_TO_SPECIFIC);

        httpPath = fwStorageUrl;
        if (!scanFirmware(*fwScanner, fwStorageUrl)) {
            ESP_LOGW(TAG, "Primary scan failed, trying fallback...");
            delete fwScanner;
            fwScanner = new FirmwareScanner(
                ED_SYS::ESP_std::Firmware::prjName(), version,
                (verRef == nullptr) ? FirmwareScanner::UPDATE_TO_LATEST
                                    : FirmwareScanner::UPDATE_TO_SPECIFIC);
            httpPath = fwObsUrl;
            if (!scanFirmware(*fwScanner, fwObsUrl)) {
                ESP_LOGE(TAG, "Fallback scan also failed");
                error = true;
                break;
            }
        }

        if (fwScanner->targetFwFile() == nullptr) {
            ESP_LOGI(TAG, "No target firmware found");
            error = true;
            break;
        }

        fullUrl = httpPath + std::string(fwScanner->targetFwFile());
        ESP_LOGI(TAG, "OTA: launching update with file <%s>",
                 fwScanner->targetFwFile());

        config = {
            .url = fullUrl.c_str(),
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        client = esp_http_client_init(&config);
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not open HTTP connection: %s", esp_err_to_name(err));
            error = true;
            break;
        }

        int content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Failed to fetch headers, error: %d", content_length);
            error = true;
            break;
        }

        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status code: %d", status);
        if (status != 200) {
            ESP_LOGE(TAG, "Unexpected HTTP status — aborting");
            error = true;
            break;
        }

        update_partition = esp_ota_get_next_update_partition(NULL);
        if ((err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle)) !=
            ESP_OK) {
            ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
            error = true;
            break;
        }

        lz4_stream = LZ4_createStreamDecode();
        if (!lz4_stream) {
            ESP_LOGE(TAG, "Failed to create LZ4 stream decoder");
            error = true;
            break;
        }

        ESP_LOGI(TAG, "Starting OTA read loop...");
        size_t total_written = 0;

        while (true) {
            esp_task_wdt_reset();

            uint32_t block_size = 0;
            int bytes_read =
                esp_http_client_read(client, (char *)&block_size, sizeof(block_size));
            if (bytes_read == 0) {
                ESP_LOGI(TAG, "End of OTA data stream");
                break;
            }
            if (bytes_read != sizeof(block_size)) {
                ESP_LOGE(TAG, "Failed to read block size (got %d)", bytes_read);
                error = true;
                break;
            }

            if (block_size > COMPRESSED_BLOCK_SIZE) {
                ESP_LOGE(TAG, "Compressed block too large: %u bytes", block_size);
                error = true;
                break;
            }

            bytes_read = esp_http_client_read(client, (char *)c_buffer, block_size);
            if (bytes_read < 0) {
                ESP_LOGE(TAG, "HTTP read error: %d", bytes_read);
                error = true;
                break;
            }
            if ((size_t)bytes_read != block_size) {
                ESP_LOGE(TAG, "Incomplete block read: expected %u, got %d", block_size,
                         bytes_read);
                error = true;
                break;
            }
            total_compressed_read += bytes_read;

            LZ4_setStreamDecode(lz4_stream, (const char *)dict_buffer, dict_size);
            int decompressed_bytes = LZ4_decompress_safe_continue(
                lz4_stream, (const char *)c_buffer, (char *)d_buffer, bytes_read,
                DECOMPRESSED_BLOCK_SIZE);
            if (decompressed_bytes < 0) {
                ESP_LOGE(TAG, "LZ4 decompression failed with code %d",
                         decompressed_bytes);
                error = true;
                break;
            }

            err = esp_ota_write(ota_handle, d_buffer, decompressed_bytes);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA chunk: %s", esp_err_to_name(err));
                error = true;
                break;
            }

            total_written += decompressed_bytes;
            ota_data_written = true;

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
        } // end while

        if (error) break;

        if (!ota_data_written) {
            ESP_LOGE(TAG, "No OTA data was written — aborting");
            error = true;
            break;
        }

        // content_length is the compressed size from HTTP header
        if (content_length > 0 && total_compressed_read != (size_t)content_length) {
            ESP_LOGE(TAG, "OTA size mismatch: expected %d compressed bytes, got %zu",
                     content_length, total_compressed_read);
            error = true;
            break;
        }

        if ((err = esp_ota_end(ota_handle)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to complete OTA: %s", esp_err_to_name(err));
            error = true;
            break;
        }

        ESP_LOGI(TAG, "OTA update successful. Switching partition and rebooting...");
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                     esp_err_to_name(err));
            error = true;
            break;
        }
        esp_restart();

    } while (0);   // end of "do { } while(0)" block

    // Cleanup (same as before, but no goto)
    if (ota_mutex)
        xSemaphoreGive(ota_mutex);
    if (client)
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
    vTaskDelete(NULL);
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
    cmd_launchUpdate(target);
}

void OTAmanager::cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand *cmd) {
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
        const char *msgid_str = cmd->getParam("_msgID");
        if (msgid_str) {
            ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(
                std::stoll(msgid_str), cmd->cmdID,
                ED_MQTT_dispatcher::MQTTdispatcher::ackType::FAIL,
                "Failed to get OTA state");
        }
        return;
    }
    const char *msgid_str = cmd->getParam("_msgID");
    if (msgid_str) {
        ED_MQTT_dispatcher::MQTTdispatcher::ackCommand(
            std::stoll(msgid_str), cmd->cmdID,
            ED_MQTT_dispatcher::MQTTdispatcher::ackType::OK,
            response.c_str());   // send status instead of original command
    }
    ESP_LOGI(TAG, "OTA status: %s", response.c_str());
}

void OTAmanager::cmd_launchUpdate(const char *versionTarget) {
    char *ver_copy = nullptr;
    if (versionTarget != nullptr && strlen(versionTarget) > 0) {
        ver_copy = strdup(versionTarget);
        if (ver_copy == NULL) {
            ESP_LOGE(TAG, "strdup failed");
            return;
        }
    }
    xTaskCreate(&ED_OTA::OTAmanager::ota_update_task, "ota_task", 16384,
                (void *)ver_copy, 5, NULL);
}

} // namespace ED_OTA