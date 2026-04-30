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
#include "lz4.h"
#include <regex.h>


#define COMPRESSED_BLOCK_SIZE 4096 // maximum compressed block size from HTTP
#define DECOMPRESSED_BLOCK_SIZE                                                \
  16384 // must be >= max decompressed output (16KB)
#define CARRYOVER_SIZE 128
#define MAX_FILENAME_LEN 128

namespace ED_OTA {

/// @brief scans firmware files in an HTTP directory listing to find the best
/// candidate.
struct FirmwareScanner {
  enum UpdateType { UPDATE_TO_LATEST, UPDATE_TO_SPECIFIC };

  FirmwareScanner(const char *FwarePrj, const char *curFwareVer,
                  UpdateType mode);
  ~FirmwareScanner();

  void file_scanner_parse_chunk(const char *chunk, size_t chunk_len);
  const char *targetFwFile();

private:
  const char *prjID;
  regex_t regex;
  UpdateType updateMode;

  char buffer[COMPRESSED_BLOCK_SIZE + CARRYOVER_SIZE + 1];
  char carryover[CARRYOVER_SIZE + 1];
  char best_filename[MAX_FILENAME_LEN];
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
 * Implements HTTPS + LZ4 streaming.
 */
class OTAmanager : public ED_MQTT_dispatcher::CommandWithRegistry {
private:
  static inline const char fwStorageUrl[30] = "https://raspi00/fware/";
  static inline const char fwObsUrl[30] = "https://raspi00/fware/obs/";
  static void ota_update_task(void *pvParameter);

public:
  void cmd_otaValidate(ED_MQTT_dispatcher::ctrlCommand *cmd);
  void cmd_launchUpdate(ED_MQTT_dispatcher::ctrlCommand *cmd);
  void cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand *cmd);
  OTAmanager();

  void cmd_launchUpdate(const char *versionTarget);
  void cmd_otaValidate(bool otaIsValid);
};

} // namespace ED_OTA