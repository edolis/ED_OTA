// #region StdManifest
/**
 * @file ED_OTA.h
 * @brief OTA functionalities for ESP environment.
 *
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version 0.1
 * @date 2025-09-07
 */
// #endregion

#pragma once

#include "lz4.h"
#include "ED_MQTT_dispatcher.h"

#define BUFFER_SIZE 4096
#define CARRYOVER_SIZE 128
#define MAX_FILENAME_LEN 128

namespace ED_OTA {

/// @brief scans the firmware files available in the specified intranet folder
/// to check if a newer version is available ( using the current or a given reference)
// no static components as it is used once.
struct FirmwareScanner {

    enum UpdateType{
        UPDATE_TO_LATEST,
        UPDATE_TO_SPECIFIC
    };
  const char *prjID;

  /// @brief initializes a firmware scanner class instance, which scanse the
  /// buffer of a http directory to detect if a  newer firmware file is
  /// available for the current project
  /// @param FwarePrj  the standardized identifier of the current
  /// project/project item (e.g. P025A)
  /// @param curFwareVer the current firmware version
  FirmwareScanner(const char *FwarePrj, const char *curFwareVer,
                                               UpdateType mode);
  //   static void file_scanner_init(FirmwareScanner *scanner);
  void file_scanner_parse_chunk(const char *chunk, size_t chunk_len);
  /**
   * @brief returns the pointer to a buffered name of the newer firmware,
   * nullptr is no newer firmware available file
   *
   * @return char*
   */
  const char *targetFwFile();
  // void updateVersionArray(int* vArray);

private:
  char buffer[BUFFER_SIZE + CARRYOVER_SIZE + 1]; // +1 for null terminator
  char carryover[CARRYOVER_SIZE + 1];
  char best_filename[MAX_FILENAME_LEN];
  int best_version[4]; // major, minor, patch, build
bool indexLock[4];
  FirmwareScanner() = delete;
  bool is_version_higher(int new_v[4], int best_v[4]);
  bool matchingVersionFound;
};

/**
 * @brief OTA updater class which can be controlled via MQTT commands
 * thanks to the command registry.
 * Implements secure communications (SSH/TSL)
 *
 */
class OTAmanager: public ED_MQTT_dispatcher::CommandWithRegistry {
private:
int cur_version[4];
static inline const char fwStorageUrl[30] = "https://raspi00/fware/";
static inline const char fwObsUrl[30] = "https://raspi00/fware/obs/";
static  void ota_update_task(void *pvParameter);

void cmd_otaValidate(ED_MQTT_dispatcher::ctrlCommand* cmd);
void cmd_launchUpdate(ED_MQTT_dispatcher::ctrlCommand* cmd);
void cmd_getFwStatus(ED_MQTT_dispatcher::ctrlCommand* cmd);
//virtual void grabCommand(const std::string commandID, const std::string commandData) override;
public:
OTAmanager();
/**
 * @brief launches an update, to the latest  available version or
 * to the latest version available having the
 * specified version index
 *
 * @param versionTarget nullptr for update to latest, a std version string (like "v1" or "v1.1" or ..")
 * to restrict the scope and perform downgrades
 */
void cmd_launchUpdate(const char* versionTarget);
// flags the current OTA partition as valid. Needs to be called once the OTA is considered suitable for production
// otherwise at next boot the previous OTA partition will be restored
// NOTE validation will be performed by ESP if th Kparameter CONFIG_BOOTLOADER_WDT_TIME_MS<>-1
void cmd_otaValidate(bool otaIsValid);
};



} // namespace ED_OTA