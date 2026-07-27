#include "SdLogStorage.h"
#include <SD.h>
#include <LittleFS.h>
#include <vector>

fs::FS* gpsLogFs = &LittleFS;
bool gpsLogsOnSd = false;

bool initSdLogStorage(uint8_t csPin, SPIClass& spi) {
  if (!SD.begin(csPin, spi)) {
    gpsLogFs = &LittleFS;
    gpsLogsOnSd = false;
    return false;
  }
  gpsLogFs = &SD;
  gpsLogsOnSd = true;
  return true;
}

void migrateLittleFsLogsToSd() {
  if (gpsLogFs == &LittleFS) return; // SD non active -- rien a migrer, les logs restent deja sur LittleFS

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
      LittleFS.remove(name); // deja present sur la SD (migration precedente interrompue) -- nettoie juste le reste sur LittleFS
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
      LittleFS.remove(name); // copie confirmee complete -- libere la place sur LittleFS
      migrated++;
    } else {
      gpsLogFs->remove(name); // copie partielle/ratee -- ne laisse pas un fichier corrompu sur la SD
      failed++;
    }
  }
  Serial.printf("Migration SD : %d log(s) migre(s), %d echec(s).\n", migrated, failed);
}

uint64_t sdUsedBytes() {
  if (gpsLogFs != &SD) return 0;
  return SD.usedBytes();
}

uint64_t sdTotalBytes() {
  if (gpsLogFs != &SD) return 0;
  return SD.totalBytes();
}
