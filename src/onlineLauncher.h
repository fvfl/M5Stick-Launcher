#ifndef __ONLINELAUNCHER_H
#define __ONLINELAUNCHER_H

#include "partition_install_layout.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>

bool installExtFirmware(String url);

void installFirmware(
    String fid, String file, uint32_t app_size, uint32_t app_offset, bool nb,
    std::vector<LauncherInstallDataPartition> &dataPartitions, String installedName = ""
);
void installFirmwareFromManifest(String fid, String version, String installedName = "");

bool connectWifi();
bool ensureWifiConnected(String ssid = "", int encryptation = 0, bool isAP = false);

void ota_function();

void downloadFirmware(String fid, String file, String fileName, String folder = "/downloads/", String version = "", bool autoAdvance = false);
void saveDownloadedFirmware(const String &folder, const String &fid, const String &version);
bool checkForUpdates();

bool wifiConnect(String ssid, int encryptation, bool isAP = false);

bool GetJsonFromLauncherHub(
    uint8_t page = 1, String order = "downloads", bool star = false, String query = ""
);

JsonDocument getVersionInfo(String fid);

bool installFAT_OTA(String file, uint32_t offset, uint32_t size, const char *label);

#endif
