/**
 * WebServerManager -- module autonome pour le telechargement WiFi a la
 * demande (point d'acces + page de gestion des sessions enregistrees,
 * + page Statut systeme).
 *
 * Separe de main.cpp pour la meme raison que sur la Pendule Paddock :
 * garder main.cpp lisible, et isoler tout ce qui touche au WiFi/HTTP
 * dans un seul endroit. Le module ne connait rien du BLE ni du detail
 * des logs -- il recoit des callbacks pour ces besoins, ce qui evite
 * les dependances croisees avec le reste du firmware.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>

// Callbacks fournis par main.cpp au demarrage (cf. begin()) :
//  - logsFs : filesystem reellement utilise pour les logs GPS par
//    session (log_*.csv) -- SD si presente et detectee au demarrage,
//    repli sur LittleFS sinon (cf. initGpsLogStorage() dans main.cpp).
//    Le carnet cumulatif (sessionLogPath) reste toujours sur LittleFS,
//    quel que soit ce choix -- seuls les logs detailles en dependent.
//  - bleStop/bleRestart : arreter/relancer le BLE autour du cycle WiFi
//    (main.cpp sait comment faire ca proprement -- cf. les notes sur
//    BLEDevice::deinit() instable dans main.cpp)
//  - flushLogs : forcer l'ecriture sur la flash des fichiers de log
//    actuellement ouverts, avant de les servir en telechargement
//  - getStatus : etat courant du systeme (BLE/GPS/circuit/REC), pour
//    la page "Statut" -- rempli par main.cpp a chaque appel, pas mis
//    en cache cote module
//
// Pas de callback "demarrer l'enregistrement a distance" : ca n'aurait
// aucun sens, le BLE est coupe pendant tout l'usage WiFi (cf. bleStop
// ci-dessus) -- demarrer un enregistrement a ce moment-la n'ecrirait
// aucune donnee GPS, juste un fichier vide.
typedef void (*WebServerBleStopFn)();
typedef void (*WebServerBleRestartFn)();
typedef void (*WebServerFlushLogsFn)();

struct WebServerStatusInfo {
  bool bleConnected = false;
  uint8_t fixStatus = 0;
  uint8_t numSats = 0;
  String circuitName = "?";
  bool recordingEnabled = false;
  int lapsCount = 0;
  String lastLapTime = "--:--.---";
  String bestLapTime = "--:--.---";
  // Position live -- utilisee par la page /circuits (bouton "capturer la
  // position GPS actuelle"). hasGpsFix distingue "pas de fix" de "fix
  // pile sur lat/lng 0,0" (qui existe en theorie, autant etre explicite).
  bool hasGpsFix = false;
  double latitude = 0;
  double longitude = 0;
  // Filesystem des logs GPS detailles, QUAND il est distinct de LittleFS
  // (typiquement une carte SD) -- LittleFS a deja sa propre barre sur la
  // page Sessions, pas la peine de la dupliquer si aucun stockage separe
  // n'est actif. usedBytes/totalBytes ne font PAS partie de l'interface
  // commune fs::FS (chaque implementation les ajoute a part) -- main.cpp,
  // qui connait le type concret (SD, FFat...), les fournit donc ici
  // plutot que WebServerManager ne les lise via g_logsFs (fs::FS*
  // generique, ne les exposerait pas).
  //
  // Valeurs par defaut neutres (false/0) : un main.cpp qui ne les
  // renseigne pas (cas de cette variante OLED tant qu'elle n'a pas sa
  // propre SD cablee) ne casse rien -- hasSeparateLogsFs reste false, la
  // 2e barre reste simplement invisible. Port depuis la variante TFT
  // (cf. README_FS.md) -- partie partagee uniquement, aucun cablage SD
  // n'est ajoute ici.
  bool hasSeparateLogsFs = false;
  String logsFsLabel = "SD (logs GPS)";
  uint64_t logsFsUsedBytes = 0;
  uint64_t logsFsTotalBytes = 0;
};
typedef WebServerStatusInfo (*WebServerGetStatusFn)();

class WebServerManager {
public:
  // A appeler une fois dans setup(). sessionLogPath = chemin du carnet
  // de session cumulatif (ex "/sessions.csv"), utilise pour batir le
  // resume (nb de tours, meilleur temps) affiche a cote de chaque
  // session GPS detaillee sur la page de gestion. circuitsFilePath =
  // chemin du fichier de circuits (ex "/circuits.csv"), gere entierement
  // cote WebServerManager (page /circuits) -- main.cpp le relit de son
  // cote au demarrage pour construire CourseManager (cf.
  // loadActiveCircuitsIntoTracks() dans main.cpp), meme principe de
  // decouplage que pour /sessions.csv.
  void begin(const char* apSsid, const char* sessionLogPath, const char* circuitsFilePath, fs::FS& logsFs,
             WebServerBleStopFn bleStop, WebServerBleRestartFn bleRestart,
             WebServerFlushLogsFn flushLogs, WebServerGetStatusFn getStatus);

  // Active le point d'acces + le serveur HTTP (coupe le BLE via le
  // callback fourni a begin()).
  void startDownloadMode();

  // Coupe le point d'acces + le serveur HTTP (relance le BLE).
  void stopDownloadMode();

  // A appeler a chaque tour de loop() -- traite les requetes HTTP
  // entrantes quand le mode telechargement est actif (no-op sinon).
  void loop();

  bool isActive() const { return _active; }
  String getSsid() const { return _apSsid; }
  String getIp() const; // valide seulement si isActive()

private:
  const char* _apSsid = nullptr;
  const char* _sessionLogPath = nullptr;
  WebServerBleStopFn _bleStop = nullptr;
  WebServerBleRestartFn _bleRestart = nullptr;
  WebServerFlushLogsFn _flushLogs = nullptr;
  bool _active = false;
};

extern WebServerManager webServerManager;
