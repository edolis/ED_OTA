// #region StdManifest
/**
 * @file ED_OTA.h
 * @brief OTA functionalities for ESP environment.
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version 0.2
 * @date 2025-09-07
 */
// #endregion

#pragma once

#include "ED_MQTT_dispatcher.h"
#include "lz4/lz4.h"                // raw LZ4 API (streaming)
#include <regex.h>

#define COMPRESSED_BLOCK_SIZE   4096   // maximum compressed block from HTTP
#define DECOMPRESSED_BLOCK_SIZE 16384  // must be >= max decompressed output (16KB)
#define CARRYOVER_SIZE 128
#define MAX_FILENAME_LEN 128

namespace ED_OTA {

/// @brief scans firmware files in an HTTP directory listing to find the best candidate.
struct FirmwareScanner {
  enum UpdateType { UPDATE_TO_LATEST, UPDATE_TO_SPECIFIC };

  FirmwareScanner(const char *FwarePrj, const char *curFwareVer,
                  UpdateType mode);
  ~FirmwareScanner();

  void file_scanner_parse_chunk(const char *chunk, size_t chunk_len);
  const char *targetFwFile();

  /// @return full version string of the best candidate (e.g., "v1.2.3-5")
  const char *getBestVersionStr() const { return best_version_str; }

private:
  const char *prjID;
  regex_t regex;
  UpdateType updateMode;

  char buffer[COMPRESSED_BLOCK_SIZE + CARRYOVER_SIZE + 1];
  char carryover[CARRYOVER_SIZE + 1];
  char best_filename[MAX_FILENAME_LEN];
  char best_version_str[MAX_FILENAME_LEN];   // stores full version like "v1.2.3-5"
  int best_version[4]; // major, minor, patch, build
  bool prefix_locked[4];
  bool matchingVersionFound;

  bool is_version_higher(int new_v[4]);
  void parse_version_string(const char *ver_str, int out[4]);
  bool matches_prefix(const int cand[4]);

  FirmwareScanner() = delete;
};

/**
 * @brief OTA updater controlled via MQTT commands.
 * Implements HTTPS + LZ4 streaming (raw block‑prefixed format).
 */
class OTAmanager : public ED_MQTT_dispatcher::CommandWithRegistry {
private:
  static inline const char fwStorageUrl[30] = "https://raspi00/fware/";
  static inline const char fwObsUrl[30] = "https://raspi00/fware/obs/";

  struct OtaTaskParams {
    const char *versionTarget;   // may be nullptr (owned by this struct, freed by task)
    int64_t     msgID;           // original MQTT message ID for ack
  };

  static void ota_update_task(void *pvParameter);
  static void sendOtaAck(int64_t msgID, const char *selectedFile);

public:
  void cmd_otaValidate(ED_MQTT_dispatcher::ctrlCommand *cmd);
  void cmd_launchUpdate(ED_MQTT_dispatcher::ctrlCommand *cmd);
  void cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand *cmd);
  OTAmanager();

  void cmd_launchUpdate(const char *versionTarget);
  void cmd_otaValidate(bool otaIsValid);
};

} // namespace ED_OTA