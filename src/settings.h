#ifndef __SETTINGS_H
#define __SETTINGS_H

#include <FS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>

/*
config.conf JSON structure

[
   {
      "rot": 1,
      "onlyBins":1,
      "bright":100,
      "askSpiffs":1,
      "wui_usr":"admin",
      "wui_pwd":"launcher",
      "dwn_path": "/downloads/",
      "FGCOLOR":2016,
      "BGCOLOR":0,
      "ALCOLOR":63488,
      "even":13029,
      "odd": 12485,
      "wifi":[
         {
            "ssid":"myNetSSID",
            "pwd":"myNetPassword",
         },
      ]
   }
]

*/
void settings_menu();
void _setBrightness(uint8_t brightval) __attribute__((weak));
void setBrightnessMenu();
void setBrightness(int bright, bool save = true);
void getBrightness();
bool gsetOnlyBins(bool set = false, bool value = true);
bool gsetAskSpiffs(bool set = false, bool value = true);
int gsetRotation(bool set = false);
void getConfigs();
void saveConfigs();
bool saveIntoNVS();
bool saveWifiIntoNVS();
bool getFromNVS();
bool getWifiFromNVS();
bool getWifiCredential(const String &ssid, String &password);
bool setWifiCredential(const String &ssid, const String &password, bool persist = false);
void setdimmerSet();
void setUiColor();
void chargeMode();
JsonObject ensureSettingsRoot();
#endif
