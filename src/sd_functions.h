#ifndef __SD_FUNCTIONS_H
#define __SD_FUNCTIONS_H
#include <CustomUpdate.h>
#include <FFat.h>
#include <FS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <globals.h>

extern SPIClass sdcardSPI;
#ifndef PART_04MB
bool eraseFAT();
#endif
bool setupSdCard();

bool deleteFromSd(String path);

bool renameFile(String path, String filename);

bool copyFile(String path);

bool pasteFile(String path);

bool createFolder(String path);

void readFs(String &folder, std::vector<Option> &opt);

bool sortList(const Option &a, const Option &b);

String loopSD(bool filePicker = false);

void performUpdate(Stream &updateSource, size_t updateSize, int command);

void updateFromSD(String path);

bool performFATUpdate(Stream &updateSource, size_t updateSize, const char *label = "vfs");
#endif
