#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SPI.h>

// ===================== Module stockage logs GPS (SD ou LittleFS) =====================
//
// Interface volontairement minimale, dans le meme esprit que GpsManager.h :
// mutualisee telle quelle avec l'autre variante (OLED/TFT) -- si tu
// modifies cette interface, pense a reporter le changement dans l'autre
// projet (cf. WebServerManager.h, meme principe de partage).
//
// Ce module NE possede PAS son propre SPIClass et ne fait jamais
// spi.begin() lui-meme -- la gestion du bus SPI reste la responsabilite
// de main.cpp, car elle differe structurellement selon la variante :
//  - TFT : bus SPI deja utilise par l'ecran (tftSPI) -- MOSI/SCLK
//    partages, seuls CS+MISO sont dedies a la SD.
//  - OLED : ecran en I2C, aucun bus SPI existant -- bus SPI entierement
//    dedie a la SD (cf. SPIClass sdSPI dans main.cpp).
// Le module se contente de recevoir un SPIClass& deja initialise +
// un pin CS, et gere tout le reste (SD.begin(), repli LittleFS,
// migration, stats pour la page web).

// Filesystem actif pour les logs GPS detailles (log_*.csv) -- &SD si la
// carte est active, &LittleFS en repli sinon. Valide seulement APRES
// l'appel a initSdLogStorage(). Le carnet cumulatif (sessions.csv) et
// circuits.csv ne suivent JAMAIS ce choix -- ils restent toujours sur
// LittleFS, quel que soit l'etat de la SD (cf. WebServerManager.h).
extern fs::FS* gpsLogFs;

// true si gpsLogFs pointe reellement sur la SD (false = repli LittleFS).
// Sert uniquement a choisir un libelle d'affichage ("SD"/"LittleFS")
// dans les logs Serial -- gpsLogFs lui-meme suffit pour toute decision
// fonctionnelle (gpsLogFs == &SD).
extern bool gpsLogsOnSd;

// Tente SD.begin(csPin, spi) -- spi doit deja avoir ete initialisee par
// l'appelant (sdSPI.begin(...) cote OLED, tftSPI.begin(...) cote TFT) --
// ce module ne touche jamais au bus lui-meme. Bascule gpsLogFs sur &SD
// et retourne true en cas de succes ; laisse gpsLogFs sur &LittleFS et
// retourne false sinon (carte absente, mal cablee, en panne -- repli
// automatique, la SD reste facultative). A appeler dans setup(), apres
// LittleFS.begin().
bool initSdLogStorage(uint8_t csPin, SPIClass& spi);

// Migre les eventuels log_*.csv restes sur LittleFS (sessions
// enregistrees ou importees AVANT que la SD ne soit active) vers
// gpsLogFs -- no-op si gpsLogFs pointe toujours sur LittleFS (rien a
// migrer). Idempotente : ne fait rien des la 2e fois. Copie verifiee par
// taille avant suppression de la source -- aucun risque de perte si la
// carte est retiree ou tombe en panne en plein milieu. A appeler juste
// apres initSdLogStorage() si elle a renvoye true.
void migrateLittleFsLogsToSd();

// Pour la barre de stockage SD sur la page web (WebServerStatusInfo) --
// valides seulement si gpsLogFs == &SD (sdUsedBytes()/sdTotalBytes()
// renvoient 0 sinon, plutot que de planter sur un SD.usedBytes() appele
// alors que la carte n'a jamais ete initialisee).
uint64_t sdUsedBytes();
uint64_t sdTotalBytes();
