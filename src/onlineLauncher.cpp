#include "onlineLauncher.h"
#include "app_registry.h"
#include "backup_manager.h"
#include "display.h"
#include "idf/idf_http_client.h"
#include "idf/idf_update.h"
#include "idf/idf_wifi.h"
#include "idf/launcher_platform.h"
#include "install_shared.h"
#include "littlefs_patch.h"
#include "mykeyboard.h"
#include "partition_install_layout.h"
#include "partition_table_model.h"
#include "powerSave.h"
#include "sd_functions.h"
#include "settings.h"
#include <esp_ota_ops.h>
#include <globals.h>

#define M5_SERVER_PATH "https://m5burner-cdn.m5stack.com/firmware/"

constexpr int kWifiConnectAttempts = 20;

/***************************************************************************************
** Function name: wifiConnect
** Description:   Connects to wifiNetwork
***************************************************************************************/
bool wifiConnect(String ssid, int encryptation, bool isAP) {
    if (!isAP) {
        bool found = false;
        bool wrongPass = false;
        getConfigs();

        String knownPwd;
        if (getWifiCredential(ssid, knownPwd)) {
            pwd = knownPwd;
            found = true;
            launcherConsolePrintf("Found SSID: %s\n", ssid.c_str());
        }
        launcherConsolePrintf("sdcardMounted: %d\n", sdcardMounted);

    Retry:
        if (!found || wrongPass) {
            if (encryptation > 0) {
                pwd = keyboard(pwd, 63, "Network Password:");
                if (pwd == String(KEY_ESCAPE)) {
                    returnToMenu = true;
                    launcherDelayMs(0);
                    return false;
                }
            }

            if (!found) {
                if (setWifiCredential(ssid, pwd)) {
                    found = true;
                    launcherConsolePrintf("wifiConnect: ssid->%s, pwd->%s\n", ssid.c_str(), pwd.c_str());
                    saveConfigs();
                } else {
                    launcherConsolePrintln("wifiConnect: failed to store new WiFi entry");
                }
            } else if (wrongPass) {
                if (setWifiCredential(ssid, pwd)) {
                    launcherConsolePrintf("Mudou pwd de SSID: %s\n", ssid.c_str());
                    saveConfigs();
                }
            }
        }

        resetTftDisplay(10, 10, FGCOLOR, FP);
        tft->fillScreen(BGCOLOR);
        tftprint("Connecting to: " + ssid + ".", 10);
        tft->drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, FGCOLOR);

        int count = 0;
        LauncherWifiConnectState connectState = LauncherWifiConnectState::Pending;
        while (connectState != LauncherWifiConnectState::Connected) {
            connectState = launcherWifiConnectStatus(ssid.c_str(), pwd.c_str(), 500);
            if (connectState == LauncherWifiConnectState::Connected) break;
            if (connectState == LauncherWifiConnectState::WrongPassword) {
                displayRedStripe("Wrong Password");
                launcherDelayMs(1200);
                wrongPass = true;
                goto Retry;
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
            tftprint(".", 10);
            count++;
            if (connectState == LauncherWifiConnectState::Failed || count > kWifiConnectAttempts) {
                options = {
                    {"Retry",     [&]() { yield(); }            },
                    {"Main Menu", [&]() { returnToMenu = true; }},
                };
                loopOptions(options);
                if (!returnToMenu) goto Retry;
                launcherDelayMs(0);
                return false;
            }
            tft->display(false);
        }
    } else { // Running in Access point mode
#if !CONFIG_ESP_HOSTED_ENABLED
        launcherWifiStop();
        vTaskDelay(50 / portTICK_PERIOD_MS);
#endif
        launcherWifiStartAp("Launcher", "", 6, 4);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        launcherConsolePrintf("IP: %s\n", launcherWifiApIp().c_str());
    }
    launcherDelayMs(0);
    return isAP || launcherWifiIsConnected();
}
bool connectWifi() {
    displayRedStripe("Scanning...");
#if CONFIG_ESP_HOSTED_ENABLED
    launcherWifiStop();
#endif
    std::vector<LauncherWifiAp> networks;
    int nets = launcherWifiScan(networks);
    // Serial.printf("connectWifi: scan returned %d networks\n", nets);
    options = {};
    for (int i = 0; i < nets; i++) {
        String networkSsid = networks[i].ssid.c_str();
        if (networkSsid.isEmpty()) continue;
        int authMode = static_cast<int>(networks[i].authmode);
        options.push_back({networkSsid, [=]() { wifiConnect(networkSsid, authMode); }});
    }
    options.push_back({"Hidden SSID", [=]() {
                           String __ssid = keyboard("", 32, "Your SSID");
                           if (__ssid != String(KEY_ESCAPE)) wifiConnect(__ssid.c_str(), 8);
                       }});
    options.push_back({"Main Menu", [=]() { returnToMenu = true; }});
    loopOptions(options);
    return launcherWifiIsConnected();
}

bool ensureWifiConnected(String ssid, int encryptation, bool isAP) {
    if (launcherWifiIsConnected() && !isAP) return true;
    if (isAP) return wifiConnect(ssid, encryptation, true);
    if (!ssid.isEmpty()) return wifiConnect(ssid, encryptation, false);
    return connectWifi();
}
/***************************************************************************************
** Function name: ota_function
** Description:   Start OTA function
***************************************************************************************/
void ota_function() {
#ifndef DISABLE_OTA
    bool fav = false;
    if (ensureWifiConnected()) {
        // Debug
        // Serial.printf("Favorite size: %d\n", favorite.size());
        // serializeJsonPretty(favorite, Serial);
        // Debug
        if (favorite.size() > 0) {
            options = {
                {"OTA List",      [&]() { fav = false; }        },
                {"Favorite List", [&]() { fav = true; }         },
                {"Main Menu",     [=]() { returnToMenu = true; }}
            };
            loopOptions(options);
        }
        if (returnToMenu) return;
        if (fav) {
            int idx = 0;
            auto NavMenu = [&](int fw) {
                options.clear();
                if (favorite[fw]["fid"].as<String>().length() > 0) {
                    options.push_back({"View firmware", [=]() {
                                           loopVersions(favorite[fw]["fid"].as<String>());
                                       }});
                } else {
                    options.push_back({"Install", [=]() {
                                           installExtFirmware(favorite[fw]["link"].as<String>());
                                       }});
                }
                options.push_back({"Remove Favorite", [=]() {
                                       favorite.remove(fw);
                                       saveConfigs();
                                   }});
                options.push_back({"Back to List", [=]() { /* Do nothing, just return */ }});
                options.push_back({"Main Menu", [=]() { returnToMenu = true; }});
                loopOptions(options);
            };
        RELOAD:
            options.clear();
            int count = 0;
            for (JsonObject item : favorite) {
                options.push_back({item["name"].as<String>(), [=]() { NavMenu(count); }});
                count++;
            }
            options.push_back({"Main Menu", [=]() { returnToMenu = true; }, ALCOLOR});
            idx = loopOptions(options, false, FGCOLOR, BGCOLOR, false, idx);
            if (!returnToMenu && idx != -1) goto RELOAD;
        } else {
            if (GetJsonFromLauncherHub()) loopFirmware();
        }
    }
    tft->fillScreen(BGCOLOR);
#endif
}

#ifndef DISABLE_OTA

/***************************************************************************************
** Function name: replaceChars
** Description:   Replace some characters for _
***************************************************************************************/
String replaceChars(String input) {
    // Define os caracteres que devem ser substituídos
    const char charsToReplace[] = {'<', '>', ':', '\"', '/', '\\', '|', '?', '*', '\'', '`', '&'};
    // Define o caractere de substituição (neste exemplo, usamos um espaço)
    const char replacementChar = '_';

    // Percorre a string e substitui os caracteres especificados
    for (size_t i = 0; i < sizeof(charsToReplace); i++) {
        input.replace(String(charsToReplace[i]), String(replacementChar));
    }

    for (size_t i = 0; i < input.length(); i++) {
        if (input[i] < 32) { input.setCharAt(i, replacementChar); }
    }

    input.trim();
    while (input.endsWith(".") || input.endsWith(" ")) { input.remove(input.length() - 1); }

    if (input.isEmpty()) input = "firmware";

    return input;
}

struct RangeBufferContext {
    uint8_t *buffer;
    size_t capacity;
    size_t written;
};

bool rangeBufferCb(const uint8_t *data, size_t len, void *ctx) {
    RangeBufferContext *range = static_cast<RangeBufferContext *>(ctx);
    if (!range || !range->buffer || range->written + len > range->capacity) return false;
    memcpy(range->buffer + range->written, data, len);
    range->written += len;
    return true;
}

static uint8_t inputHandlerPauseDepth = 0;

void pauseInputHandlerTask() {
    if (!xHandle) return;
    if (inputHandlerPauseDepth++ == 0) vTaskSuspend(xHandle);
}

void resumeInputHandlerTask() {
    if (!xHandle || inputHandlerPauseDepth == 0) return;
    inputHandlerPauseDepth--;
    if (inputHandlerPauseDepth == 0) vTaskResume(xHandle);
}

bool discardHttpCb(const uint8_t *, size_t, void *) { return true; }

bool parseContentRangeTotal(const char *contentRange, size_t &total) {
    if (!contentRange) return false;
    String range = contentRange;
    int slash = range.lastIndexOf('/');
    if (slash < 0 || slash + 1 >= range.length()) return false;
    total = range.substring(slash + 1).toInt();
    return total > 0;
}

bool getRemoteFileSize(const String &url, size_t &size, const char *hwid = nullptr) {
    LauncherHttpResponse response;
    if (!launcherHttpGetRange(url.c_str(), 0, 1, discardHttpCb, nullptr, &response, hwid)) return false;
    if (response.status != 206) return false;
    return parseContentRangeTotal(response.content_range, size);
}

struct FileDownloadContext {
    File *file;
    size_t downloaded;
    size_t expected;
    long progressTick;
    LauncherHttpResponse *response; // back-pointer to get content_length once headers arrive
};

bool fileDownloadCb(const uint8_t *data, size_t len, void *ctx) {
    FileDownloadContext *download = static_cast<FileDownloadContext *>(ctx);
    if (!download || !download->file) return false;

    // On the first chunk, content_length is already populated by fetch_headers.
    // Use it to initialize the progress bar with the real file size.
    if (download->expected == 0 && download->response && download->response->content_length > 0) {
        download->expected = static_cast<size_t>(download->response->content_length);
        progressHandler(0, download->expected);
    }

    size_t totalWrote = 0;
    constexpr size_t sdWriteChunk = 512;
    while (totalWrote < len) {
        const size_t part = min(sdWriteChunk, len - totalWrote);
        size_t wrote = download->file->write(data + totalWrote, part);
        if (wrote != part) return false;
        totalWrote += wrote;
    }
    download->downloaded += totalWrote;

    if (download->expected > 0) {
        if (download->progressTick >= 10) {
            download->file->flush();
            vTaskDelay(pdMS_TO_TICKS(2));
            progressHandler(download->downloaded, download->expected);
            vTaskDelay(pdMS_TO_TICKS(2));
            download->progressTick = 0;
        } else {
            download->progressTick++;
        }
    }
    return true;
}

struct RawHttpUpdateContext {
    uint32_t address;
    size_t partitionSize;
    size_t expected;
    size_t written;
    bool appImage;
    bool started;
};

bool launcherRawUpdateHttpCb(const uint8_t *data, size_t len, void *ctx) {
    RawHttpUpdateContext *updateCtx = static_cast<RawHttpUpdateContext *>(ctx);
    if (!updateCtx) return false;
    if (!updateCtx->started) {
        if (!launcherRawUpdateBegin(
                updateCtx->address, updateCtx->partitionSize, updateCtx->expected, updateCtx->appImage
            )) {
            return false;
        }
        updateCtx->started = true;
        progressHandler(0, updateCtx->expected);
    }
    const size_t remaining =
        updateCtx->written < updateCtx->expected ? updateCtx->expected - updateCtx->written : 0;
    const size_t writeLen = len > remaining ? remaining : len;
    if (writeLen == 0) return true;
    size_t wrote = launcherRawUpdateWrite(data, writeLen);
    if (wrote != writeLen) { return false; }
    updateCtx->written += wrote;
    progressHandler(updateCtx->written, updateCtx->expected);
    return true;
}

bool flashRawRangeFromHttp(
    const String &url, uint32_t sourceOffset, size_t imageSize, const LauncherPartitionEntry &target,
    bool appImage, const char *hwid = nullptr
) {
    pauseInputHandlerTask();
    RawHttpUpdateContext update = {target.offset, target.size, imageSize, 0, appImage, false};
    bool httpOk = false;
    LauncherHttpResponse response;
    constexpr uint8_t maxAttempts = 24;
    for (uint8_t attempt = 0; update.written < imageSize && attempt < maxAttempts; ++attempt) {
        size_t before = update.written;
        const uint32_t requestOffset = sourceOffset + update.written;
        const size_t remaining = imageSize - update.written;
        response = LauncherHttpResponse();
        httpOk = launcherHttpGetRange(
            url.c_str(), requestOffset, remaining, launcherRawUpdateHttpCb, &update, &response, hwid
        );
        if (httpOk && update.written == imageSize) break;
        if (update.written == before) break;
        launcherDelayMs(500);
    }
    bool complete = update.written == imageSize;
    bool endOk = complete && launcherRawUpdateEnd();
    bool ok = complete && endOk;
    if (ok && !appImage) {
        String patchError;
        if (!launcherPatchReducedLittlefsSuperblocks(target, &patchError)) {
            launcherConsolePrintf(
                "LittleFS patch failed after HTTP copy label=%s offset=0x%08X size=0x%08X: %s\n",
                target.label,
                target.offset,
                target.size,
                patchError.c_str()
            );
            ok = false;
        }
    }
    resumeInputHandlerTask();
    return ok;
}

bool installFirmwareDynamic(
    const String &fileAddr, const String &file, uint32_t appSize, uint32_t appPartitionSize,
    uint32_t appOffset, bool nb, std::vector<LauncherInstallDataPartition> &dataPartitions,
    const String &installedName
) {
    String error;
    LauncherPartitionTable table;
    if (!launcherPartitionReadCurrent(table, &error)) {
        displayRedStripe(error.length() ? error : "Partition read failed");
        launcherDelayMs(2000);
        return false;
    }

    size_t updateSize = appSize;
    String hwid = String(launcherWifiMac().c_str());
    if (updateSize == 0) {
        size_t remoteSize = 0;
        if (!getRemoteFileSize(fileAddr, remoteSize, hwid.c_str())) {
            displayRedStripe("Size failed");
            launcherDelayMs(2000);
            return false;
        }
        if (nb) {
            updateSize = remoteSize;
        } else {
            if (appOffset >= remoteSize) {
                displayRedStripe("Bad app offset");
                launcherDelayMs(2000);
                return false;
            }
            updateSize = remoteSize - appOffset;
        }
    }
    if (updateSize == 0) {
        displayRedStripe("Invalid app size");
        launcherDelayMs(2000);
        return false;
    }
    if (appPartitionSize == 0 || appPartitionSize < updateSize) appPartitionSize = updateSize;

    String appLabel = launcherInstallNextAppLabel(table, file, installedName);
    LauncherPartitionEntry appEntry;

    if (!launcherSelectInstallLayout(table, appPartitionSize, appLabel, dataPartitions, appEntry, error)) {
        displayRedStripe(error.length() ? error : "No install space");
        launcherDelayMs(2000);
        return false;
    }

    pauseInputHandlerTask();
    bool success = false;
    displayRedStripe("Installing APP");
    prog_handler = 0;
    progressHandler(0, updateSize);
    if (!flashRawRangeFromHttp(fileAddr, nb ? 0 : appOffset, updateSize, appEntry, true, hwid.c_str())) {
        displayRedStripe(String("APP: ") + launcherUpdateLastErrorName());
        launcherDelayMs(2000);
        goto DONE;
    }

    for (const auto &dp : dataPartitions) {
        if (!dp.hasEntry || dp.copySize == 0) continue;
        const char *typeStr = dp.subtype == 0x81 ? "FAT" : dp.subtype == 0x83 ? "LittleFS" : "SPIFFS";
        displayRedStripe(String("Installing ") + typeStr);
        prog_handler = 1;
        const uint32_t copySize = dp.copySize > dp.entry.size ? dp.entry.size : dp.copySize;
        progressHandler(0, copySize);
        if (!flashRawRangeFromHttp(fileAddr, dp.sourceOffset, copySize, dp.entry, false, hwid.c_str())) {
            displayRedStripe(String(typeStr) + ": " + launcherUpdateLastErrorName());
            launcherDelayMs(2000);
            goto DONE;
        }
    }

    displayRedStripe("Writing table");
    if (!launcherPartitionWriteGeneratedTable(table, &error)) {
        displayRedStripe(error.length() ? error : "Table failed");
        launcherDelayMs(2000);
        goto DONE;
    }

    displayRedStripe("Setting boot");
    if (!launcherPartitionSetOtaBoot(table, appEntry.subtype, &error)) {
        displayRedStripe(error.length() ? error : "Boot failed");
        launcherDelayMs(2000);
        goto DONE;
    }

    {
        std::vector<String> fatLabels;
        String registeredSpiffsLabel;
        for (const auto &dp : dataPartitions) {
            if (!dp.hasEntry) continue;
            if (dp.subtype == 0x81) fatLabels.push_back(dp.label);
            else registeredSpiffsLabel = dp.label;
        }
        launcherSaveInstalledAppMetadata(
            table, appEntry, file, installedName, fatLabels, registeredSpiffsLabel
        );

        String appNum = generateAppNum(file);
        BackupInstallInfo bkInfo;
        bkInfo.appNum = appNum;
        bkInfo.sdFilepath = file;
        bkInfo.appName = installedName.isEmpty() ? String(appEntry.label) : installedName;
        for (const auto &dp : dataPartitions) {
            if (!dp.hasEntry) continue;
            BackupPartitionInfo part;
            part.label = dp.label;
            part.type = dp.subtype == 0x81 ? "FAT" : dp.subtype == 0x83 ? "LittleFS" : "SPIFFS";
            bkInfo.partitions.push_back(part);
        }
        saveInstalledToConfig(bkInfo);
        if (autoBackup && !bkInfo.partitions.empty()) backupAllPartitionsForApp(appNum);
    }

    saveIntoNVS();
    success = true;

DONE:
    resumeInputHandlerTask();
    if (success) {
        displayRedStripe("Restarting");
        launcherDelayMs(500);
        reboot();
    }
    return success;
}

bool getInfo(String serverUrl, JsonDocument &_doc) {
    if (launcherWifiIsConnected()) {
        pauseInputHandlerTask();
        resetTftDisplay();
        tft->drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, FGCOLOR);
        tft->drawCentreString("Getting info from", tftWidth / 2, tftHeight / 3, 1);
        tft->drawCentreString("LauncherHub", tftWidth / 2, tftHeight / 3 + FM * 9, 1);
        tft->display(false);
        tft->setCursor(18, tftHeight / 3 + FM * 9 * 2);
        const uint8_t maxAttempts = 5;
        for (uint8_t attempt = 0; attempt < maxAttempts; ++attempt) {
            String payload;
            if (launcherHttpGetToString(serverUrl.c_str(), payload)) {
                _doc.clear();
                DeserializationError error = deserializeJson(_doc, payload);
                if (error) {
                    displayRedStripe("JSON Parse Failed");
                    vTaskDelay(1500 / portTICK_PERIOD_MS);
                    _doc.clear();
                    resumeInputHandlerTask();
                    return false;
                }
                resumeInputHandlerTask();
                return true;
            }

            tftprint(".", 10);
            vTaskDelay(pdTICKS_TO_MS(500));
        }
    }
    resumeInputHandlerTask();
    return false;
}

/***************************************************************************************
** Function name: GetJsonFromLauncherHub
** Description:   Gets JSON from github server
***************************************************************************************/
String encodeQueryValue(const String &value) {
    String encoded;
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            encoded += hex;
        }
    }
    return encoded;
}

String normalizeExtraQuery(String extra) {
    extra.trim();
    while (extra.startsWith("/") || extra.startsWith("\\")) { extra.remove(0, 1); }
    if (extra.length() == 0) return extra;
    if (!extra.startsWith("&") && !extra.startsWith("?")) extra = "&" + extra;
    return extra;
}

bool GetJsonFromLauncherHub(uint8_t page, String order, bool star, String query) {
    String q = "&order_by=" + order;
    q += page > 1 ? "&page=" + String(page) : "";
    q += query.length() > 0 ? "&q=" + encodeQueryValue(query) : "";
    q += star ? "&star=1" : "";
#ifdef OTA_EXTRA
    q += normalizeExtraQuery(String(OTA_EXTRA));
#endif
    String serverUrl = "https://api.launcherhub.net/firmwares?category=" + String(OTA_TAG) + q;

    if (getInfo(serverUrl, doc)) {
        total_firmware = doc["total"].as<int>();
        num_pages = doc["total"].as<int>() / doc["page_size"].as<int>();
        current_page = page;
        return true;
    }
    displayRedStripe("Firmware list fetch Failed");
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    return false;
}
JsonDocument getVersionInfo(String fid) {
    JsonDocument versions;
    String serverUrl = "https://api.launcherhub.net/firmwares?fid=" + fid;
    if (!getInfo(serverUrl, versions)) {
        displayRedStripe("Version fetch Failed");
        vTaskDelay(1500 / portTICK_PERIOD_MS);
    }
    return versions;
}

void installFirmwareFromManifest(String fid, String version, String installedName) {
    displayRedStripe("Getting install info");

    JsonDocument detail;
    String serverUrl =
        "https://api.launcherhub.net/firmwares?fid=" + fid + "&version=" + encodeQueryValue(version);
    if (!getInfo(serverUrl, detail)) {
        displayRedStripe("Install info failed");
        launcherDelayMs(2000);
        return;
    }

    JsonObject versionObj = detail["version"].as<JsonObject>();
    JsonObject install = versionObj["install"].as<JsonObject>();
    JsonObject app = install["app"].as<JsonObject>();
    JsonArray partitions = install["partitions"].as<JsonArray>();
    String file = versionObj["file"].as<String>();
    if (file.isEmpty() || app.isNull()) {
        displayRedStripe("Bad install info");
        launcherDelayMs(2000);
        return;
    }

    uint32_t appOffset = app["source_offset"] | 0;
    uint32_t appCopySize = app["image_size"] | 0;
    uint32_t appPartitionSize = appCopySize;
    bool nb = appOffset == 0;

    std::vector<LauncherInstallDataPartition> dataPartitions;

    for (JsonObject part : partitions) {
        String type = part["type"].as<String>();
        String subtype = part["subtype"].as<String>();
        if (type == "app" && subtype == "ota") {
            appOffset = part["source_offset"] | appOffset;
            appCopySize = part["copy_size"] | appCopySize;
            appPartitionSize = appCopySize;
            nb = appOffset == 0;
        } else if (type == "data" && (subtype == "spiffs" || subtype == "littlefs")) {
            LauncherInstallDataPartition dp;
            dp.subtype = (subtype == "littlefs") ? 0x83 : 0x82;
            uint32_t declaredSize = part["size"] | 0;
            dp.sourceOffset = part["source_offset"] | 0;
            dp.copySize = part["copy_size"] | 0;
            dp.label = part["label"].as<String>();
            if (dp.label.isEmpty()) dp.label = "spiffs";
            if (dp.label == "assets" && declaredSize > LAUNCHER_DEFAULT_SPIFFS_SIZE) {
                dp.partitionSize = declaredSize;
            } else if (declaredSize > LAUNCHER_DEFAULT_SPIFFS_THRESHOLD) {
                dp.partitionSize = LAUNCHER_INSTALL_USE_REMAINING_SPIFFS_SIZE;
            } else {
                dp.partitionSize = LAUNCHER_DEFAULT_SPIFFS_SIZE;
            }
            dataPartitions.push_back(dp);
        } else if (type == "data" && subtype == "fat") {
            LauncherInstallDataPartition dp;
            dp.subtype = 0x81;
            dp.label = part["label"].as<String>();
            bool hasExistingFat = std::any_of(
                dataPartitions.begin(), dataPartitions.end(), [](const LauncherInstallDataPartition &d) {
                    return d.subtype == 0x81;
                }
            );
            uint32_t declaredSize = part["size"] | 0;
            dp.sourceOffset = part["source_offset"] | 0;
            uint32_t requestedCopySize = part["copy_size"] | 0;
            LauncherPartitionPayloadPlan payload =
                launcherPartitionFatPayloadPlan(dp.label.c_str(), declaredSize, requestedCopySize);
            dp.partitionSize = payload.partitionSize;
            dp.copySize = payload.copySize;
            dataPartitions.push_back(dp);
        }
    }

    if (appCopySize == 0 || appPartitionSize == 0) {
        displayRedStripe("Invalid app size");
        launcherDelayMs(2000);
        return;
    }

    if (!file.startsWith("https://")) file = M5_SERVER_PATH + file;
    String fileAddr = "https://api.launcherhub.net/download?fid=" + fid + "&file=" + file;
    if (fid == "") fileAddr = file;

    String manifestName = detail["name"].as<String>();
    if (!manifestName.isEmpty()) installedName = manifestName;

    if (!installFirmwareDynamic(
            fileAddr, file, appCopySize, appPartitionSize, appOffset, nb, dataPartitions, installedName
        )) {
        launcherDelayMs(2500);
    }
}
/***************************************************************************************
** Function name: downloadFirmware
** Description:   Downloads the firmware and save into the SDCard
***************************************************************************************/
void downloadFirmware(String fid, String file_url, String fileName, String folder) { // Adicionar "fid"
    displayRedStripe("Preparing..");
    if (!file_url.startsWith("https://")) file_url = M5_SERVER_PATH + file_url;
    String fileAddr = "https://api.launcherhub.net/download?fid=" + fid + "&file=" + file_url;
    if (fid == "") fileAddr = file_url;
    int tries = 0;
    fileName = replaceChars(fileName);
    prog_handler = 2;
    if (!setupSdCard()) {
        displayRedStripe("SDCard Not Found");
        launcherDelayMs(2500);
        return;
    }
    if (!folder.endsWith("/")) folder = folder + "/";
    if (!folder.startsWith("/")) folder = "/" + folder;
    String folder_name = folder.substring(0, folder.length() - 1);
    if (folder_name.length() > 2) {
        if (!SDM.exists(folder_name)) {
            if (!SDM.mkdir(folder_name)) {
                log_i("Download: Couldn't create folder '%s'\n", folder_name.c_str());
                displayRedStripe("Can't create: '" + folder_name + "'");
                launcherDelayMs(2000);
                return;
            }
        }
    }
    String filePath = folder + fileName + ".bin";
    File file;
retry:
    file = SDM.open(filePath, FILE_WRITE);
    if (!file) {
        log_i("Download: Couldn't create file %s", filePath.c_str());
        displayRedStripe("Fail creating file.");
        launcherDelayMs(2000);
        return;
    }
    LauncherHttpResponse response;
    prog_handler = 2;
    pauseInputHandlerTask();
    FileDownloadContext download = {&file, 0, 0, 0, &response};
    bool ok = launcherHttpGetStream(
        fileAddr.c_str(), fileDownloadCb, &download, &response, "HWID", launcherWifiMac().c_str()
    );
    file.flush();
    file.close();
    resumeInputHandlerTask();

    vTaskDelay(pdTICKS_TO_MS(50));
    file = SDM.open(filePath, FILE_READ);
    size_t sdSize = file ? file.size() : 0;
    if (file) file.close();
    if ((!ok || sdSize <= bufSize) && tries < 1) {
        tries++;
        SDM.remove(filePath);
        goto retry;
    }
    if (!ok || (response.content_length > 0 && sdSize != (size_t)response.content_length)) {
        SDM.remove(filePath);
        displayRedStripe("Download FAILED");
        while (!check(SelPress)) yield();
    } else {
        progressHandler(100, 100);
        launcherConsolePrintln("File successfully downloaded..");
        displayRedStripe(" Downloaded ");
        while (!check(SelPress)) yield();
    }
    wakeUpScreen();
}
/***************************************************************************************
** Function name: installExtFirmware
** Description:   installs External Firmware using OTA grabbing file information from url
***************************************************************************************/
bool installExtFirmware(String url) {
    size_t file_size;
    bool nb = 1;
    std::vector<LauncherInstallDataPartition> dataPartitions;
    uint8_t bytes[32];
    if (!url.startsWith("http")) {
        displayRedStripe("Invalid link");
        launcherDelayMs(2000);
        return false;
    }
    displayRedStripe("Getting file info");
    LauncherHttpResponse response;
    RangeBufferContext range = {buff, bufSize, 0};
    if (!launcherHttpGetRange(url.c_str(), 32768, 416, rangeBufferCb, &range, &response) ||
        response.status != 206) {
        displayRedStripe("File not found");
        launcherDelayMs(2000);
        return false;
    }
    if (!parseContentRangeTotal(response.content_range, file_size)) return false;

    size_t PartitionSize = 0;
    size_t PartitionOffset = 0x10000;
    if (buff[0] == 0xAA) {
        nb = 0;
        for (int i = 0x0; i <= 0x1A0; i += 0x20) {
            memcpy(bytes, &buff[i], 32);

            if (bytes[3] == 0x00 || (bytes[3] >= 0x10 && bytes[3] <= 0x1F)) {
                if (bytes[0x0A] > 0 && PartitionSize == 0) {
                    PartitionSize = (bytes[0x0A] << 16) | (bytes[0x0B] << 8) | bytes[0x0C];
                    PartitionOffset = (bytes[0x06] << 16) | (bytes[0x07] << 8) | bytes[0x08];
                }
            }
            if (bytes[3] == 0x81) {
                char labelBuf[17] = {0};
                memcpy(labelBuf, bytes + 12, 16);
                if (labelBuf[0] == '\0') continue;
                LauncherInstallDataPartition dp;
                dp.subtype = 0x81;
                dp.label = String(labelBuf);
                dp.sourceOffset = (bytes[0x06] << 16) | (bytes[0x07] << 8) | bytes[0x08];
                bytes[0x0C] = 0;
                uint32_t declaredSize = (bytes[0x0A] << 16) | (bytes[0x0B] << 8) | bytes[0x0C];
                LauncherPartitionPayloadPlan payload =
                    launcherPartitionFatPayloadPlan(dp.label.c_str(), declaredSize, declaredSize);
                dp.partitionSize = payload.partitionSize;
                dp.copySize = payload.copySize;
                dataPartitions.push_back(dp);
            }
            if (bytes[3] == 0x82 || bytes[3] == 0x83) {
                char labelBuf[17] = {0};
                memcpy(labelBuf, bytes + 12, 16);
                if (labelBuf[0] == '\0') continue;
                LauncherInstallDataPartition dp;
                dp.subtype = bytes[3];
                dp.label = String(labelBuf);
                dp.sourceOffset = (bytes[0x06] << 16) | (bytes[0x07] << 8) | bytes[0x08];
                bytes[0x0C] = 0;
                dp.partitionSize = (bytes[0x0A] << 16) | (bytes[0x0B] << 8) | bytes[0x0C];
                dp.copySize = dp.partitionSize;
                dataPartitions.push_back(dp);
            }
        }
        if (file_size < PartitionOffset + PartitionSize) PartitionSize = file_size - PartitionOffset;
        for (auto &dp : dataPartitions) {
            if (dp.subtype != 0x81 && file_size < dp.sourceOffset + dp.copySize)
                dp.copySize = file_size - dp.sourceOffset;
        }
    }
    installFirmware("", url, PartitionSize, PartitionOffset, nb, dataPartitions, "External OTA");
    return true;
}

/***************************************************************************************
** Function name: installFirmware
** Description:   installs Firmware using OTA
***************************************************************************************/
void installFirmware(
    String fid, String file, uint32_t app_size, uint32_t app_offset, bool nb,
    std::vector<LauncherInstallDataPartition> &dataPartitions, String installedName
) {
    if (!file.startsWith("https://")) file = M5_SERVER_PATH + file;
    String fileAddr = "https://api.launcherhub.net/download?fid=" + fid + "&file=" + file;
    if (fid == "") fileAddr = file;

    {
        auto spiffsIt = std::find_if(
            dataPartitions.begin(), dataPartitions.end(), [](const LauncherInstallDataPartition &d) {
                return d.subtype != 0x81;
            }
        );
        if (spiffsIt != dataPartitions.end()) {
            if (!askSpiffs) {
                spiffsIt->copySize = 0;
            } else if (spiffsIt->copySize > 0) {
                bool copySpiffs = true;
                options = {
                    {"SPIFFS No",  [&]() { copySpiffs = false; }},
                    {"SPIFFS Yes", [&]() { copySpiffs = true; } },
                };
                loopOptions(options);
                if (!copySpiffs) spiffsIt->copySize = 0;
            }
        }
    }

    tft->fillRect(7, 40, tftWidth - 14, 88, BGCOLOR); // Erase the information below the firmware name
    displayRedStripe("Connecting FW");

    if (!installFirmwareDynamic(
            fileAddr, file, app_size, app_size, app_offset, nb, dataPartitions, installedName
        )) {
        launcherDelayMs(2500);
    }
}

/***************************************************************************************
** Function name: installFAT_OTA
** Description:   install FAT partition OverTheAir
***************************************************************************************/
bool installFAT_OTA(String file, uint32_t offset, uint32_t size, const char *label) {
    prog_handler = 1; // review

    tft->fillRect(7, 40, tftWidth - 14, 88, BGCOLOR); // Erase the information below the firmware name
    displayRedStripe("Connecting FAT");

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) {
        displayRedStripe("Partition not found");
        launcherDelayMs(2500);
        return false;
    }

    RawHttpUpdateContext fatUpdate = {partition->address, partition->size, size, 0, false, false};
    displayRedStripe("Installing FAT");
    pauseInputHandlerTask();
    bool ok = launcherHttpGetRange(file.c_str(), offset, size, launcherRawUpdateHttpCb, &fatUpdate) &&
              fatUpdate.written == size && launcherRawUpdateEnd();
    resumeInputHandlerTask();
    if (ok) {
        String patchError;
        if (!launcherPatchReducedLittlefsSuperblocks(partition->address, partition->size, &patchError)) {
            launcherConsolePrintf(
                "LittleFS patch failed after FAT OTA label=%s: %s\n", label, patchError.c_str()
            );
            ok = false;
        }
    }
    vTaskDelay(pdTICKS_TO_MS(500));
    return ok;
}

#endif
