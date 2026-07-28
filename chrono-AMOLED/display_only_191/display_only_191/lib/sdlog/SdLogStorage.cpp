#include "SdLogStorage.h"
#include <SD_MMC.h>
#include <LittleFS.h>
#include <vector>

// Pins SDMMC confirmes au bring-up du 1.91 (mode 1-fil -- flag
// "VersionControl_V2" actif par defaut dans le driver Waveshare d'origine,
// cf. README_AMOLED_bringup.md).
#define SD_MMC_D0   8
#define SD_MMC_CMD  42
#define SD_MMC_CLK  9

fs::FS* gpsLogFs = &LittleFS;
bool gpsLogsOnSd = false;

bool initSdLogStorage() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (!SD_MMC.begin("/sdcard", true /* mode1bit -- SDMMC 1-fil, pas 4-fils */)) {
    gpsLogFs = &LittleFS;
    gpsLogsOnSd = false;
    return false;
  }
  gpsLogFs = &SD_MMC;
  gpsLogsOnSd = true;
  return true;
}

// Logique identique au TFT -- generique via gpsLogFs, ne depend pas de
// SD vs SD_MMC.
void migrateLittleFsLogsToSd() {
  if (gpsLogFs == &LittleFS) return; // SD non active -- rien a migrer

  std::vector<String> toMigrate;
  File root = LittleFS.open("/");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (!name.startsWith("/")) name = "/" + name;
      if (name.indexOf("/log_") >= 0 && name.endsWith(".csv")) toMigrate.push_back(name);
      f = root.openNextFile();
    }
  }
  if (toMigrate.empty()) return;

  Serial.printf("Migration SD : %d log(s) GPS trouve(s) sur LittleFS, deplacement vers la SD...\n", (int)toMigrate.size());
  int migrated = 0, failed = 0;
  for (const String& name : toMigrate) {
    if (gpsLogFs->exists(name)) {
      LittleFS.remove(name);
      continue;
    }
    File src = LittleFS.open(name, "r");
    if (!src) { failed++; continue; }
    uint32_t srcSize = src.size();
    File dst = gpsLogFs->open(name, "w");
    if (!dst) { src.close(); failed++; continue; }

    uint32_t written = 0;
    uint8_t buf[512];
    int n;
    while ((n = src.read(buf, sizeof(buf))) > 0) written += dst.write(buf, n);
    src.close();
    dst.close();

    if (written == srcSize) {
      LittleFS.remove(name);
      migrated++;
    } else {
      gpsLogFs->remove(name);
      failed++;
    }
  }
  Serial.printf("Migration SD : %d log(s) migre(s), %d echec(s).\n", migrated, failed);
}

uint64_t sdUsedBytes() {
  if (gpsLogFs != &SD_MMC) return 0;
  return SD_MMC.usedBytes();
}

uint64_t sdTotalBytes() {
  if (gpsLogFs != &SD_MMC) return 0;
  return SD_MMC.totalBytes();
}
