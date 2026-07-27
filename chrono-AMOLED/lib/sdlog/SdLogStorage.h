#pragma once

#include <Arduino.h>
#include <FS.h>

// ===================== Module stockage logs GPS (SD ou LittleFS) =====================
//
// Adapte du SdLogStorage.h du projet TFT (meme interface externe --
// gpsLogFs/gpsLogsOnSd/migrateLittleFsLogsToSd()/sdUsedBytes()/
// sdTotalBytes() inchanges) pour ce board 1.91, qui utilise le mode
// **SDMMC** (1 fil, D0/CMD/CLK) et non SPI classique comme le TFT --
// cf. README_AMOLED_bringup.md, piege SD ("VersionControl_V2"). Arduino-
// ESP32 fournit `SD_MMC` (<SD_MMC.h>), qui expose la meme interface
// fs::FS que `SD` mais pilote le SDMMC en interne -- l'essentiel de la
// logique (repli LittleFS, migration, stats) reste donc identique au
// TFT, seul le montage physique change.
//
// Difference d'interface avec la version TFT : initSdLogStorage() ne
// prend plus (csPin, SPIClass&) -- inutile en SDMMC (pas de bus SPI, pas
// de CS). Les pins D0/CMD/CLK sont fixes en dur dans le .cpp (deja
// confirmes au bring-up), pas besoin de les faire remonter jusqu'a
// main.cpp.

extern fs::FS* gpsLogFs;
extern bool gpsLogsOnSd;

// Monte la carte SD en SDMMC (pins fixes, cf. .cpp) -- bascule gpsLogFs
// sur &SD_MMC et retourne true en cas de succes ; laisse gpsLogFs sur
// &LittleFS et retourne false sinon (carte absente/en panne -- repli
// automatique). A appeler dans setup(), apres LittleFS.begin().
bool initSdLogStorage();

// Migre les eventuels log_*.csv restes sur LittleFS vers gpsLogFs --
// no-op si gpsLogFs pointe toujours sur LittleFS. Idempotente.
void migrateLittleFsLogsToSd();

// Pour la barre de stockage SD sur la page web -- valides seulement si
// gpsLogFs == &SD_MMC (0 sinon).
uint64_t sdUsedBytes();
uint64_t sdTotalBytes();
