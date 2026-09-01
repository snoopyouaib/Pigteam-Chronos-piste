/**
 * Chrono GPS moto piste -- firmware ESP32-S3-AMOLED-2.41 (firmware_241)
 * ----------------------------------------------------------------------
 * Portage complet (14/08) du firmware reel chrono-AMOLED (1.91") vers le
 * module ESP32-S3-Touch-AMOLED-2.41 (SKU 30589, RM690B0 600x450, FT6336,
 * revision materielle V2). Distinct de display_only_241, qui reste un
 * banc de test ecran pur sans GPS/SD/batterie/WiFi reels.
 * GPS reel (GpsManager), detection de circuit/tour reelle (CourseManager,
 * vendore depuis la lib publique DovesLapTimer), sessions reelles
 * (/sessions.csv, LittleFS), batterie reelle (ADC interne, GPIO17/ADC2 --
 * PAS la meme broche que le 1.91), stockage SD reel (SdLogStorage,
 * SDMMC), WebServerManager (WiFi/telechargement) integre.
 * Ecrans reagences pour 600x450 (repris du banc display_only_241),
 * details complets du portage dans README.md.
 *
 * Ecran + bouton PUSH simple (rotatif EC11 retire le 27/07 sur le 1.91,
 * remplace par un bouton poussoir simple sur la meme broche -- cf.
 * commentaire pres de PUSH_BUTTON) + bouton BACK + bouton PWR (GPIO15,
 * specifique au 2.41 -- extinction propre par appui long, cf. README.md).
 *
 * Navigation (pas de menu liste separe) :
 *   - Depuis Statut : BACK -> ecran Circuit (1er ecran de l'anneau)
 *   - Anneau Circuit -> Nouveau circuit -> Connexion -> Session -> Reglages
 *     -> Circuit... (swipe tactile uniquement depuis le retrait du rotatif)
 *   - Sur Circuit/Session/Reglages : PUSH entre en mode selection,
 *     re-PUSH valide -- ou tap direct sur une ligne (equivalent en un
 *     seul geste, et desormais le moyen normal vu l'absence de rotation).
 *   - BACK depuis un ecran de l'anneau (hors mode selection) -> Statut.
 *   - BACK en mode selection -> annule, sort du mode selection (reste
 *     sur le meme ecran, anneau).
 *   - Session -> PUSH/tap ouvre les tours de la session (ecran feuille),
 *     BACK y revient a la liste des sessions.
 *   - Reglages -> PUSH/tap ouvre WiFi (ecran feuille, encore factice),
 *     BACK y revient directement a Statut.
 *   - Statut : PUSH ou tap sur le bouton REC demarre l'enregistrement
 *     (si circuit detecte) ; BACK l'arrete (PUSH desarme pendant le REC,
 *     par securite).
 */

#include <Arduino.h>
#include <lvgl.h>
#include <limits.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <LittleFS.h>
#include <CourseManager.h>

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "expander_bsp.h"
#include "display_bsp.h"
#include "fonts_teko.h"
#include "splash_pigteam.h"
#include "GpsManager.h"
#include "adc_bsp.h"
#include "SdLogStorage.h"
#include "ImuManager.h"
#include "WebServerManager.h"

// ===================== Pins (valides au bring-up 1.91) =====================
// Rotatif physiquement retire (27/07) -- remplace par un bouton poussoir
// simple sur la meme broche (ENCODER_CLK/DT ne sont plus cables). La
// navigation anneau/listes, auparavant pilotee par la rotation, repose
// desormais entierement sur le tactile (swipe pour l'anneau, tap direct
// pour les listes -- deja fonctionnel en parallele avant ce changement,
// cf. ringScreenGestureCb() et les commentaires "Tap: choisir").
// 2.41 : GPIO16/GPIO2 du 1.91 sont pris ailleurs sur ce board (16 =
// BAT_Control, 2 = SD_CS) -- repris sur GPIO18/GPIO8, valides au banc
// display_only_241 (libres, sans fonction de strapping).
#define PUSH_BUTTON   18
#define BACK_BUTTON   8

volatile bool backButtonPressed = false;
static unsigned long lastBackIsrMs = 0;
// Porte a 500ms (28/07, etait 200ms) : rebond/bruit constate sur BACK
// juste apres le retrait du rotatif -- un vrai double-appui volontaire ne
// se produit jamais en moins d'une demi-seconde, donc aucune perte de
// reactivite percue. A verifier aussi cote materiel (soudure/connecteur
// deranges en manipulant a cote pendant le remplacement du rotatif).
static const unsigned long BACK_DEBOUNCE_MS = 500;

volatile bool pushButtonPressed = false;
static unsigned long lastPushIsrMs = 0;
static const unsigned long PUSH_DEBOUNCE_MS = 200;

// BACK maintenu pendant l'enregistrement = arret definitif (05/08,
// remplace l'ancien systeme PUSH=pause + ecran de confirmation
// PUSH=reprendre/BACK=arreter -- source de confusion reelle constatee
// sur le terrain, cf. section README correspondante : plusieurs
// redemarrages parasites le 06/08 en cherchant a arreter, l'utilisateur
// appuyant sur PUSH en pensant confirmer l'arret alors que PUSH
// relancait). En course comme en track day le chrono tourne du paddock
// au paddock sans pause -- plus besoin de ce mode intermediaire.
//
// Le maintien protege contre un appui accidentel : BACK est le bouton
// le plus court des deux (choix delibere, plus dur a actionner par
// inadvertance qu'un bouton plus long) et n'a plus d'ecran de
// confirmation derriere lui pour rattraper une erreur -- d'ou un delai
// plus long (1400ms sur le 2.41, double du 700ms initial du 1.91 --
// demande explicite du 14/08 pour securiser encore plus l'arret) que
// celui utilise auparavant pour la simple navigation vers Circuit
// (300ms, qui avait un filet de securite derriere). Un simple contact
// parasite (vibration) ressemble a un front descendant isole, pas a un
// contact ferme en continu sur 1400ms -- largement en dessous du temps
// de reaction humain pour un vrai appui volontaire, meme allonge.
static const unsigned long BACK_HOLD_STOP_MS = 1400; // double du delai initial (700ms, cf. 1.91) -- securise l'arret, moins de risque de coupure accidentelle
// PWR (GPIO15) -- extinction propre par appui long, ajoute le 14/08
// maintenant qu'une vraie batterie est disponible pour tester. Plus
// long que BACK_HOLD_STOP_MS pour eviter une extinction accidentelle
// en manipulant le chrono. Sans effet sur USB (l'alimentation USB
// maintient la carte independamment de GPIO16) -- utile seulement sur
// batterie, ce qui est le comportement voulu.
#define PWR_BUTTON 15
static const unsigned long PWR_HOLD_OFF_MS = 2000;
static unsigned long pwrHoldSinceMs = 0; // 0 = pas d'appui PWR en cours
static unsigned long backHoldStopPendingSinceMs = 0; // 0 = pas d'appui BACK en attente de confirmation

// Recalcul periodique du debit RMC reel (07/08, suite au constat que
// gps_hz restait fige a la valeur du boot pendant tout le trajet de
// 900km, cf. section README correspondante). Base sur
// gpsRmcSentenceCount (simple delta / temps ecoule), jamais de lecture
// bloquante du port GPS -- n'entre jamais en concurrence avec le
// parsing reel des trames.
static uint32_t lastRmcCountSnapshot = 0;
static unsigned long lastRmcHzUpdateMs = 0;
static const unsigned long RMC_HZ_UPDATE_INTERVAL_MS = 10000UL;

void IRAM_ATTR backButtonISR() {
  unsigned long now = millis();
  if (now - lastBackIsrMs < BACK_DEBOUNCE_MS) return;
  lastBackIsrMs = now;
  backButtonPressed = true;
}
void IRAM_ATTR pushButtonISR() {
  unsigned long now = millis();
  if (now - lastPushIsrMs < PUSH_DEBOUNCE_MS) return;
  lastPushIsrMs = now;
  pushButtonPressed = true;
}

// mm:ss.mmm
static void formatLapTime(unsigned long ms, char* buf, size_t bufSize) {
  if (ms == 0) { snprintf(buf, bufSize, "--:--.---"); return; }
  unsigned long totalMinutes = ms / 60000;
  unsigned long seconds = (ms / 1000) % 60;
  unsigned long millisPart = ms % 1000;
  if (totalMinutes >= 60) {
    // Bascule H:MM:SS.mmm au-dela d'une heure -- ajoute le 07/08 suite
    // au trajet de 900km, ou le chrono Route affichait "244:59.439" au
    // lieu de repasser en heures. N'affecte jamais les vrais temps au
    // tour (toujours tres en dessous de 60 minutes en usage normal),
    // seulement le compteur cumulatif du mode Route sur trajet long.
    unsigned long hours = totalMinutes / 60;
    unsigned long minutes = totalMinutes % 60;
    snprintf(buf, bufSize, "%lu:%02lu:%02lu.%03lu", hours, minutes, seconds, millisPart);
  } else {
    snprintf(buf, bufSize, "%lu:%02lu.%03lu", totalMinutes, seconds, millisPart);
  }
}

// ===================== Cache GPS local (fixStatus/numSVs/vitesse) =====================
//
// Copies locales de liveData, mises a jour a chaque tour de loop() --
// utilisees par l'affichage (evite de repeter liveData.xxx partout,
// et garde un point unique si on veut lisser/lisser plus tard).
static int gpsFixStatus = 0;
static int gpsNumSVs = 0;
static float gpsSpeedKmh = 0;

static void gpsUpdateFromLiveData() {
  gpsFixStatus = liveData.fixStatus;
  gpsNumSVs = liveData.numSVs;
  gpsSpeedKmh = liveData.speedMmPerS * 3.6f / 1000.0f; // mm/s -> km/h
}

// ===================== Utilitaires date/heure (GPS UTC -> heure locale) =====================
//
// Repris a l'identique du firmware TFT reel -- utcTmToEpoch() est une
// implementation portable (algorithme "days_from_civil" de Howard
// Hinnant) plutot que mktime()/timegm(), qui dependent de la TZ systeme
// et donneraient un resultat faux ici (TZ deja postionnee sur l'heure
// locale via tzset(), pas UTC).
static time_t utcTmToEpoch(const struct tm& tmUtc) {
  int year = tmUtc.tm_year + 1900;
  int month = tmUtc.tm_mon + 1;
  int day = tmUtc.tm_mday;
  int64_t y = year - (month <= 2 ? 1 : 0);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return (time_t)(days * 86400 + tmUtc.tm_hour * 3600 + tmUtc.tm_min * 60 + tmUtc.tm_sec);
}

static void getLocalDateTime(char* dateBuf, size_t dateBufSize, char* timeBuf, size_t timeBufSize) {
  struct tm utcTm = {};
  utcTm.tm_year = liveData.year - 1900;
  utcTm.tm_mon  = liveData.month - 1;
  utcTm.tm_mday = liveData.day;
  utcTm.tm_hour = liveData.hour;
  utcTm.tm_min  = liveData.minute;
  utcTm.tm_sec  = liveData.second;
  time_t epoch = utcTmToEpoch(utcTm);
  struct tm localTm;
  localtime_r(&epoch, &localTm);
  if (dateBuf) snprintf(dateBuf, dateBufSize, "%04d-%02d-%02d", localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday);
  if (timeBuf) snprintf(timeBuf, timeBufSize, "%02d:%02d:%02d", localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
}

static void getLocalDateTimeCompact(char* buf, size_t bufSize) {
  struct tm utcTm = {};
  utcTm.tm_year = liveData.year - 1900;
  utcTm.tm_mon  = liveData.month - 1;
  utcTm.tm_mday = liveData.day;
  utcTm.tm_hour = liveData.hour;
  utcTm.tm_min  = liveData.minute;
  utcTm.tm_sec  = liveData.second;
  time_t epoch = utcTmToEpoch(utcTm);
  struct tm localTm;
  localtime_r(&epoch, &localTm);
  snprintf(buf, bufSize, "%04d%02d%02d_%02d%02d%02d",
           localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
           localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
}

// Inverse de formatLapTime() -- "M:SS.mmm" ou "H:MM:SS.mmm" (07/08,
// cf. formatLapTime() -- trajets Route > 1h) -> millisecondes.
// ULONG_MAX si le format ne correspond a aucun des deux (ex.
// "--:--.---" = pas de temps).
static unsigned long parseLapTimeStr(const String& s) {
  int firstColon = s.indexOf(':');
  int dot = s.indexOf('.');
  if (firstColon < 0 || dot < 0) return ULONG_MAX;
  int secondColon = s.indexOf(':', firstColon + 1);
  long hours = 0, minutes, seconds, millisPart;
  if (secondColon >= 0 && secondColon < dot) {
    hours = s.substring(0, firstColon).toInt();
    minutes = s.substring(firstColon + 1, secondColon).toInt();
    seconds = s.substring(secondColon + 1, dot).toInt();
  } else {
    minutes = s.substring(0, firstColon).toInt();
    seconds = s.substring(firstColon + 1, dot).toInt();
  }
  millisPart = s.substring(dot + 1).toInt();
  if (hours == 0 && minutes == 0 && seconds == 0 && millisPart == 0 && s.charAt(0) != '0') return ULONG_MAX;
  return (unsigned long)(hours * 3600000UL + minutes * 60000 + seconds * 1000 + millisPart);
}

// ===================== CourseManager (detection auto / mode proximite) =====================
//
// Port direct de la logique du firmware TFT reel (main.cpp du projet
// pigteam-chrono-tft) -- geofencing, mode manuel, capture de nouveau
// circuit compris. Cf. ce fichier pour le detail des commentaires
// d'origine (repris ici de facon condensee).

static const char* CIRCUITS_FILE_PATH = "/circuits.csv";
static const char* SESSION_LOG_PATH = "/sessions.csv";
static char courseNameBuf[MAX_COURSES][32];
static TrackConfig myTracks = { "Mes circuits PIGTEAM", "PIGTEAM", {}, 0 };
static CourseManager* courseManager = nullptr;
static int lastLapCount = 0;

// Fige l'affichage du chrono sur le temps du tour qui vient de se
// terminer pendant lapFreezeS secondes apres le passage de ligne, au
// lieu de repartir instantanement a 0 -- laisse le temps de lire le
// tour avant que le compteur du tour suivant prenne sa place. Remis a
// jour a CHAQUE tour termine (checkLapCompletion()), donc un tour plus
// rapide que ce delai ecourte naturellement le gel precedent au profit
// du nouveau temps. N'affecte que l'affichage : le chrono/la detection
// sous-jacents (CourseManager/DovesLapTimer) continuent normalement,
// rien n'est retarde cote enregistrement.
//
// Reglable depuis l'ecran Reglages (tap sur la ligne -- cycle parmi
// FREEZE_PRESETS_S), persiste sur LittleFS (SETTINGS_FILE_PATH) pour
// survivre a un redemarrage.
static unsigned long lapFreezeUntilMs = 0;
static int lapFreezeS = 10;
static const int FREEZE_PRESETS_S[] = { 0, 5, 10, 15, 20, 30 };
static const int FREEZE_PRESETS_COUNT = 6;
// Logs diagnostic (diag_*.csv, une ligne toutes les DIAG_WRITE_INTERVAL_MS)
// desactives par defaut -- ecriture SD non essentielle au fonctionnement
// (contrairement au log GPS detaille, log_*.csv, toujours actif), mais
// qui use la carte sur la duree. Persiste comme lapFreezeS, ajoute le 14/08.
static bool diagLogsEnabled = false;
static const char* SETTINGS_FILE_PATH = "/settings.csv";

static void saveDisplaySettings() {
  File f = LittleFS.open(SETTINGS_FILE_PATH, "w");
  if (!f) { Serial.println("Reglages : echec ecriture /settings.csv"); return; }
  f.printf("lapFreezeS,%d\n", lapFreezeS);
  f.printf("diagLogsEnabled,%d\n", diagLogsEnabled ? 1 : 0);
  f.close();
}

static void loadDisplaySettings() {
  if (!LittleFS.exists(SETTINGS_FILE_PATH)) return; // 1er boot -- garde les valeurs par defaut
  File f = LittleFS.open(SETTINGS_FILE_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("lapFreezeS,")) {
      int v = line.substring(11).toInt();
      // Ignore toute valeur hors des presets connus (fichier corrompu/edite a la main).
      for (int i = 0; i < FREEZE_PRESETS_COUNT; i++) {
        if (FREEZE_PRESETS_S[i] == v) { lapFreezeS = v; break; }
      }
    } else if (line.startsWith("diagLogsEnabled,")) {
      diagLogsEnabled = (line.substring(16).toInt() != 0);
    }
  }
  f.close();
}

static int splitCsvLine(const String& line, String fields[], int maxFields) {
  int count = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && count < maxFields; i++) {
    if (i == (int)line.length() || line[i] == ',') {
      fields[count++] = line.substring(start, i);
      start = i + 1;
    }
  }
  return count;
}

static void loadActiveCircuitsIntoTracks() {
  if (!LittleFS.exists(CIRCUITS_FILE_PATH)) {
    Serial.println("Circuits: /circuits.csv absent -- demarrage en mode proximite uniquement.");
    myTracks.courseCount = 0;
    return;
  }
  File f = LittleFS.open(CIRCUITS_FILE_PATH, "r");
  if (!f) {
    Serial.println("Circuits: impossible d'ouvrir /circuits.csv -- mode proximite uniquement.");
    myTracks.courseCount = 0;
    return;
  }

  int loaded = 0;
  bool firstLine = true;
  while (f.available() && loaded < MAX_COURSES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) { firstLine = false; continue; }

    String fld[17];
    if (splitCsvLine(line, fld, 17) < 17) continue;
    if (fld[0].toInt() != 1) continue;

    CourseConfig& c = myTracks.courses[loaded];
    strncpy(courseNameBuf[loaded], fld[1].c_str(), sizeof(courseNameBuf[loaded]) - 1);
    courseNameBuf[loaded][sizeof(courseNameBuf[loaded]) - 1] = '\0';
    c.name = courseNameBuf[loaded];
    c.lengthFt = fld[2].toFloat();
    c.startALat = atof(fld[3].c_str());  c.startALng = atof(fld[4].c_str());
    c.startBLat = atof(fld[5].c_str());  c.startBLng = atof(fld[6].c_str());
    c.hasSector2 = fld[7].toInt() == 1;
    c.sector2ALat = atof(fld[8].c_str());  c.sector2ALng = atof(fld[9].c_str());
    c.sector2BLat = atof(fld[10].c_str()); c.sector2BLng = atof(fld[11].c_str());
    c.hasSector3 = fld[12].toInt() == 1;
    c.sector3ALat = atof(fld[13].c_str()); c.sector3ALng = atof(fld[14].c_str());
    c.sector3BLat = atof(fld[15].c_str()); c.sector3BLng = atof(fld[16].c_str());
    loaded++;
  }
  f.close();
  myTracks.courseCount = loaded;
  Serial.printf("Circuits: %d circuit(s) actif(s) charge(s) depuis circuits.csv.\n", loaded);
}

// ----- Mode manuel (force un circuit precis, court-circuite la detection) -----
static DovesLapTimer manualTimer(7.0, &Serial);
static bool manualOverrideActive = false;
static int manualCourseIndex = -1;

static bool newCircuitCaptureArmed = false;
static bool pendingNewCircuitCapture = false;
static double pendingSaLat = 0, pendingSaLng = 0, pendingSbLat = 0, pendingSbLng = 0;
static bool newCircuitAutoSaved = false;
static double prevWaypointLat = 0, prevWaypointLng = 0;

static void activateManualCourse(int index) {
  if (index < 0 || index >= myTracks.courseCount) return;
  CourseConfig& c = myTracks.courses[index];

  manualTimer.reset();
  manualTimer.setStartFinishLine(c.startALat, c.startALng, c.startBLat, c.startBLng);
  if (c.hasSector2) manualTimer.setSector2Line(c.sector2ALat, c.sector2ALng, c.sector2BLat, c.sector2BLng);
  if (c.hasSector3) manualTimer.setSector3Line(c.sector3ALat, c.sector3ALng, c.sector3BLat, c.sector3BLng);

  manualOverrideActive = true;
  manualCourseIndex = index;
  lastLapCount = 0;
  newCircuitCaptureArmed = false;
  pendingNewCircuitCapture = false;
  Serial.printf("Circuit force : %s\n", c.name);
}

static bool geofenceCheckDone = false;

static void activateAutoMode() {
  manualOverrideActive = false;
  manualCourseIndex = -1;
  courseManager->reset();
  geofenceCheckDone = false;
  lastLapCount = 0;
  newCircuitCaptureArmed = false;
  pendingNewCircuitCapture = false;
  Serial.println("Retour en mode detection automatique.");
}

// ----- Geofencing (reconnaissance quasi instantanee sur circuit connu) -----
static const float GEOFENCE_MAX_DISTANCE_M = 15000.0f;

static void checkCircuitGeofence(double lat, double lng) {
  geofenceCheckDone = true;
  int bestIdx = -1;
  double bestDist = 1e18;
  for (int i = 0; i < myTracks.courseCount; i++) {
    CourseConfig& c = myTracks.courses[i];
    double midLat = (c.startALat + c.startBLat) / 2.0;
    double midLng = (c.startALng + c.startBLng) / 2.0;
    double d = geoHaversine(lat, lng, midLat, midLng);
    if (d < bestDist) { bestDist = d; bestIdx = i; }
  }
  if (bestIdx >= 0 && bestDist <= GEOFENCE_MAX_DISTANCE_M) {
    Serial.printf("Geofencing : a %.1fkm de \"%s\" -- activation directe.\n", bestDist / 1000.0, myTracks.courses[bestIdx].name);
    activateManualCourse(bestIdx);
  } else {
    Serial.println("Geofencing : aucun circuit actif a moins de 15km -- detection normale par tour+longueur.");
  }
}

static bool lapAnythingEffective() {
  return !manualOverrideActive && courseManager->isLapAnythingActive();
}

// ----- Mode Route : parcours libre sans detection de tour (balade a
// velo, trajet en voiture...) -- juste GPS/vitesse/distance/IMU
// enregistres en continu, sans notion de ligne de depart-arrivee ni de
// tour. PAS PERSISTE (contrairement a lapFreezeS) : revient toujours a
// false au demarrage -- mode explicite a reactiver a chaque usage, par
// securite (pas de risque d'oublier le chrono "bloque" en mode Route
// apres une balade). Toggle par tap sur l'ecran Reglages. Declare ici
// (avant sa 1ere utilisation, detectionEffectivelyComplete() juste en
// dessous) plutot que pres de selectingMode -- erreur de compilation
// "not declared in this scope" sinon (C++ n'autorise pas l'usage avant
// declaration, contrairement aux fonctions qui peuvent etre appelees
// avant leur definition tant qu'elles sont declarees quelque part).
static bool routeMode = false;
static unsigned long routeRecStartMs = 0;

static bool detectionEffectivelyComplete() {
  return routeMode || manualOverrideActive || courseManager->isDetectionComplete();
}

// ----- Historique GPS court terme (calcul de cap pour la capture de nouveau circuit) -----
struct GpsHistoryPoint { double lat; double lng; unsigned long timeMs; };
static const int GPS_HISTORY_SIZE = 40;
static GpsHistoryPoint gpsHistory[GPS_HISTORY_SIZE];
static int gpsHistoryCount = 0;
static int gpsHistoryHead = 0;

static void pushGpsHistory(double lat, double lng, unsigned long timeMs) {
  gpsHistory[gpsHistoryHead] = { lat, lng, timeMs };
  gpsHistoryHead = (gpsHistoryHead + 1) % GPS_HISTORY_SIZE;
  if (gpsHistoryCount < GPS_HISTORY_SIZE) gpsHistoryCount++;
}

static bool findHistoryPointBefore(unsigned long nowMs, unsigned long windowMs, double& outLat, double& outLng) {
  if (gpsHistoryCount == 0) return false;
  int best = (gpsHistoryHead - 1 + GPS_HISTORY_SIZE) % GPS_HISTORY_SIZE;
  for (int i = 0; i < gpsHistoryCount; i++) {
    int j = (gpsHistoryHead - 1 - i + GPS_HISTORY_SIZE) % GPS_HISTORY_SIZE;
    best = j;
    if (nowMs - gpsHistory[j].timeMs >= windowMs) break;
  }
  outLat = gpsHistory[best].lat;
  outLng = gpsHistory[best].lng;
  return true;
}

static double geoBearingDeg(double lat1, double lng1, double lat2, double lng2) {
  double phi1 = radians(lat1), phi2 = radians(lat2);
  double dlambda = radians(lng2 - lng1);
  double y = sin(dlambda) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlambda);
  return fmod(degrees(atan2(y, x)) + 360.0, 360.0);
}

// Distance a vol d'oiseau entre 2 points GPS (formule de haversine) --
// utilisee pour cumuler la distance parcourue pendant un tour (cf.
// currentLapDistanceM dans logGpsRow()/checkLapCompletion()). Reprend
// exactement le meme calcul que haversineM() cote JS (WebServerManager
// .cpp, page /lap) -- desormais fait ici, une fois par trame, plutot que
// re-parcouru par le navigateur a chaque affichage de page.
static double geoDistanceM(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371000.0;
  double dLat = radians(lat2 - lat1), dLng = radians(lng2 - lng1);
  double a = sin(dLat / 2) * sin(dLat / 2) + cos(radians(lat1)) * cos(radians(lat2)) * sin(dLng / 2) * sin(dLng / 2);
  return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static void geoDestinationPoint(double lat, double lng, double bearingDeg, double distanceM, double& outLat, double& outLng) {
  const double R = 6371000.0;
  double delta = distanceM / R;
  double theta = radians(bearingDeg);
  double phi1 = radians(lat);
  double lambda1 = radians(lng);
  double phi2 = asin(sin(phi1) * cos(delta) + cos(phi1) * sin(delta) * cos(theta));
  double lambda2 = lambda1 + atan2(sin(theta) * sin(delta) * cos(phi1), cos(delta) - sin(phi1) * sin(phi2));
  outLat = degrees(phi2);
  outLng = fmod(degrees(lambda2) + 540.0, 360.0) - 180.0;
}

// ----- Capture automatique de nouveau circuit -----
static const double NEW_CIRCUIT_LINE_OFFSET_M = 4.0;

static void armNewCircuitCapture() {
  manualOverrideActive = false;
  manualCourseIndex = -1;
  courseManager->reset();
  geofenceCheckDone = true; // suspend le geofencing pendant la capture
  lastLapCount = 0;
  newCircuitCaptureArmed = true;
  newCircuitAutoSaved = false;
  pendingNewCircuitCapture = false;
  prevWaypointLat = 0; prevWaypointLng = 0;
  Serial.println("Capture de nouveau circuit armee -- roule un tour complet pour l'enregistrer.");
}

static void cancelNewCircuitCapture() {
  if (!newCircuitCaptureArmed) return;
  if (newCircuitAutoSaved) {
    Serial.println("BACK : capture deja ecrite dans circuits.csv -- rien a annuler ici.");
    return;
  }
  newCircuitCaptureArmed = false;
  pendingNewCircuitCapture = false;
  prevWaypointLat = 0; prevWaypointLng = 0;
  Serial.println("BACK : capture de nouveau circuit annulee.");
}

static void appendAutoCircuitToFile(const char* name, float lengthFt) {
  File f = LittleFS.open(CIRCUITS_FILE_PATH, "a");
  if (!f) { Serial.println("Capture circuit : impossible d'ouvrir circuits.csv en ecriture."); return; }
  f.printf("0,%s,%.1f,%.7f,%.7f,%.7f,%.7f,0,0,0,0,0,0,0,0,0,0,0\n",
           name, lengthFt, pendingSaLat, pendingSaLng, pendingSbLat, pendingSbLng);
  f.close();
  Serial.printf("Nouveau circuit capture et sauvegarde (inactif) : %s\n", name);
}

static void checkAutoCircuitCapture(unsigned long timeMs) {
  if (!newCircuitCaptureArmed || manualOverrideActive || newCircuitAutoSaved) return;

  WaypointLapTimer* t = courseManager->getLapAnythingTimer();
  double wpLat = t->getWaypointLat();
  double wpLng = t->getWaypointLng();

  if (!pendingNewCircuitCapture && (wpLat != 0.0 || wpLng != 0.0) &&
      prevWaypointLat == 0.0 && prevWaypointLng == 0.0) {
    double histLat, histLng;
    if (findHistoryPointBefore(timeMs, 2000, histLat, histLng)) {
      double heading = geoBearingDeg(histLat, histLng, wpLat, wpLng);
      double perp = fmod(heading + 90.0, 360.0);
      geoDestinationPoint(wpLat, wpLng, perp, NEW_CIRCUIT_LINE_OFFSET_M, pendingSaLat, pendingSaLng);
      geoDestinationPoint(wpLat, wpLng, perp + 180.0, NEW_CIRCUIT_LINE_OFFSET_M, pendingSbLat, pendingSbLng);
      pendingNewCircuitCapture = true;
      Serial.println("Capture circuit : point de reference repere, ligne A/B estimee -- confirmation au premier tour valide.");
    }
  }
  prevWaypointLat = wpLat;
  prevWaypointLng = wpLng;

  if (pendingNewCircuitCapture && t->getRaceStarted() && lapAnythingEffective()) {
    char compactDateTime[16];
    getLocalDateTimeCompact(compactDateTime, sizeof(compactDateTime));
    char name[32];
    snprintf(name, sizeof(name), "Nouveau_%s", compactDateTime);
    float lengthFt = t->getLastLapDistance() * 3.28084f;
    appendAutoCircuitToFile(name, lengthFt);
    newCircuitAutoSaved = true;
    pendingNewCircuitCapture = false;
  }
}

// ----- Traitement d'un fix GPS -----
static void processGpsFix(double lat, double lng, float altM, float speedKnots, unsigned long timeMs) {
  if (liveData.fixStatus < 2) return;
  pushGpsHistory(lat, lng, timeMs);
  if (routeMode) return; // pas de detection de tour en mode Route -- juste GPS/vitesse/distance (logGpsRow()) accumules en continu

  if (!geofenceCheckDone && !manualOverrideActive) {
    checkCircuitGeofence(lat, lng);
  }

  if (manualOverrideActive) {
    manualTimer.updateCurrentTime(timeMs);
    manualTimer.loop(lat, lng, altM, speedKnots);
  } else {
    courseManager->updateCurrentTime(timeMs);
    courseManager->loop(lat, lng, altM, speedKnots);
    checkAutoCircuitCapture(timeMs);
  }
}

// ----- Etat d'affichage courant (tour en cours/meilleur/nb de tours) -----
static void getDisplayState(unsigned long& currentLapMs, unsigned long& bestLapMs, bool& hasBest, int& lapsCount) {
  currentLapMs = 0; bestLapMs = 0; hasBest = false; lapsCount = 0;
  if (routeMode) {
    // Pas de garde recordingEnabled ici (declaree plus loin dans le
    // fichier, "not declared in this scope" sinon) -- sans risque, cette
    // valeur n'est utilisee par l'appelant que si un REC est en cours.
    currentLapMs = millis() - routeRecStartMs;
    return; // mode Route : pas de notion de tour/meilleur, juste la duree ecoulee depuis le depart
  }
  if (manualOverrideActive) {
    if (manualTimer.getRaceStarted()) currentLapMs = manualTimer.getCurrentLapTime();
    hasBest = manualTimer.getBestLapNumber() > 0;
    bestLapMs = manualTimer.getBestLapTime();
    lapsCount = manualTimer.getLaps();
  } else if (lapAnythingEffective()) {
    WaypointLapTimer* t = courseManager->getLapAnythingTimer();
    if (t->getRaceStarted()) currentLapMs = t->getCurrentLapTime();
    hasBest = t->getBestLapNumber() > 0;
    bestLapMs = t->getBestLapTime();
    lapsCount = t->getLaps();
  } else if (DovesLapTimer* t = courseManager->getActiveTimer()) {
    if (t->getRaceStarted()) currentLapMs = t->getCurrentLapTime();
    hasBest = t->getBestLapNumber() > 0;
    bestLapMs = t->getBestLapTime();
    lapsCount = t->getLaps();
  }
}

static unsigned long getLastFinishedLapMs() {
  if (manualOverrideActive) return manualTimer.getLastLapTime();
  if (lapAnythingEffective()) return courseManager->getLapAnythingTimer()->getLastLapTime();
  if (DovesLapTimer* t = courseManager->getActiveTimer()) return t->getLastLapTime();
  return 0;
}

static const char* getActiveCourseNameForDisplay() {
  if (routeMode) return "Route";
  if (newCircuitCaptureArmed) return newCircuitAutoSaved ? "Circuit capture !" : "CAPTURE NEW TRACK";
  if (manualOverrideActive) return myTracks.courses[manualCourseIndex].name;
  if (!detectionEffectivelyComplete()) return "Detection...";
  if (lapAnythingEffective()) return "Inconnu (proximite)";
  return courseManager->getActiveCourseName();
}

// ===================== Sessions (carnet /sessions.csv, LittleFS) =====================

struct SessionSummary {
  String compactKey;
  int lapCount;
  unsigned long bestLapMs;
};
struct LapDetail {
  int lapNumber;
  unsigned long lapMs;
  String circuit;
  float maxSpeedKmh; // 0 si absent (anciennes sessions enregistrees avant ce champ)
};

static std::vector<SessionSummary> loadSessionSummaries() {
  std::vector<SessionSummary> result;
  File f = LittleFS.open(SESSION_LOG_PATH, "r");
  if (!f) return result;

  bool inSession = false;
  SessionSummary cur;
  static const char* MARK_START = "# session demarree ";
  static const char* MARK_STOP = "# session arretee";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith(MARK_START)) {
      if (inSession) result.push_back(cur);
      cur = SessionSummary();
      cur.compactKey = line.substring(strlen(MARK_START));
      cur.lapCount = 0;
      cur.bestLapMs = ULONG_MAX;
      inSession = true;
      continue;
    }
    if (line.startsWith(MARK_STOP)) {
      if (inSession) { result.push_back(cur); inSession = false; }
      continue;
    }
    if (!inSession) continue;

    String fld[6];
    if (splitCsvLine(line, fld, 6) < 6) continue;
    cur.lapCount++;
    unsigned long lapMs = parseLapTimeStr(fld[3]);
    if (lapMs != ULONG_MAX && lapMs < cur.bestLapMs) cur.bestLapMs = lapMs;
  }
  if (inSession) result.push_back(cur);
  f.close();
  return result;
}

static std::vector<LapDetail> loadLapsForSession(const String& compactKey) {
  std::vector<LapDetail> result;
  File f = LittleFS.open(SESSION_LOG_PATH, "r");
  if (!f) return result;

  bool inTarget = false;
  static const char* MARK_START = "# session demarree ";
  static const char* MARK_STOP = "# session arretee";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith(MARK_START)) {
      inTarget = (line.substring(strlen(MARK_START)) == compactKey);
      continue;
    }
    if (line.startsWith(MARK_STOP)) {
      if (inTarget) break;
      continue;
    }
    if (!inTarget) continue;

    String fld[7];
    if (splitCsvLine(line, fld, 7) < 6) continue;
    LapDetail lap;
    lap.lapNumber = fld[2].toInt();
    lap.lapMs = parseLapTimeStr(fld[3]);
    lap.circuit = fld[5];
    lap.maxSpeedKmh = fld[6].length() > 0 ? fld[6].toFloat() : 0.0f; // absent sur les sessions enregistrees avant ce champ
    result.push_back(lap);
  }
  f.close();
  return result;
}

// "AAAAMMJJ_HHMMSS" -> "JJ/MM HH:MM"
static String formatCompactKeyShort(const String& key) {
  if (key.length() < 15) return key;
  return key.substring(6, 8) + "/" + key.substring(4, 6) + " " + key.substring(9, 11) + ":" + key.substring(11, 13);
}

// ===================== Enregistrement (REC) =====================

static File logFile;
static bool loggingOk = false;
static char currentLogPath[40] = "";

// Log diagnostic periodique -- ajoute le 06/08 en vue du trajet longue
// duree (900km A/R) : fichier separe et leger (1 ligne / 30s, pas a
// 10Hz comme le log GPS) pour voir l'evolution dans le temps de la
// batterie, temperature CPU, debit GPS reel, tas libre -- des infos
// qu'on ne peut normalement observer qu'en direct sur l'ecran
// Connexion ou via le serial, aucun des deux disponible pendant un
// long trajet sans s'arreter. Ouvert/ferme avec l'enregistrement,
// meme cycle de vie que logFile -- pas de nouveau mode a activer.
static File diagFile;
static bool diagLoggingOk = false;
static char currentDiagLogPath[40] = "";
static unsigned long lastDiagWriteMs = 0;
static const unsigned long DIAG_WRITE_INTERVAL_MS = 30000UL;
static char currentSessionCompactKey[16] = "";
static bool recordingEnabled = false;
static float currentLapMaxSpeedKmh = 0.0f; // remise a 0 a chaque tour termine (checkLapCompletion)

// ----- Detection wheelie/stoppie (roue avant/arriere levee) -----
//
// Pas de calcul d'angle de tangage continu ici (contrairement au
// roulis, cf. ImuManager) : sous forte acceleration/freinage,
// l'accelerometre confond acceleration lineaire et gravite bien plus
// violemment qu'en virage -- un angle calcule serait denue de sens
// pendant exactement les evenements qu'on veut detecter. Approche plus
// robuste et standard en telemetrie moto : detection par SEUIL sur la
// vitesse de tangage brute (gyro Y, cf. mapping d'axe dans
// ImuManager.cpp -- roulis=gyroZ, tangage=gyroY, lacet=gyroX), avec
// hysteresis pour ne compter qu'UNE fois un evenement soutenu (pas a
// chaque tick pendant qu'il dure).
//
// ATTENTION -- seuil/duree NON valides sur piste (contrairement au
// roulis, testable a la main sur un etabli, un wheelie/stoppie ne se
// simule pas vraiment hors roulage reel -- il faut la dynamique
// acceleration+levee de roue). Valeurs de depart a affiner apres un
// premier roulage selon les faux positifs/negatifs constates
// (vibrations moteur/route vs vrai levage de roue). Sens confirme par
// test reel le 29/07 (mouvement de bascule capture en cours de geste,
// pas a l'arret) : gyroY negatif = roue avant qui se leve (wheelie,
// -110.75 dps observe), gyroY positif = roue arriere qui se leve
// (stoppie, +390.53 dps observe).
static const float WHEELIE_GYRO_THRESHOLD_DPS = 45.0f;      // a affiner sur piste
static const unsigned long WHEELIE_MIN_DURATION_MS = 250UL; // filtre les a-coups/vibrations, pas un vrai levage
static unsigned long wheelieStartMs = 0, stoppieStartMs = 0;
static bool wheelieCounted = false, stoppieCounted = false;
static int currentLapWheelieCount = 0, currentLapStoppieCount = 0;

static void checkWheelieStoppie() {
  if (!recordingEnabled || !imuOk) return;
  float ax, ay, az, gx, gy, gz;
  getImuRaw(ax, ay, az, gx, gy, gz);
  unsigned long nowMs = millis();

  // Sens confirme par test reel (29/07, mouvement captures en cours de
  // bascule -- pas a l'arret) : roue avant qui se leve (wheelie) ->
  // gyroY negatif (-110.75 dps observe) ; roue arriere qui se leve
  // (stoppie) -> gyroY positif (+390.53 dps observe). Inverse de
  // l'hypothese de depart, corrige ici.
  if (gy < -WHEELIE_GYRO_THRESHOLD_DPS) {
    if (wheelieStartMs == 0) wheelieStartMs = nowMs;
    if (!wheelieCounted && nowMs - wheelieStartMs >= WHEELIE_MIN_DURATION_MS) {
      currentLapWheelieCount++;
      wheelieCounted = true;
      Serial.println("Wheelie detecte (estime -- seuil/duree encore a affiner sur piste)");
    }
  } else {
    wheelieStartMs = 0;
    wheelieCounted = false;
  }

  if (gy > WHEELIE_GYRO_THRESHOLD_DPS) {
    if (stoppieStartMs == 0) stoppieStartMs = nowMs;
    if (!stoppieCounted && nowMs - stoppieStartMs >= WHEELIE_MIN_DURATION_MS) {
      currentLapStoppieCount++;
      stoppieCounted = true;
      Serial.println("Stoppie detecte (estime -- seuil/duree encore a affiner sur piste)");
    }
  } else {
    stoppieStartMs = 0;
    stoppieCounted = false;
  }
}
static float currentLapMinSpeedKmh = -1.0f;   // -1 = pas encore de mesure sur ce tour
static double currentLapSpeedSumKmh = 0.0;    // pour la moyenne -- somme/echantillons
static unsigned long currentLapSpeedSamples = 0;
static double currentLapDistanceM = 0.0;      // cumul haversine entre trames successives
static bool currentLapHasPrevPoint = false;   // faux juste apres reinit -- evite un saut errone depuis le point du tour precedent
static double currentLapPrevLat = 0.0, currentLapPrevLng = 0.0;
static char currentLapStartTimeStr[10] = "--:--:--"; // capturee sur la 1ere trame de chaque tour (cf. logGpsRow())
static unsigned long lastLapMsSeen = 0xFFFFFFFFUL; // detecte le plateau/redemarrage de current_lap_ms (cf. logGpsRow())
static void finalizeRouteSessionIfNeeded(); // definie plus bas -- forward-declaree pour etre appelable depuis stopRecording() (cf. fix ordre d'ecriture, 29/07)
static int batteryVoltageToPercent(float v); // definie plus bas -- declaration anticipee, utilisee par logDiagRow() et getStatusCallback() avant sa definition
// Angle max a droite/a gauche par tour -- leanAngleDeg positif = droite,
// negatif = gauche (convention confirmee au banc le 29/07, cf.
// ImuManager.cpp). On stocke l'angle a droite comme un max simple, et
// l'angle a gauche comme un min (le plus negatif), affiche ensuite en
// valeur absolue -- 0.0f de depart convient dans les deux cas (aucun
// angle ne peut etre "moins penche" que 0 au repos).
static float currentLapMaxAngleRightDeg = 0.0f;
static float currentLapMaxAngleLeftDeg = 0.0f; // le plus negatif observe (0 = pas encore penche a gauche)

static void appendSessionLine(const String& line) {
  File f = LittleFS.open(SESSION_LOG_PATH, "a");
  if (!f) { Serial.println("Carnet de session: impossible d'ecrire /sessions.csv."); return; }
  f.println(line);
  f.close();
}

static void startRecording() {
  if (recordingEnabled) return;
  if (!liveData.year) {
    Serial.println("REC refuse : pas encore de fix GPS (horodatage necessaire).");
    return;
  }
  getLocalDateTimeCompact(currentSessionCompactKey, sizeof(currentSessionCompactKey));
  snprintf(currentLogPath, sizeof(currentLogPath), "/log_%s.csv", currentSessionCompactKey);

  logFile = gpsLogFs->open(currentLogPath, "w");
  if (!logFile) {
    Serial.printf("REC : impossible de creer %s\n", currentLogPath);
    return;
  }
  logFile.println("local_time,millis_boot,lat,lng,speed_kmh,fix,sats,laps,circuit,current_lap_ms,lean_angle_deg,accel_x_mg,accel_y_mg,accel_z_mg,gyro_x_dps,gyro_y_dps,gyro_z_dps");
  logFile.flush();
  loggingOk = true;

  snprintf(currentDiagLogPath, sizeof(currentDiagLogPath), "/diag_%s.csv", currentSessionCompactKey);
  if (!diagLogsEnabled) {
    Serial.println("REC : logs diagnostic desactives (reglage) -- pas de diag_*.csv pour cette session.");
  } else {
    diagFile = gpsLogFs->open(currentDiagLogPath, "w");
    if (diagFile) {
      diagFile.println("local_time,uptime_s,batt_pct,batt_v,cpu_temp_c,free_heap,gps_hz,gps_fix,gps_sats,laps,detection_rejections");
      diagFile.flush();
      diagLoggingOk = true;
      lastDiagWriteMs = 0; // force une premiere ligne rapidement plutot que d'attendre 30s
    } else {
      Serial.printf("REC : impossible de creer %s (log diag desactive pour cette session)\n", currentDiagLogPath);
    }
  }

  recordingEnabled = true;
  routeRecStartMs = millis(); // n'est utilise que si routeMode est actif, inoffensif sinon
  lastLapCount = 0;
  lapFreezeUntilMs = 0;
  currentLapMaxSpeedKmh = 0.0f;
  currentLapMinSpeedKmh = -1.0f;
  currentLapSpeedSumKmh = 0.0;
  currentLapSpeedSamples = 0;
  currentLapDistanceM = 0.0;
  currentLapHasPrevPoint = false;
  currentLapWheelieCount = 0;
  currentLapStoppieCount = 0;
  wheelieStartMs = 0; stoppieStartMs = 0;
  wheelieCounted = false; stoppieCounted = false;
  currentLapMaxAngleRightDeg = 0.0f;
  currentLapMaxAngleLeftDeg = 0.0f;
  lastLapMsSeen = 0xFFFFFFFFUL; // force la capture du depart des le tout 1er point de la session
  strncpy(currentLapStartTimeStr, "--:--:--", sizeof(currentLapStartTimeStr));
  appendSessionLine(String("# session demarree ") + currentSessionCompactKey);
  Serial.printf("REC ON : %s\n", currentLogPath);
}

static void stopRecording() {
  if (!recordingEnabled) return;
  if (loggingOk) { logFile.flush(); logFile.close(); loggingOk = false; }
  if (diagLoggingOk) { diagFile.flush(); diagFile.close(); diagLoggingOk = false; }
  // IMPORTANT : appele ICI, avant "# session arretee" -- une version
  // precedente l'appelait seulement au moment de la confirmation d'arret
  // definitif (ancien ecran SCR_CONFIRM_STOP, retire le 05/08 -- cf.
  // section README correspondante). Consequence reelle constatee a
  // l'epoque : la ligne "Route" atterrissait APRES le marqueur
  // "# session arretee" deja ecrit, donc EN DEHORS du bloc demarree/arretee
  // -- loadSessionSummaries() l'ignorait completement (current==nullptr des
  // ce marqueur vu), rendant le resume Route invisible sur la page web
  // (carte d'accueil ET lien /lap absents, lapCount reste a 0).
  finalizeRouteSessionIfNeeded(); // no-op si routeMode est false
  appendSessionLine("# session arretee");
  recordingEnabled = false;
  Serial.printf("REC OFF : %s ferme.\n", currentLogPath);
}

static void logGpsRow() {
  if (!loggingOk) return;
  char timeBuf[10];
  getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));
  double lat = liveData.latitude / 1e7;
  double lng = liveData.longitude / 1e7;
  float speedKmh = liveData.speedMmPerS / 1000.0f * 3.6f;

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);

  // Capture generale du depart : 1ere trame accumulee depuis le dernier
  // reset (par checkLapCompletion() a la fin du tour precedent, ou par
  // le plateau ci-dessous pour le tour 1) -- couvre TOUS les tours,
  // pas seulement le 1er. Sans cette ligne (oubliee dans la version
  // precedente en limitant la capture au seul cas lapsCount==0 du
  // plateau), le depart des tours 2, 3, etc. restait bloque sur le
  // placeholder "--:--:--" ecrit par checkLapCompletion() -- confirme
  // sur log reel du 29/07 (tour 2 sans depart).
  if (currentLapSpeedSamples == 0) strncpy(currentLapStartTimeStr, timeBuf, sizeof(currentLapStartTimeStr));

  // current_lap_ms qui stagne (plateau -- observe sur le terrain le
  // 28/07 : reste a 0 pendant 27s, le temps que le geofence s'arme,
  // avant le vrai depart du 1er tour) : tant qu'AUCUN tour n'est encore
  // valide (lapsCount==0) ET que current_lap_ms ne progresse pas, on
  // considere que le tour n'a pas vraiment commence et on continue de
  // glisser le depart/les accumulateurs en avant. IMPORTANT : strictement
  // limite a lapsCount==0 -- une premiere version comparait juste
  // current_lap_ms sans cette garde, et se redeclenchait a CHAQUE
  // transition entre tours (le compteur du tour suivant peut deja
  // apparaitre dans getDisplayState() avant que checkLapCompletion() ait
  // pu lire/ecrire les stats du tour qui vient de se terminer -- verifie
  // sur le log reel du 28/07, ou le tour 2 demarre direct a 1182ms sans
  // jamais repasser par 0), ce qui aurait efface les stats de TOUS les
  // tours suivants au lieu de corriger juste le 1er. Les transitions
  // normales entre tours restent gerees par la reinit en fin de
  // checkLapCompletion(), inchangee.
  if (lapsCount == 0 && currentLapMs <= lastLapMsSeen) {
    currentLapMaxSpeedKmh = 0.0f;
    currentLapMinSpeedKmh = -1.0f;
    currentLapSpeedSumKmh = 0.0;
    currentLapSpeedSamples = 0;
    currentLapDistanceM = 0.0;
    currentLapHasPrevPoint = false;
    currentLapWheelieCount = 0;
    currentLapStoppieCount = 0;
    wheelieStartMs = 0; stoppieStartMs = 0;
    wheelieCounted = false; stoppieCounted = false;
    currentLapMaxAngleRightDeg = 0.0f;
    currentLapMaxAngleLeftDeg = 0.0f;
    strncpy(currentLapStartTimeStr, timeBuf, sizeof(currentLapStartTimeStr));
  }
  lastLapMsSeen = currentLapMs;

  if (speedKmh > currentLapMaxSpeedKmh) currentLapMaxSpeedKmh = speedKmh;
  if (currentLapMinSpeedKmh < 0 || speedKmh < currentLapMinSpeedKmh) currentLapMinSpeedKmh = speedKmh;
  currentLapSpeedSumKmh += speedKmh;
  currentLapSpeedSamples++;
  if (currentLapHasPrevPoint) currentLapDistanceM += geoDistanceM(currentLapPrevLat, currentLapPrevLng, lat, lng);
  currentLapPrevLat = lat; currentLapPrevLng = lng; currentLapHasPrevPoint = true;

  float accelXmg, accelYmg, accelZmg, gyroXdps, gyroYdps, gyroZdps;
  getImuRaw(accelXmg, accelYmg, accelZmg, gyroXdps, gyroYdps, gyroZdps);
  float leanAngleDeg = getLeanAngleDeg();
  if (leanAngleDeg > currentLapMaxAngleRightDeg) currentLapMaxAngleRightDeg = leanAngleDeg;   // positif = droite
  if (leanAngleDeg < currentLapMaxAngleLeftDeg) currentLapMaxAngleLeftDeg = leanAngleDeg;     // negatif = gauche (le plus negatif = le plus penche)

  // 7 champs IMU ajoutes EN FIN de ligne (angle d'inclinaison + valeurs
  // brutes accelero/gyro), meme principe de retrocompatibilite que pour
  // les champs deja ajoutes a /sessions.csv : les lecteurs existants qui
  // ne lisent que les 10 premiers champs continuent de fonctionner sans
  // modification sur les logs enregistres avant cet ajout. Valeurs a 0
  // si l'IMU n'a pas ete detectee au demarrage (cf. imuOk).
  logFile.printf("%s,%lu,%.7f,%.7f,%.1f,%u,%u,%d,%s,%lu,%.2f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f\n",
                 timeBuf, millis(), lat, lng, speedKmh,
                 gpsFixStatus, gpsNumSVs, lapsCount,
                 getActiveCourseNameForDisplay(), currentLapMs,
                 leanAngleDeg, accelXmg, accelYmg, accelZmg, gyroXdps, gyroYdps, gyroZdps);

  static unsigned long lastFlush = 0;
  if (millis() - lastFlush >= 5000) { lastFlush = millis(); logFile.flush(); }
}

// Ligne de diagnostic systeme periodique -- separee du log GPS (cf.
// DIAG_WRITE_INTERVAL_MS et commentaire pres de currentDiagLogPath).
// Appelee depuis loop() independamment de newGpsData, sur une cadence
// horloge murale plutot que calee sur les fixes GPS -- reste utile meme
// si le GPS a un souci, ce qui est justement une partie de ce qu'on
// veut pouvoir observer apres coup.
static void logDiagRow() {
  if (!diagLoggingOk) return;
  if (millis() - lastDiagWriteMs < DIAG_WRITE_INTERVAL_MS) return;
  lastDiagWriteMs = millis();

  char timeBuf[10];
  getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));

  float battV = 0; int battRaw = 0;
  adc_get_value(&battV, &battRaw);
  int battPct = batteryVoltageToPercent(battV);

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);

  int rejections = courseManager ? courseManager->getDetectionRejectionCount() : 0;

  diagFile.printf("%s,%lu,%d,%.2f,%.0f,%u,%.1f,%u,%u,%d,%d\n",
                  timeBuf, millis() / 1000UL, battPct, battV, temperatureRead(),
                  (unsigned)ESP.getFreeHeap(), gpsMeasuredRmcHz, gpsFixStatus, gpsNumSVs,
                  lapsCount, rejections);
  diagFile.flush(); // 1 ligne / 30s -- cout d'un flush systematique negligeable ici, contrairement au log GPS a 10Hz
}

static void checkLapCompletion() {
  if (!recordingEnabled) return;
  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);
  if (lapsCount <= lastLapCount) return;
  lastLapCount = lapsCount;
  lapFreezeUntilMs = millis() + (unsigned long)lapFreezeS * 1000UL;

  char dateBuf[12], timeBuf[10];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  char lapBuf[16], bestBuf[16];
  formatLapTime(getLastFinishedLapMs(), lapBuf, sizeof(lapBuf));
  formatLapTime(hasBest ? bestLapMs : 0, bestBuf, sizeof(bestBuf));

  float avgSpeedKmh = (currentLapSpeedSamples > 0) ? (float)(currentLapSpeedSumKmh / currentLapSpeedSamples) : 0.0f;
  float minSpeedKmh = (currentLapMinSpeedKmh < 0) ? 0.0f : currentLapMinSpeedKmh;
  float distanceKm = (float)(currentLapDistanceM / 1000.0);

  // 15 champs desormais (etait 6, puis 7 avec Vmax, puis 11 avec
  // Vmin/Vmoy/distance/depart, puis 13 avec wheelie/stoppie) -- toujours
  // ajoutes EN FIN de ligne, jamais en milieu : les lecteurs existants
  // (page /lap, ecran physique) qui ne lisent que les premiers champs
  // continuent de fonctionner sans modification sur les anciennes ET
  // les nouvelles sessions (retrocompatibilite, meme principe que pour
  // Vmax cf. README). Angle a droite/gauche max en valeur absolue (le
  // signe -- droite positif, gauche negatif -- ne sert qu'en interne
  // pour l'accumulation, cf. currentLapMaxAngleRightDeg/LeftDeg) ; 0.0
  // pour les deux si l'IMU n'a pas ete detectee.
  char line[248];
  snprintf(line, sizeof(line), "%s,%s,%d,%s,%s,%s,%.0f,%.0f,%.0f,%.2f,%s,%d,%d,%.0f,%.0f",
           dateBuf, timeBuf, lapsCount, lapBuf, bestBuf, getActiveCourseNameForDisplay(),
           currentLapMaxSpeedKmh, minSpeedKmh, avgSpeedKmh, distanceKm, currentLapStartTimeStr,
           currentLapWheelieCount, currentLapStoppieCount,
           currentLapMaxAngleRightDeg, fabsf(currentLapMaxAngleLeftDeg));
  appendSessionLine(line);
  Serial.printf("Tour %d enregistre : %s (meilleur : %s, Vmax %.0f, Vmin %.0f, Vmoy %.0f km/h, %.2f km, depart %s, %d wheelie(s), %d stoppie(s), angle D%.0f/G%.0f)\n",
                lapsCount, lapBuf, bestBuf, currentLapMaxSpeedKmh, minSpeedKmh, avgSpeedKmh, distanceKm, currentLapStartTimeStr,
                currentLapWheelieCount, currentLapStoppieCount,
                currentLapMaxAngleRightDeg, fabsf(currentLapMaxAngleLeftDeg));

  // Reinitialisation pour le tour suivant -- filet de securite : en
  // temps normal, logGpsRow() se reinitialise deja tout seul sur la
  // trame suivante (currentLapMs qui redemarre a 0, cf. plus haut), ceci
  // couvre juste le cas ou aucune trame ne suit avant un arret REC.
  currentLapMaxSpeedKmh = 0.0f;
  currentLapMinSpeedKmh = -1.0f;
  currentLapSpeedSumKmh = 0.0;
  currentLapSpeedSamples = 0;
  currentLapDistanceM = 0.0;
  currentLapHasPrevPoint = false;
  strncpy(currentLapStartTimeStr, "--:--:--", sizeof(currentLapStartTimeStr));
  currentLapWheelieCount = 0;
  currentLapStoppieCount = 0;
  currentLapMaxAngleRightDeg = 0.0f;
  currentLapMaxAngleLeftDeg = 0.0f;
  wheelieStartMs = 0; stoppieStartMs = 0;
  wheelieCounted = false; stoppieCounted = false;
}

// Ecrit une ligne "Route" dans /sessions.csv a l'arret definitif --
// meme format/mêmes accumulateurs que checkLapCompletion() (Vmax/Vmin/
// Vmoy/distance/depart/wheelie/stoppie/angles), qui se sont accumules
// en continu sur TOUTE la duree puisque lapsCount reste a 0 en mode
// Route (cf. getDisplayState()/processGpsFix() plus haut) -- jamais
// remis a zero par checkLapCompletion(), qui ne se declenche jamais
// faute de tour detecte. lap_number fixe a 1 (un seul "tour" = le
// parcours entier), best_lap_time = lap_time (pas de notion de
// meilleur sur un parcours unique).
static void finalizeRouteSessionIfNeeded() {
  if (!routeMode || currentLapSpeedSamples == 0) return; // rien a ecrire si jamais demarre ou aucune trame recue

  char dateBuf[12], timeBuf[10];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  char lapBuf[16];
  formatLapTime(millis() - routeRecStartMs, lapBuf, sizeof(lapBuf));

  float avgSpeedKmh = (currentLapSpeedSamples > 0) ? (float)(currentLapSpeedSumKmh / currentLapSpeedSamples) : 0.0f;
  float minSpeedKmh = (currentLapMinSpeedKmh < 0) ? 0.0f : currentLapMinSpeedKmh;
  float distanceKm = (float)(currentLapDistanceM / 1000.0);

  char line[248];
  snprintf(line, sizeof(line), "%s,%s,%d,%s,%s,%s,%.0f,%.0f,%.0f,%.2f,%s,%d,%d,%.0f,%.0f",
           dateBuf, timeBuf, 1, lapBuf, lapBuf, "Route",
           currentLapMaxSpeedKmh, minSpeedKmh, avgSpeedKmh, distanceKm, currentLapStartTimeStr,
           currentLapWheelieCount, currentLapStoppieCount,
           currentLapMaxAngleRightDeg, fabsf(currentLapMaxAngleLeftDeg));
  appendSessionLine(line);
  Serial.printf("Route enregistree : %s (Vmax %.0f, Vmin %.0f, Vmoy %.0f km/h, %.2f km, depart %s)\n",
               lapBuf, currentLapMaxSpeedKmh, minSpeedKmh, avgSpeedKmh, distanceKm, currentLapStartTimeStr);
}

// ===================== WebServerManager (WiFi/telechargement) =====================
//
// Le module ne connait rien du GPS/CourseManager -- il recoit des
// callbacks fournis ici, meme principe de decouplage que le TFT/OLED.
// bleStop/bleRestart passes a nullptr (pas de BLE sur ce board).
static void flushLogsCallback() {
  if (loggingOk) logFile.flush();
}

static WebServerStatusInfo getStatusCallback() {
  WebServerStatusInfo s;
  s.bleConnected = gpsActive; // champ reutilise tel quel -- represente "GPS actif" ici
  s.fixStatus = liveData.fixStatus;
  s.numSats = liveData.numSVs;
  s.circuitName = getActiveCourseNameForDisplay();
  s.recordingEnabled = recordingEnabled;

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);
  s.lapsCount = lapsCount;
  char lapBuf[16], bestBuf[16];
  formatLapTime(getLastFinishedLapMs(), lapBuf, sizeof(lapBuf));
  formatLapTime(hasBest ? bestLapMs : 0, bestBuf, sizeof(bestBuf));
  s.lastLapTime = lapBuf;
  s.bestLapTime = bestBuf;

  s.hasGpsFix = liveData.fixStatus >= 2;
  s.latitude = liveData.latitude / 1e7;
  s.longitude = liveData.longitude / 1e7;

  if (gpsLogsOnSd) {
    s.hasSeparateLogsFs = true;
    s.logsFsLabel = "SD (logs GPS detailles)";
    s.logsFsUsedBytes = sdUsedBytes();
    s.logsFsTotalBytes = sdTotalBytes();
  }

  // Diagnostics ajoutes le 05/08 -- memes infos que l'ecran Connexion,
  // dupliquees ici pour un acces a distance sans etre devant l'appareil
  // (cf. WebServerStatusInfo).
  s.gpsRmcHz = gpsMeasuredRmcHz;
  s.gpsFixRateAckOk = gpsFixRateAckOk;
  s.detectionRejectionCount = courseManager ? courseManager->getDetectionRejectionCount() : 0;
  float battV = 0; int battRaw = 0;
  adc_get_value(&battV, &battRaw);
  s.battVoltage = battV;
  s.battPercent = batteryVoltageToPercent(battV);
  s.cpuTempC = temperatureRead();

  return s;
}

// ===================== Batterie (ADC interne, deja valide au bring-up 1.91) =====================

static int batteryVoltageToPercent(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.0f)  return 0;
  struct { float v; int pct; } curve[] = {
    {4.15f, 100}, {4.0f, 85}, {3.85f, 65}, {3.7f, 45},
    {3.55f, 25}, {3.4f, 10}, {3.2f, 3}, {3.0f, 0}
  };
  for (int i = 0; i < 7; i++) {
    if (v >= curve[i+1].v) {
      float t = (v - curve[i+1].v) / (curve[i].v - curve[i+1].v);
      return curve[i+1].pct + (int)(t * (curve[i].pct - curve[i+1].pct));
    }
  }
  return 0;
}

static int readBatteryPercent() {
  float v; int raw;
  adc_get_value(&v, &raw);
  return batteryVoltageToPercent(v);
}

// ===================== Commandes Serial (reprises du firmware TFT) =====================
//
// l : liste les logs GPS detailles (gpsLogFs -- SD si montee, sinon LittleFS)
// d : dump du log en cours (ou du dernier ferme) sur le port Serial
// s : dump du carnet de sessions (/sessions.csv, LittleFS)
// c : efface tous les logs GPS detailles (arrete le REC en cours au besoin)
// b : tension/pourcentage batterie
//
// Utile pour verifier/nettoyer sans passer par le WiFi -- pratique sur
// le banc, avec juste le cable USB deja branche pour le moniteur.
template<typename Fn>
static void forEachGpsLogFile(Fn callback) {
  File root = gpsLogFs->open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.indexOf("/log_") >= 0 && name.endsWith(".csv")) callback(name, (uint32_t)f.size());
    f.close();
    f = root.openNextFile();
  }
  root.close();
}

// Meme principe, filtre sur /diag_ au lieu de /log_ -- logs diagnostic
// periodiques (cf. section README "Log diagnostic periodique", 06/08).
// Copie separee plutot que parametree sur le prefixe : deux fonctions
// courtes et lisibles valent mieux qu'un parametre supplementaire pour
// un usage aussi ponctuel (commande serial 'g' uniquement).
template<typename Fn>
static void forEachDiagLogFile(Fn callback) {
  File root = gpsLogFs->open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.indexOf("/diag_") >= 0 && name.endsWith(".csv")) callback(name, (uint32_t)f.size());
    f.close();
    f = root.openNextFile();
  }
  root.close();
}

static void printSerialCommandsHelp() {
  Serial.println("=== Commandes Serial disponibles (tape la lettre, pas besoin d'Entree) ===");
  Serial.println("  l : liste les logs GPS detailles (SD ou LittleFS)");
  Serial.println("  d : dump du log en cours/dernier ferme");
  Serial.println("  s : dump du carnet de sessions (/sessions.csv)");
  Serial.println("  c : efface tous les logs GPS detailles");
  Serial.println("  g : efface tous les logs diagnostic (/diag_*.csv) -- ne touche ni aux logs GPS ni au carnet de sessions");
  Serial.println("  x : efface le carnet de sessions (/sessions.csv)");
  Serial.println("  b : tension/pourcentage batterie");
  Serial.println("  i : angle d'inclinaison + valeurs brutes IMU (accelero/gyro)");
  Serial.println("  w : flux gyroY en continu 3s -- pour identifier le signe exact d'un wheelie/stoppie");
  Serial.println("  ? ou h : reaffiche cette aide");
}

static void handleSerialCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c == 'b') {
    float v; int raw;
    adc_get_value(&v, &raw);
    Serial.printf("Batterie : %.3fV (%d%%)\n", v, batteryVoltageToPercent(v));

  } else if (c == 'l') {
    Serial.println("=== Logs GPS detailles ===");
    int count = 0;
    forEachGpsLogFile([&](const String& name, uint32_t size) {
      Serial.printf("  %s (%u octets)\n", name.c_str(), (unsigned)size);
      count++;
    });
    if (count == 0) Serial.println("  (aucun)");

  } else if (c == 'd') {
    if (loggingOk) logFile.flush();
    if (strlen(currentLogPath) == 0) {
      Serial.println("Aucun log enregistre pour l'instant.");
    } else {
      Serial.printf("=== DEBUT DUMP %s ===\n", currentLogPath);
      File f = gpsLogFs->open(currentLogPath, "r");
      while (f && f.available()) Serial.write(f.read());
      f.close();
      Serial.printf("=== FIN DUMP %s ===\n", currentLogPath);
    }

  } else if (c == 's') {
    Serial.println("=== DEBUT CARNET DE SESSION ===");
    File f = LittleFS.open(SESSION_LOG_PATH, "r");
    while (f && f.available()) Serial.write(f.read());
    f.close();
    Serial.println("=== FIN CARNET DE SESSION ===");

  } else if (c == 'c') {
    if (recordingEnabled) stopRecording();
    String toDelete[20];
    int count = 0;
    forEachGpsLogFile([&](const String& name, uint32_t size) {
      if (count < 20) toDelete[count++] = name;
    });
    for (int i = 0; i < count; i++) gpsLogFs->remove(toDelete[i]);
    currentLogPath[0] = '\0';
    Serial.printf("%d fichier(s) de log GPS efface(s).\n", count);

  } else if (c == 'g') {
    // Efface uniquement /diag_*.csv -- ne touche ni aux logs GPS
    // detailles (log_*.csv, cf. 'c') ni au carnet de sessions cumulatif
    // (/sessions.csv, cf. 'x'), fichiers completement independants sur
    // le filesystem (aucun des deux ne referencie les noms de fichiers
    // diag_*.csv, contrairement au carnet qui liste les log_*.csv).
    if (recordingEnabled) stopRecording(); // ferme aussi le diagFile en cours, cf. stopRecording()
    String toDelete[20];
    int count = 0;
    forEachDiagLogFile([&](const String& name, uint32_t size) {
      if (count < 20) toDelete[count++] = name;
    });
    for (int i = 0; i < count; i++) gpsLogFs->remove(toDelete[i]);
    currentDiagLogPath[0] = '\0';
    Serial.printf("%d fichier(s) de log diagnostic efface(s).\n", count);

  } else if (c == 'x') {
    // Repris de la variante OLED (absent du TFT) -- efface le carnet
    // cumulatif, PAS les logs GPS detailles (cf. 'c' pour ca).
    if (recordingEnabled) stopRecording();
    if (LittleFS.remove(SESSION_LOG_PATH)) {
      Serial.println("Carnet de session (/sessions.csv) efface.");
    } else {
      Serial.println("Carnet de session deja absent ou erreur d'effacement.");
    }
    lastLapCount = 0;

  } else if (c == 'i') {
    float ax, ay, az, gx, gy, gz;
    getImuRaw(ax, ay, az, gx, gy, gz);
    Serial.printf("IMU : %s -- angle roulis %.1f deg\n", imuOk ? "OK" : "absente/en panne", getLeanAngleDeg());
    Serial.printf("  Accel (mg) : X=%.1f Y=%.1f Z=%.1f\n", ax, ay, az);
    Serial.printf("  Gyro (dps) : X=%.2f Y=%.2f Z=%.2f\n", gx, gy, gz);
    if (imuOk) Serial.println("  -- carte a plat : ~1000mg sur un seul axe accelero, gyro proche de 0 attendus au repos.");

  } else if (c == 'w') {
    // Flux continu au lieu d'un instantane -- une lecture ponctuelle
    // ('i') tape a la main ne peut pas viser precisement le pic d'un
    // mouvement rapide comme un wheelie/stoppie (constate le 29/07 :
    // deux tests donnent des signes opposes selon l'instant exact de la
    // capture -- montee du mouvement vs rebond/oscillation qui suit).
    // Ici, on imprime gyroY en continu pendant 2s pour voir toute la
    // courbe et identifier le signe du VRAI pic sans ambiguite de
    // timing.
    if (!imuOk) {
      Serial.println("IMU absente/en panne -- rien a tester.");
    } else {
      Serial.println("Bascule la carte maintenant (wheelie ou stoppie) -- flux 3s :");
      unsigned long startMs = millis();
      while (millis() - startMs < 3000) {
        imuTick(); // sinon getImuRaw() renverrait les valeurs figees d'avant la boucle (loop() est bloque ici)
        float ax, ay, az, gx, gy, gz;
        getImuRaw(ax, ay, az, gx, gy, gz);
        Serial.printf("  t+%4lums  gyroY=%7.1f dps  (accelZ=%6.1f mg)\n", millis() - startMs, gy, az);
        delay(15); // imuTick() se limite deja a ~50Hz (20ms) en interne
      }
      Serial.println("Fin du flux -- note le signe au moment ou |gyroY| est maximal (c'est le vrai pic du mouvement).");
    }

  } else if (c == '?' || c == 'h') {
    printSerialCommandsHelp();
  }
}

// ===================== Navigation =====================

enum AppScreen { SCR_STATUS, SCR_CIRCUIT, SCR_CONNEXION, SCR_SESSION_LIST, SCR_SESSION_LAPS, SCR_SETTINGS, SCR_WIFI, SCR_NEW_CIRCUIT };
static AppScreen currentScreen = SCR_STATUS;
// Idem, mais cote ecran Statut : une fois l'arret DEFINITIF confirme, le
// geofencing (rayon 15km, cf. GEOFENCE_MAX_DISTANCE_M) peut redetecter le
// circuit et rearmer le bouton REC en ~1s si on est pres de la piste --
// constate au banc (cf. log Serial du 27/07 : "Retour en mode detection
// automatique" suivi de "REC ON" en une poignee de lignes). Cette fenetre
// protege le REC de l'ecran Statut juste apres un arret definitif.
static unsigned long lastDefinitiveStopMs = 0;
static const unsigned long STATUS_REC_GRACE_AFTER_STOP_MS = 1500UL;
static bool selectingMode = false; // true = mode "selection dans la liste" (Circuit/Session/Reglages), arme par PUSH

// Batterie trop faible pour demarrer un enregistrement -- evite de
// couper l'alimentation en plein tour (perte de log en cours) et
// d'user une batterie deja quasi vide. Affichage "NO BAT" a la place de
// "PRESS REC", PUSH ignore sur l'ecran Statut tant que c'est sous ce
// seuil (mais n'arrete pas un REC deja en cours -- cf. handlePush()).
static const int LOW_BATT_NO_REC_PERCENT = 5;

static int ringIndex = 0;        // 0=Circuit,1=NouveauCircuit,2=Connexion,3=Session,4=Reglages
static int circuitSelection = 0; // 0=Auto, 1..N=courses (plus de "Nouveau circuit" ici, ecran dedie desormais)
static int sessionListSelection = 0;
static int sessionLapSelection = 0;

static lv_obj_t* scrStatus;
static lv_obj_t* scrCircuit;
static lv_obj_t* scrConnexion;
static lv_obj_t* scrSessionList;
static lv_obj_t* scrSessionLaps;
static lv_obj_t* scrSettings;
static lv_obj_t* scrWifi;
static lv_obj_t* scrNewCircuit;

static bool screenDirty = true; // force un redessin complet au prochain refresh

// WiFi.softAP()/stopDownloadMode() sont lents (radio) -- ne JAMAIS les
// appeler directement depuis un callback tactile LVGL (deja execute
// avec le mutex LVGL tenu par la tache de rendu -- les bloquer dessus
// gele tout l'affichage/tactile pendant leur duree, terrain favorable
// aux plantages). On se contente ici de positionner un flag ; le vrai
// appel se fait dans loop(), hors de toute section verrouillee.
static bool wifiStartRequested = false;
static bool wifiStopRequested = false;

static void setEncoderForRing() {
  // No-op depuis le retrait du rotatif (27/07) -- gardee pour ne pas
  // toucher aux appels dans goToScreen(), l'anneau se navigue au swipe.
}

static void goToScreen(AppScreen s) {
  currentScreen = s;
  selectingMode = false;
  screenDirty = true;
  switch (s) {
    case SCR_STATUS:       lv_scr_load(scrStatus); break;
    case SCR_CIRCUIT:      ringIndex = 0; lv_scr_load(scrCircuit); setEncoderForRing(); break;
    case SCR_NEW_CIRCUIT:  ringIndex = 1; lv_scr_load(scrNewCircuit); setEncoderForRing(); break;
    case SCR_CONNEXION:    ringIndex = 2; lv_scr_load(scrConnexion); setEncoderForRing(); break;
    case SCR_SESSION_LIST: ringIndex = 3; lv_scr_load(scrSessionList); setEncoderForRing(); break;
    case SCR_SETTINGS:     ringIndex = 4; lv_scr_load(scrSettings); setEncoderForRing(); break;
    case SCR_SESSION_LAPS: lv_scr_load(scrSessionLaps); break;
    case SCR_WIFI:         lv_scr_load(scrWifi); break;
  }
}

static void goToRingScreen(int idx) {
  // idx: 0=Circuit,1=NouveauCircuit,2=Connexion,3=Session,4=Reglages --
  // appele au swipe pour changer d'ecran dans l'anneau (rotatif retire)
  // (hors mode selection).
  switch (idx) {
    case 0: goToScreen(SCR_CIRCUIT); break;
    case 1: goToScreen(SCR_NEW_CIRCUIT); break;
    case 2: goToScreen(SCR_CONNEXION); break;
    case 3: goToScreen(SCR_SESSION_LIST); break;
    default: goToScreen(SCR_SETTINGS); break;
  }
}

// Glissement gauche/droite sur un ecran de l'anneau -- change d'ecran,
// seul mecanisme de navigation de l'anneau depuis le retrait du rotatif. Ignore en mode selection
// (meme logique que la rotation : pas de changement d'ecran quand on
// est en train de choisir un element dans une liste).
static void ringScreenGestureCb(lv_event_t* e) {
  if (selectingMode) return;
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT) {
    goToRingScreen((ringIndex + 1) % 5);
  } else if (dir == LV_DIR_RIGHT) {
    goToRingScreen((ringIndex + 4) % 5); // -1 mod 5
  }
}

// A appeler sur chacun des 4 ecrans de l'anneau juste apres leur
// creation, pour activer le swipe.
static void enableRingSwipe(lv_obj_t* scr) {
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE); // sinon le glissement fait defiler l'ecran au lieu de generer un geste
  lv_obj_add_event_cb(scr, ringScreenGestureCb, LV_EVENT_GESTURE, NULL);
}

// ===================== Ecran Statut (LVGL) -- inchange, cf. sessions precedentes =====================

static lv_obj_t* lblGps;
static lv_obj_t* lblRec;
static lv_obj_t* lblBatt;
static lv_obj_t* lblBig;
static lv_obj_t* lblClock;
static lv_obj_t* lblRouteChrono; // mode Route uniquement -- meme taille/style que lblClock, en face (BOTTOM_RIGHT)
static lv_obj_t* lblRecHint; // "PRESS REC", cf. commentaire pres de sa creation
static lv_obj_t* lblDernier;
static lv_obj_t* lblBest;
static lv_obj_t* lblTours;

static void buildStatusScreen() {
  scrStatus = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrStatus, lv_color_black(), 0);
  lv_obj_clear_flag(scrStatus, LV_OBJ_FLAG_SCROLLABLE); // sinon un depassement de contenu (ex: bouton REC) rend l'ecran defilant

  lblGps = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblGps, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lblGps, lv_color_white(), 0);
  lv_obj_align(lblGps, LV_ALIGN_TOP_LEFT, 4, 0);

  lblRec = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblRec, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lblRec, lv_palette_main(LV_PALETTE_RED), 0);
  lv_label_set_text(lblRec, "REC");
  lv_obj_align(lblRec, LV_ALIGN_TOP_RIGHT, -60, 0);

  lblBatt = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblBatt, &lv_font_teko_medium_26, 0);
  lv_obj_align(lblBatt, LV_ALIGN_TOP_RIGHT, -4, 0);

  lblBig = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblBig, &lv_font_teko_bold_110, 0); // chrono principal agrandi (12/08, etait bold_84)
  lv_obj_set_style_text_color(lblBig, lv_color_white(), 0);

  lblClock = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblClock, &lv_font_teko_bold_56, 0); // taille intermediaire (etait bold_84, trop gros)
  lv_obj_set_style_text_color(lblClock, lv_color_white(), 0);

  lblRouteChrono = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblRouteChrono, &lv_font_teko_bold_56, 0); // meme taille que lblClock
  lv_obj_set_style_text_color(lblRouteChrono, lv_color_white(), 0);
  lv_obj_add_flag(lblRouteChrono, LV_OBJ_FLAG_HIDDEN); // mode Route uniquement, cf. updateStatusScreen()

  // Tactile retire (28/07, soir) -- theorie confirmee par un test dedie :
  // 2 redemarrages fantomes avec la trace "[trigger] REC via tactile" en
  // quelques minutes, sans aucun contact humain, alors meme que le bug
  // d'underflow independant etait deja corrige. Faux contact capacitif
  // confirme comme cause reelle et distincte -- cf. discussion du 28/07.
  // Seul le PUSH (bouton simple, contact mecanique, insensible au bruit
  // electrique) demarre l'enregistrement -- cf. handlePush()/SCR_STATUS.
  lblRecHint = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblRecHint, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lblRecHint, lv_palette_main(LV_PALETTE_RED), 0);
  lv_label_set_text(lblRecHint, "PRESS REC");

  lblDernier = lv_label_create(scrStatus);
  // bold_56, empile juste au-dessus de lblBest en bas d'ecran
  // (cf. commentaires Y ci-dessous et sur lblBig/lblBest).
  lv_obj_set_style_text_font(lblDernier, &lv_font_teko_bold_56, 0);
  lv_obj_set_style_text_color(lblDernier, lv_color_white(), 0);
  lv_obj_align(lblDernier, LV_ALIGN_TOP_LEFT, 4, 310);

  lblBest = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblBest, &lv_font_teko_bold_56, 0); // idem lblDernier
  lv_obj_set_style_text_color(lblBest, lv_color_white(), 0);
  lv_obj_align(lblBest, LV_ALIGN_TOP_LEFT, 4, 382);

  lblTours = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblTours, &lv_font_teko_bold_38, 0); // etait medium_34, aligne sur Dernier/Best
  lv_obj_set_style_text_color(lblTours, lv_color_white(), 0);
  lv_obj_align(lblTours, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
}

// ===================== Arret definitif (aligne sur chrono-AMOLED, 05/08) =====================
// BACK maintenu (BACK_HOLD_STOP_MS, 1400ms sur le 2.41) pendant
// l'enregistrement (SCR_STATUS) = arret definitif direct, sans ecran
// de confirmation -- cf. bloc dedie dans loop() et commentaire pres de
// cette constante.
// L'ancien systeme (SCR_CONFIRM_STOP, PUSH=reprendre/BACK=arreter)
// etait source de confusion reelle sur le terrain, retire le 05/08.

static void updateStatusScreen(unsigned long nowMs) {
  char buf[64];
  bool circuitDetected = detectionEffectivelyComplete();
  const char* gpsLabel = gpsActive ? "GPS OK" : "GPS --";

  if (circuitDetected) {
    snprintf(buf, sizeof(buf), "%s  Fix:%d Sat:%d  -- %s", gpsLabel, gpsFixStatus, gpsNumSVs, getActiveCourseNameForDisplay());
  } else {
    snprintf(buf, sizeof(buf), "%s  Fix:%d Sat:%d", gpsLabel, gpsFixStatus, gpsNumSVs);
  }
  lv_label_set_text(lblGps, buf);

  if (recordingEnabled) lv_obj_clear_flag(lblRec, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(lblRec, LV_OBJ_FLAG_HIDDEN);

  int battPercent = readBatteryPercent();
  lv_color_t battColor = battPercent > 50 ? lv_palette_main(LV_PALETTE_GREEN)
                        : battPercent > 20 ? lv_palette_main(LV_PALETTE_ORANGE)
                        : lv_palette_main(LV_PALETTE_RED);
  lv_obj_set_style_text_color(lblBatt, battColor, 0);
  snprintf(buf, sizeof(buf), "%3d%%", battPercent);
  lv_label_set_text(lblBatt, buf);

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);

  if (!recordingEnabled) {
    lv_obj_add_flag(lblRouteChrono, LV_OBJ_FLAG_HIDDEN); // au cas ou on vient de quitter le mode Route
    lv_obj_clear_flag(lblBig, LV_OBJ_FLAG_HIDDEN); // au cas ou on stoppe pile en phase "eteinte" du clignotement (cf. branche REC actif)
    snprintf(buf, sizeof(buf), "%d km/h", (int)gpsSpeedKmh);
    lv_label_set_text(lblBig, buf);
    lv_obj_align(lblBig, LV_ALIGN_TOP_MID, 0, 40);

    char timeBuf[10];
    getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));
    lv_label_set_text(lblClock, timeBuf);
    lv_obj_align(lblClock, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_clear_flag(lblClock, LV_OBJ_FLAG_HIDDEN);

    if (circuitDetected) {
      lv_obj_align(lblRecHint, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
      lv_obj_clear_flag(lblRecHint, LV_OBJ_FLAG_HIDDEN);
      if (battPercent <= LOW_BATT_NO_REC_PERCENT) {
        lv_label_set_text(lblRecHint, "NO BAT");
      } else {
        lv_label_set_text(lblRecHint, "PRESS REC");
      }
    } else {
      lv_obj_add_flag(lblRecHint, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(lblDernier, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblBest, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblTours, LV_OBJ_FLAG_HIDDEN);

  } else if (routeMode) {
    // Mode Route : vitesse en gros comme avant REC (plus utile en
    // continu qu'un chrono qui defile), heure conservee a sa place
    // habituelle (comme avant REC), chrono du parcours ajoute en face
    // (BOTTOM_RIGHT, meme taille) -- duree ecoulee depuis le depart, ne
    // repart jamais a zero. Dernier/Best/Tours toujours masques (aucun
    // sens sans notion de tour). Aligne sur chrono-AMOLED.
    lv_obj_clear_flag(lblBig, LV_OBJ_FLAG_HIDDEN); // au cas ou on bascule en mode Route pile en phase "eteinte" du clignotement (cf. branche REC actif)
    snprintf(buf, sizeof(buf), "%d km/h", (int)gpsSpeedKmh);
    lv_label_set_text(lblBig, buf);
    lv_obj_align(lblBig, LV_ALIGN_TOP_MID, 0, 130); // meme position que le chrono en REC actif (cf. plus bas)

    char timeBuf[10];
    getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));
    lv_label_set_text(lblClock, timeBuf);
    lv_obj_align(lblClock, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_clear_flag(lblClock, LV_OBJ_FLAG_HIDDEN);

    formatLapTime(currentLapMs, buf, sizeof(buf));
    lv_label_set_text(lblRouteChrono, buf);
    lv_obj_align(lblRouteChrono, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_clear_flag(lblRouteChrono, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(lblDernier, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblBest, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblTours, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblRecHint, LV_OBJ_FLAG_HIDDEN);

  } else {
    lv_obj_add_flag(lblRouteChrono, LV_OBJ_FLAG_HIDDEN); // hors mode Route -- au cas ou on vient d'en sortir
    if (millis() < lapFreezeUntilMs) {
      // Tour vient de se terminer -- affiche encore son temps fige,
      // plutot que de reafficher direct le chrono du tour suivant.
      // Clignote (500ms allume/250ms eteint) pour bien signaler que
      // c'est un temps fige, pas le chrono qui continue de tourner --
      // sinon rien ne distingue visuellement ce temps de tour termine
      // d'un chrono actif normal.
      formatLapTime(getLastFinishedLapMs(), buf, sizeof(buf));
      unsigned long phase = millis() % 750; // cycle 500ms ON + 250ms OFF = 750ms
      bool blinkOn = phase < 500;
      if (blinkOn) lv_obj_clear_flag(lblBig, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(lblBig, LV_OBJ_FLAG_HIDDEN);
    } else if (currentLapMs > 0) {
      // Ligne franchie (getRaceStarted() true cote CourseManager/DovesLapTimer) -- chrono actif.
      lv_obj_clear_flag(lblBig, LV_OBJ_FLAG_HIDDEN); // au cas ou on sort tout juste du clignotement ci-dessus
      formatLapTime(currentLapMs, buf, sizeof(buf));
    } else {
      // REC actif mais course pas encore demarree -- vitesse plus utile qu'un chrono fige.
      lv_obj_clear_flag(lblBig, LV_OBJ_FLAG_HIDDEN); // idem
      snprintf(buf, sizeof(buf), "%d km/h", (int)gpsSpeedKmh);
    }
    lv_label_set_text(lblBig, buf);
    // y=130 (remonte depuis 165, sur demande -- meme position que le
    // mode Route desormais, cf. plus haut). lblBig (bold_110,
    // line_height=125) occupe 130-255, lblDernier 310-374, lblBest
    // 382-446. ~55px de marge entre lblBig et lblDernier.
    lv_obj_align(lblBig, LV_ALIGN_TOP_MID, 0, 130);

    char lastBuf[16], bestBuf[16];
    formatLapTime(getLastFinishedLapMs(), lastBuf, sizeof(lastBuf));
    formatLapTime(hasBest ? bestLapMs : 0, bestBuf, sizeof(bestBuf));
    snprintf(buf, sizeof(buf), "Dernier: %s", lastBuf);
    lv_label_set_text(lblDernier, buf);
    snprintf(buf, sizeof(buf), "Best: %s", bestBuf);
    lv_label_set_text(lblBest, buf);
    lv_obj_clear_flag(lblDernier, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lblBest, LV_OBJ_FLAG_HIDDEN);

    int toursDisplay = lapsCount + (currentLapMs > 0 ? 1 : 0);
    snprintf(buf, sizeof(buf), "Tours: %d", toursDisplay);
    lv_label_set_text(lblTours, buf);
    lv_obj_clear_flag(lblTours, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(lblClock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblRecHint, LV_OBJ_FLAG_HIDDEN);
  }
}

// ===================== Helpers d'ecran =====================
//
// createListRow : medium_34 (agrandi, etait medium_26) -- pour les
// listes courtes (Circuit ~5 items, Reglages 1 item) qui ont la place.
// createListRowSmall : medium_26 -- pour les listes qui peuvent avoir
// jusqu'a 8 lignes (Session/Tours), ou medium_34 deborderait.

static lv_obj_t* createListRow(lv_obj_t* parent, int16_t y) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, y);
  return lbl;
}

static lv_obj_t* createListRowSmall(lv_obj_t* parent, int16_t y) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, y);
  return lbl;
}

static lv_obj_t* createTitle(lv_obj_t* parent, const char* text) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_label_set_text(lbl, text);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, 0);
  return lbl;
}

// ===================== Liste defilable (scroll tactile vertical) =====================
//
// Conteneur LVGL natif avec scroll vertical -- remplace les tableaux
// de labels a position fixe (limites a un nombre max de lignes sans
// defilement). Les lignes sont creees/detruites a chaque refresh
// (lv_obj_clean + recreation), acceptable ici car ces ecrans ne se
// rafraichissent que sur evenement (pas en continu comme Statut).

static lv_obj_t* createScrollList(lv_obj_t* parent, int16_t yTop, int16_t bottomMargin = 4) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  // 600x450 (2.41) -- PAS 536x240 (1.91). Bug de merge du 14/08 : cette
  // fonction avait ete recopiee telle quelle avec les dimensions du
  // 1.91, jamais adaptee -- corrige le 18/08 (remonte par l'utilisateur
  // : trop peu de tours/sessions visibles sans raison apparente).
  lv_obj_set_size(cont, 600, 450 - yTop - bottomMargin);
  lv_obj_set_pos(cont, 0, yTop);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(cont, 4, 0);
  lv_obj_set_style_pad_row(cont, 4, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER); // vertical seulement -- le glissement horizontal reste libre pour le swipe d'ecran (bubble vers le parent)
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
  return cont;
}

// Ligne de liste dans un conteneur flex -- pas de positionnement manuel,
// le flex layout s'en charge automatiquement.
static lv_obj_t* createFlexRow(lv_obj_t* parent, bool big) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, big ? &lv_font_teko_medium_34 : &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE); // les labels ne sont PAS cliquables par defaut, contrairement aux lv_obj_create() de base
  // Flash visuel des l'appui (pas seulement au relachement) -- sinon
  // aucun retour tant que l'action ne s'est pas declenchee, ce qui est
  // trop tard vu que l'ecran change souvent immediatement apres.
  lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(lbl, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(lbl, lv_color_black(), LV_STATE_PRESSED);
  return lbl;
}

// ===================== Ecran Circuit =====================

// Declarations anticipees -- definies plus bas (pres de handlePush),
// utilisees ici par les callbacks de tap direct.
static void applyCircuitSelection(int i);
static void openSessionLaps(int i);

static lv_obj_t* circuitListCont;

static void buildCircuitScreen() {
  scrCircuit = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrCircuit, lv_color_black(), 0);
  enableRingSwipe(scrCircuit);
  createTitle(scrCircuit, "Circuit");

  circuitListCont = createScrollList(scrCircuit, 44); // pleine hauteur -- plus de bouton en bas, "Nouveau circuit" a son propre ecran dans l'anneau
}

static void circuitRowTappedCb(lv_event_t* e) {
  // Bloque le changement de circuit pendant un enregistrement en cours --
  // le tap direct valide immediatement sans confirmation, donc un tap
  // parasite ici en pleine session changerait le circuit actif sous
  // CourseManager sans aucun garde-fou. Manquait sur le 2.41 depuis le
  // merge du 14/08 (le banc display_only_241 n'avait pas
  // d'enregistrement reel, donc pas besoin de cette protection --
  // repere et corrige le 18/08 en comparant au 1.91, cf. meme garde-fou
  // present depuis le debut sur ce dernier).
  if (recordingEnabled) {
    Serial.println("[UI] Tap circuit ignore -- enregistrement en cours, changement de circuit bloque.");
    return;
  }
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  applyCircuitSelection(idx); // tap direct = selectionne + valide en un seul geste
}

static void refreshCircuitScreen() {
  lv_obj_clean(circuitListCont);
  int itemCount = myTracks.courseCount + 1; // Auto + N circuits reels charges depuis circuits.csv
  for (int i = 0; i < itemCount; i++) {
    lv_obj_t* row = createFlexRow(circuitListCont, true);
    bool sel = selectingMode && (i == circuitSelection);
    lv_obj_set_style_text_color(row, sel ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
    char buf[40];
    const char* prefix = sel ? "> " : "  ";
    if (i == 0) snprintf(buf, sizeof(buf), "%sAuto (detection)", prefix);
    else snprintf(buf, sizeof(buf), "%s%s", prefix, myTracks.courses[i - 1].name);
    lv_label_set_text(row, buf);
    lv_obj_add_event_cb(row, circuitRowTappedCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

// ===================== Ecran Connexion =====================

static lv_obj_t* lblConnGps;
static lv_obj_t* lblConnCircuit;
static lv_obj_t* lblConnSd;
static lv_obj_t* lblConnBatt;

static void buildConnexionScreen() {
  scrConnexion = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrConnexion, lv_color_black(), 0);
  enableRingSwipe(scrConnexion);
  createTitle(scrConnexion, "Connexion");

  lblConnGps = createListRow(scrConnexion, 44);
  lblConnCircuit = createListRow(scrConnexion, 84);
  lblConnSd = createListRow(scrConnexion, 124);
  lblConnBatt = createListRow(scrConnexion, 164);
}

static void refreshConnexionScreen() {
  char buf[64];
  // Debit GPS mesure au boot ajoute sur la meme ligne (cf. diagnostic
  // 04/08 -- souci cablage TX qui bloquait le passage a 10Hz, invisible
  // sans ecran vu qu'on n'a jamais le serial sur la moto). Ecran en
  // 536x240 (paysage) : largeur confortable pour une ligne plus longue,
  // mais pas de hauteur pour une 5e ligne separee.
  snprintf(buf, sizeof(buf), "%s  Fix:%d  Sat:%d  %.0fHz", gpsActive ? "GPS OK" : "GPS --", gpsFixStatus, gpsNumSVs, gpsMeasuredRmcHz);
  lv_obj_set_style_text_color(lblConnGps, (gpsActive && gpsMeasuredRmcHz < 8.0f) ? lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
  lv_label_set_text(lblConnGps, buf);

  // Pendant la phase de detection (avant qu'un circuit soit valide ou
  // que le repli Lap Anything s'active), on affiche le compteur de
  // rejets plutot que juste "Detection..." -- utile pour voir sur
  // circuit si ca boucle anormalement (candidat jamais valide, cf.
  // diagnostic du 04/08) sans avoir besoin du serial.
  if (!routeMode && !newCircuitCaptureArmed && !manualOverrideActive &&
      !detectionEffectivelyComplete() && !lapAnythingEffective() && courseManager) {
    char circBuf[48];
    snprintf(circBuf, sizeof(circBuf), "Detection... (%d rejets)", courseManager->getDetectionRejectionCount());
    lv_label_set_text(lblConnCircuit, circBuf);
  } else {
    lv_label_set_text(lblConnCircuit, getActiveCourseNameForDisplay());
  }

  if (gpsLogsOnSd) {
    unsigned long usedGb10 = (unsigned long)(sdUsedBytes() * 10 / (1024ULL * 1024 * 1024));
    unsigned long totalGb10 = (unsigned long)(sdTotalBytes() * 10 / (1024ULL * 1024 * 1024));
    snprintf(buf, sizeof(buf), "SD OK  %lu.%lu/%lu.%lu GB",
             usedGb10 / 10, usedGb10 % 10, totalGb10 / 10, totalGb10 % 10);
    lv_obj_set_style_text_color(lblConnSd, lv_color_white(), 0);
  } else {
    snprintf(buf, sizeof(buf), "SD --  repli LittleFS");
    lv_obj_set_style_text_color(lblConnSd, lv_palette_main(LV_PALETTE_ORANGE), 0);
  }
  lv_label_set_text(lblConnSd, buf);

  float v = 0;
  int raw = 0;
  adc_get_value(&v, &raw);
  int battPct = batteryVoltageToPercent(v);
  // Temperature interne du capteur ESP32-S3 -- surveillance ajoutee le
  // 05/08 suite a une inquietude sur le montage AMOLED : l'ESP32 se
  // trouve juste sous le connecteur SD, et le nouveau verrou imprime
  // pourrait gener la convection a cet endroit. Rouge si >70C (marge
  // avant la zone ou le plastique PLA du boitier/verrou commencerait a
  // ramollir, cf. Tg PLA ~60C -- reste large en dessous du seuil de
  // throttling du chip lui-meme, plus haut).
  float cpuTempC = temperatureRead();
  snprintf(buf, sizeof(buf), "Batt %d%%  (%.2fV)  CPU %.0fC", battPct, v, cpuTempC);
  lv_obj_set_style_text_color(lblConnBatt, (battPct <= 15 || cpuTempC >= 70.0f) ? lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
  lv_label_set_text(lblConnBatt, buf);
}

// ===================== Ecran Session (liste) =====================

static lv_obj_t* sessionListCont;
static lv_obj_t* lblSessionEmpty;
static std::vector<SessionSummary> sessionListCache;

static void buildSessionListScreen() {
  scrSessionList = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSessionList, lv_color_black(), 0);
  enableRingSwipe(scrSessionList);
  createTitle(scrSessionList, "Sessions");

  sessionListCont = createScrollList(scrSessionList, 44);
  lblSessionEmpty = createListRow(scrSessionList, 44);
}

static void sessionRowTappedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  openSessionLaps(idx); // tap direct = ouvre les tours en un seul geste
}

static void refreshSessionListScreen() {
  sessionListCache = loadSessionSummaries();
  std::reverse(sessionListCache.begin(), sessionListCache.end()); // plus recente en premier

  bool empty = sessionListCache.empty();
  if (empty) {
    lv_label_set_text(lblSessionEmpty, "Aucune session (PUSH ou tap REC sur Statut pour enregistrer).");
    lv_obj_clear_flag(lblSessionEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(sessionListCont);
  } else {
    lv_obj_add_flag(lblSessionEmpty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(sessionListCont);
    for (int i = 0; i < (int)sessionListCache.size(); i++) {
      const SessionSummary& s = sessionListCache[i];
      lv_obj_t* row = createFlexRow(sessionListCont, true);
      bool sel = selectingMode && (i == sessionListSelection);
      lv_obj_set_style_text_color(row, sel ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);

      char bestBuf[16];
      formatLapTime(s.bestLapMs == ULONG_MAX ? 0 : s.bestLapMs, bestBuf, sizeof(bestBuf));
      char buf[64];
      snprintf(buf, sizeof(buf), "%s%s  %d tours  %s", sel ? "> " : "  ",
               formatCompactKeyShort(s.compactKey).c_str(), s.lapCount, bestBuf);
      lv_label_set_text(row, buf);
      lv_obj_add_event_cb(row, sessionRowTappedCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
  }

}

// Session actuellement selectionnee (meme ordre affiche -- plus recente en premier)
static const SessionSummary* selectedSession() {
  if (sessionListCache.empty()) return nullptr;
  if (sessionListSelection < 0 || sessionListSelection >= (int)sessionListCache.size()) return nullptr;
  return &sessionListCache[sessionListSelection];
}

// ===================== Ecran Session (tours) =====================

static lv_obj_t* lapListCont;
static lv_obj_t* lblLapEmpty;
static std::vector<LapDetail> lapListCache;

// Factorisee -- utilisee a la fois par handleBack() (bouton physique)
// et par le nouveau bouton tactile de retour (haut droite de l'ecran).
static void backFromSessionLapsToList() {
  goToScreen(SCR_SESSION_LIST); // remet selectingMode a false en interne
  selectingMode = true;         // on veut rester en mode selection sur la liste des sessions
  screenDirty = true;
}

static void btnBackLapsClickedCb(lv_event_t* e) {
  backFromSessionLapsToList();
}

static void buildSessionLapsScreen() {
  scrSessionLaps = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSessionLaps, lv_color_black(), 0);
  lv_obj_clear_flag(scrSessionLaps, LV_OBJ_FLAG_SCROLLABLE); // le scroll se fait dans le conteneur interne, pas sur l'ecran
  createTitle(scrSessionLaps, "Tours");

  // Bouton tactile de retour -- en plus du BACK physique, pratique avec
  // des gants ou pour rester une main sur le guidon. Compact, coin haut
  // droit -- deplace le hint juste en dessous pour ne pas chevaucher.
  lv_obj_t* btnBack = lv_btn_create(scrSessionLaps);
  lv_obj_set_size(btnBack, 100, 46);
  lv_obj_align(btnBack, LV_ALIGN_TOP_RIGHT, -4, 0);
  lv_obj_set_style_bg_color(btnBack, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_bg_color(btnBack, lv_palette_darken(LV_PALETTE_GREY, 2), LV_STATE_PRESSED);
  lv_obj_set_style_radius(btnBack, 8, 0);
  lv_obj_add_event_cb(btnBack, btnBackLapsClickedCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblBtnBack = lv_label_create(btnBack);
  lv_obj_set_style_text_font(lblBtnBack, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lblBtnBack, lv_color_white(), 0);
  lv_label_set_text(lblBtnBack, "Liste");
  lv_obj_center(lblBtnBack);

  lapListCont = createScrollList(scrSessionLaps, 44);
  lblLapEmpty = createListRow(scrSessionLaps, 44);
}

static void refreshSessionLapsScreen() {
  const SessionSummary* s = selectedSession();
  if (s) lapListCache = loadLapsForSession(s->compactKey);
  else lapListCache.clear();
  bool empty = lapListCache.empty();

  lv_obj_clean(lapListCont);

  if (empty) {
    lv_label_set_text(lblLapEmpty, "Aucun tour dans cette session.");
    lv_obj_clear_flag(lblLapEmpty, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(lblLapEmpty, LV_OBJ_FLAG_HIDDEN);

  unsigned long trueBest = ULONG_MAX;
  for (const LapDetail& lap : lapListCache) if (lap.lapMs < trueBest) trueBest = lap.lapMs;

  for (int i = 0; i < (int)lapListCache.size(); i++) {
    const LapDetail& lap = lapListCache[i];
    lv_obj_t* row = createFlexRow(lapListCont, true);
    bool sel = (i == sessionLapSelection);
    bool isBest = (lap.lapMs == trueBest);
    lv_obj_set_style_text_color(row, sel ? lv_palette_main(LV_PALETTE_YELLOW)
                                        : isBest ? lv_palette_main(LV_PALETTE_GREEN)
                                        : lv_color_white(), 0);
    char lapBuf[16];
    formatLapTime(lap.lapMs, lapBuf, sizeof(lapBuf));
    char vmaxBuf[16];
    if (lap.maxSpeedKmh > 0) snprintf(vmaxBuf, sizeof(vmaxBuf), "  Vmax %.0f", lap.maxSpeedKmh);
    else vmaxBuf[0] = '\0'; // anciennes sessions sans vitesse max enregistree
    char buf[64];
    snprintf(buf, sizeof(buf), "%sTour %d : %s%s%s", sel ? "> " : "  ", lap.lapNumber, lapBuf, isBest ? "  (best)" : "", vmaxBuf);
    lv_label_set_text(row, buf);
  }
}


// ===================== Ecran Reglages =====================

static lv_obj_t* lblSettingsRow;
static lv_obj_t* lblFreezeRow;
static lv_obj_t* lblRouteRow;
static lv_obj_t* lblDiagLogsRow;

static void settingsRowTappedCb(lv_event_t* e) {
  selectingMode = false;
  wifiStartRequested = true; // traite dans loop(), pas ici (cf. commentaire pres de la declaration)
  goToScreen(SCR_WIFI); // tap direct = ouvre WiFi en un seul geste
}

// Cycle parmi FREEZE_PRESETS_S a chaque tap (0/5/10/15/20/30s, boucle).
static void refreshSettingsScreen();
static void freezeRowTappedCb(lv_event_t* e) {
  int idx = 0;
  for (int i = 0; i < FREEZE_PRESETS_COUNT; i++) if (FREEZE_PRESETS_S[i] == lapFreezeS) { idx = i; break; }
  idx = (idx + 1) % FREEZE_PRESETS_COUNT;
  lapFreezeS = FREEZE_PRESETS_S[idx];
  refreshSettingsScreen();
}

// Toggle ON/OFF par tap -- pas de sauvegarde (routeMode n'est jamais
// persiste, cf. commentaire pres de sa declaration). Ignore le tap si
// un enregistrement est en cours : changer de mode en plein REC
// laisserait les accumulateurs de stats dans un etat incoherent (aligne
// sur chrono-AMOLED).
static void routeRowTappedCb(lv_event_t* e) {
  if (recordingEnabled) return;
  routeMode = !routeMode;
  refreshSettingsScreen();
}

// Toggle ON/OFF par tap, persiste comme lapFreezeS (contrairement a
// routeMode) -- reglage materiel/usure SD, pas un mode de session
// ponctuel, doit survivre au redemarrage.
static void diagLogsRowTappedCb(lv_event_t* e) {
  if (recordingEnabled) return;
  diagLogsEnabled = !diagLogsEnabled;
  saveDisplaySettings();
  refreshSettingsScreen();
}

static void buildSettingsScreen() {
  scrSettings = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSettings, lv_color_black(), 0);
  enableRingSwipe(scrSettings);
  createTitle(scrSettings, "Reglages");

  lblSettingsRow = createListRow(scrSettings, 100);
  lv_obj_add_flag(lblSettingsRow, LV_OBJ_FLAG_CLICKABLE); // idem -- label non cliquable par defaut
  lv_obj_set_style_bg_opa(lblSettingsRow, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(lblSettingsRow, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(lblSettingsRow, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_add_event_cb(lblSettingsRow, settingsRowTappedCb, LV_EVENT_CLICKED, NULL);

  lblFreezeRow = createListRow(scrSettings, 160);
  lv_obj_add_flag(lblFreezeRow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(lblFreezeRow, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(lblFreezeRow, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(lblFreezeRow, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_add_event_cb(lblFreezeRow, freezeRowTappedCb, LV_EVENT_CLICKED, NULL);

  lblRouteRow = createListRow(scrSettings, 220);
  lv_obj_add_flag(lblRouteRow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(lblRouteRow, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(lblRouteRow, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(lblRouteRow, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_add_event_cb(lblRouteRow, routeRowTappedCb, LV_EVENT_CLICKED, NULL);

  lblDiagLogsRow = createListRow(scrSettings, 280);
  lv_obj_add_flag(lblDiagLogsRow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(lblDiagLogsRow, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(lblDiagLogsRow, lv_palette_main(LV_PALETTE_YELLOW), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(lblDiagLogsRow, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_add_event_cb(lblDiagLogsRow, diagLogsRowTappedCb, LV_EVENT_CLICKED, NULL);
}

static void refreshSettingsScreen() {
  bool sel = selectingMode;
  lv_obj_set_style_text_color(lblSettingsRow, sel ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
  lv_label_set_text(lblSettingsRow, sel ? "> WiFi telechargement" : "  WiFi telechargement");

  char buf[32];
  if (lapFreezeS == 0) snprintf(buf, sizeof(buf), "  Pause chrono: desactive");
  else snprintf(buf, sizeof(buf), "  Pause chrono: %ds", lapFreezeS);
  lv_label_set_text(lblFreezeRow, buf);

  lv_obj_set_style_text_color(lblRouteRow, routeMode ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_white(), 0);
  lv_label_set_text(lblRouteRow, routeMode ? "  Mode Route: ON" : "  Mode Route: OFF");

  lv_obj_set_style_text_color(lblDiagLogsRow, diagLogsEnabled ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_white(), 0);
  lv_label_set_text(lblDiagLogsRow, diagLogsEnabled ? "  Logs diag: ON" : "  Logs diag: OFF");
}

// ===================== Ecran WiFi =====================

static lv_obj_t* lblWifiSsid;
static lv_obj_t* lblWifiIp;

static void buildWifiScreen() {
  scrWifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrWifi, lv_color_black(), 0);
  createTitle(scrWifi, "WiFi actif");

  lblWifiSsid = createListRow(scrWifi, 46);
  lblWifiIp = createListRow(scrWifi, 86);
  lv_obj_t* l3 = createListRowSmall(scrWifi, 130);
  lv_label_set_text(l3, "Connecte-toi au reseau ci-dessus, puis");
  lv_obj_t* l4 = createListRowSmall(scrWifi, 156);
  lv_label_set_text(l4, "va sur l'adresse IP depuis un navigateur.");

}

static void refreshWifiScreen() {
  char buf[48];
  snprintf(buf, sizeof(buf), "SSID: %s", webServerManager.getSsid().c_str());
  lv_label_set_text(lblWifiSsid, buf);
  snprintf(buf, sizeof(buf), "IP:   %s", webServerManager.getIp().c_str());
  lv_label_set_text(lblWifiIp, buf);
}

// ===================== Ecran Nouveau Circuit (capture) =====================
//
// Ecran dedie, retire de la liste Circuit (etait un item parmi
// d'autres avant) -- accessible via le bouton "+ Nouveau circuit" en
// bas de l'ecran Circuit.

static void startCaptureTappedCb(lv_event_t* e) {
  armNewCircuitCapture();
  goToScreen(SCR_STATUS);
}

static void buildNewCircuitScreen() {
  scrNewCircuit = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrNewCircuit, lv_color_black(), 0);
  enableRingSwipe(scrNewCircuit);
  createTitle(scrNewCircuit, "Nouveau circuit");

  lv_obj_t* l1 = createListRowSmall(scrNewCircuit, 46);
  lv_label_set_text(l1, "Capture (simule) : le trace du circuit sera");
  lv_obj_t* l2 = createListRowSmall(scrNewCircuit, 72);
  lv_label_set_text(l2, "enregistre au premier tour, ligne de depart/");
  lv_obj_t* l3 = createListRowSmall(scrNewCircuit, 98);
  lv_label_set_text(l3, "arrivee detectee automatiquement ensuite.");

  lv_obj_t* btnStart = lv_btn_create(scrNewCircuit);
  lv_obj_set_size(btnStart, 320, 60);
  lv_obj_align(btnStart, LV_ALIGN_CENTER, 0, 40);
  lv_obj_set_style_bg_color(btnStart, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_set_style_bg_color(btnStart, lv_palette_darken(LV_PALETTE_GREEN, 2), LV_STATE_PRESSED);
  lv_obj_add_event_cb(btnStart, startCaptureTappedCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblStart = lv_label_create(btnStart);
  lv_obj_set_style_text_font(lblStart, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lblStart, lv_color_white(), 0);
  lv_label_set_text(lblStart, "Demarrer la capture");
  lv_obj_center(lblStart);

}

// ===================== Refresh dispatcher =====================

static void refreshCurrentScreen(unsigned long nowMs) {
  switch (currentScreen) {
    case SCR_STATUS:       updateStatusScreen(nowMs); break;
    case SCR_CIRCUIT:      refreshCircuitScreen(); break;
    case SCR_CONNEXION:    refreshConnexionScreen(); break;
    case SCR_SESSION_LIST: refreshSessionListScreen(); break;
    case SCR_SESSION_LAPS: refreshSessionLapsScreen(); break;
    case SCR_SETTINGS:     refreshSettingsScreen(); break;
    case SCR_WIFI:         refreshWifiScreen(); break;
    case SCR_NEW_CIRCUIT:  break; // statique, rien a rafraichir
  }
  screenDirty = false;
}

// ===================== Gestion PUSH / BACK selon l'ecran courant =====================

// Applique la selection d'un item Circuit (index i) -- factorise pour
// etre appelee aussi bien par la validation PUSH (2 temps)
// que par un tap direct sur une ligne (1 temps, tactile).
static void applyCircuitSelection(int i) {
  if (i == 0) {
    activateAutoMode();
  } else {
    activateManualCourse(i - 1);
  }

  selectingMode = false;
  goToScreen(SCR_STATUS); // rebascule sur le chrono avec le circuit qu'on vient de choisir
  screenDirty = true;
}

// Ouvre l'ecran des tours pour la session d'index affiche i (0 = plus
// recente) -- factorise pour etre appele par la validation PUSH (2
// temps) et par un tap direct sur une ligne (1 temps).
static void openSessionLaps(int i) {
  sessionListSelection = i;
  sessionLapSelection = 0;
  goToScreen(SCR_SESSION_LAPS);
  screenDirty = true;
}

static void handlePush() {
  switch (currentScreen) {
    case SCR_STATUS:
      // PUSH ne fait plus rien pendant l'enregistrement (05/08) --
      // suppression du mode Pause/Reprendre. En course ou en track day
      // le chrono tourne du paddock au paddock, sans pause -- ce mode
      // n'avait plus de raison d'etre et son ecran de confirmation
      // (PUSH=reprendre / BACK=arreter) etait une source de confusion
      // reelle constatee sur le terrain (cf. session du 06/08 --
      // plusieurs redemarrages parasites en cherchant a arreter).
      // L'arret se fait desormais par un appui BACK MAINTENU (cf. bloc
      // dedie dans loop(), pres de BACK_HOLD_STOP_MS).
      if (recordingEnabled) break;
      if (millis() - lastDefinitiveStopMs < STATUS_REC_GRACE_AFTER_STOP_MS) break; // cf. commentaire pres de la constante
      if (readBatteryPercent() <= LOW_BATT_NO_REC_PERCENT) break; // "NO BAT" affiche a la place -- pas de nouvel enregistrement sous ce seuil
      if (detectionEffectivelyComplete()) {
        startRecording();
      }
      break;

    case SCR_CIRCUIT:
      if (!selectingMode) {
        selectingMode = true;
        screenDirty = true;
      } else {
        applyCircuitSelection(circuitSelection); // valide la selection (2e PUSH) -- cf. tap direct, alternative equivalente
      }
      break;

    case SCR_CONNEXION:
      break; // rien a selectionner ici

    case SCR_SESSION_LIST:
      if (!sessionListCache.empty()) {
        if (!selectingMode) {
          selectingMode = true;
        } else {
          openSessionLaps(sessionListSelection);
        }
      }
      screenDirty = true;
      break;

    case SCR_SESSION_LAPS:
      break; // simple defilement, rien a valider

    case SCR_SETTINGS:
      if (!selectingMode) {
        selectingMode = true;
      } else {
        selectingMode = false;
        wifiStartRequested = true; // traite dans loop(), pas ici
        goToScreen(SCR_WIFI);
      }
      screenDirty = true;
      break;

    case SCR_WIFI:
      break;

    case SCR_NEW_CIRCUIT:
      break; // rien a valider ici, le bouton "Demarrer la capture" gere tout au tap
  }
}

static void handleBack() {
  switch (currentScreen) {
    case SCR_STATUS:
      // Option 1 (comme sur TFT) : BACK ne met plus en pause -- c'est le
      // role de PUSH desormais (cf. handlePush()). BACK reste le simple
      // retour au menu Circuit, meme pendant un enregistrement en cours
      // (comportement identique a TFT/OLED sur ce point).
      cancelNewCircuitCapture(); // no-op si rien n'est arme
      goToScreen(SCR_CIRCUIT);
      break;

    case SCR_CIRCUIT:
    case SCR_NEW_CIRCUIT:
    case SCR_CONNEXION:
    case SCR_SESSION_LIST:
    case SCR_SETTINGS:
      if (selectingMode) {
        selectingMode = false;
        setEncoderForRing();
        screenDirty = true;
      } else {
        goToScreen(SCR_STATUS);
      }
      break;

    case SCR_SESSION_LAPS:
      backFromSessionLapsToList();
      break;

    case SCR_WIFI:
      wifiStopRequested = true; // traite dans loop(), pas ici
      goToScreen(SCR_STATUS); // coupe le WiFi et revient direct au statut, comme le TFT
      break;
  }
}

// ===================== Setup / Loop =====================

void setup() {
  // Maintien alimentation batterie (GPIO16, BAT_ON) -- DOIT etre la
  // toute premiere chose faite en boot, avant meme Serial.begin().
  // Sur ce board, le bouton PWR n'alimente le micro que
  // temporairement le temps qu'il demarre ; c'est au firmware de
  // reprendre la main tres vite en pilotant GPIO16 pour maintenir
  // l'alimentation, sinon elle retombe au relachement du bouton avant
  // que le reste du code n'ait eu le temps de s'executer (constate le
  // 14/08 : appui PWR -> LED GPS s'allume -> s'eteint au relachement,
  // le micro n'atteignait jamais adc_bsp_init() qui pilotait GPIO16
  // bien trop tard, apres GPS/SD/CourseManager/WebServer).
  BAT_GPIO_Init();
  BAT_ON();

  Serial.begin(115200);
  delay(1000);
  Serial.println("=== firmware_241 (merge complet 14/08 : moteur reel + ecrans 600x450) -- GPS/CourseManager/SD reels (Circuit/Nouveau circuit/Connexion/Session/Reglages) ===");

  I2C_master_Init();
  Serial.println("[1/5] I2C partage ok");
  // 2.41 V2 : reset ecran/tactile via TCA9554 (EXIO0/EXIO1) --
  // obligatoire avant displayInit(), sans quoi le panneau RM690B0 ne
  // recoit jamais son pulse de reset materiel (ecran blanc/indetermine,
  // pas d'erreur de compilation ni de plantage -- cf. bring-up
  // display_only_241 du 12/08).
  expanderInit();
  expanderResetOled();
  displayInit();
  Serial.println("[2/5] Ecran + LVGL ok");
  expanderResetTouch();
  Touch_Init();
  Serial.println("[3/5] Tactile ok");

  initGps();
  Serial.println("[3.5/5] GPS ok (reel -- plus simule)");

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: echec de montage (meme apres formatage) -- circuits/sessions indisponibles.");
  }
  loadDisplaySettings();
  Serial.printf("Reglages : temps de pose = %ds.\n", lapFreezeS);
  bool sdOk = initSdLogStorage();
  Serial.printf("SD (SDMMC) : %s -- logs GPS detailles sur %s.\n", sdOk ? "montee" : "absente/en panne", sdOk ? "carte SD" : "LittleFS (repli)");
  migrateLittleFsLogsToSd();

  loadActiveCircuitsIntoTracks();
  courseManager = new CourseManager(myTracks, 10.0, &Serial); // etait 7.0 -- augmente le 01/08 suite a des franchissements manques a haute vitesse (Carole, ~130-200km/h, ligne pourtant large de 12.9m -- cf. README, section "Nouveautes du 01/08")

  webServerManager.begin("ChronoMotoAMOLED", SESSION_LOG_PATH, CIRCUITS_FILE_PATH, *gpsLogFs, nullptr, nullptr, flushLogsCallback, getStatusCallback);

  adc_bsp_init();

  bool imuReady = initImu();
  Serial.printf("IMU (QMI8658) : %s -- lean angle %s.\n",
                imuReady ? "detecte" : "absent/en panne",
                imuReady ? "logge (log_*.csv)" : "indisponible (0 dans le log)");

  pinMode(BACK_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BACK_BUTTON), backButtonISR, FALLING);
  pinMode(PUSH_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PUSH_BUTTON), pushButtonISR, FALLING);
  pinMode(PWR_BUTTON, INPUT_PULLUP); // pas d'interruption -- simple sondage en loop(), pas urgent
  Serial.println("[4/5] PUSH + BACK ok");

  if (lvglLock(-1)) {
    lv_obj_t* scrSplash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scrSplash, lv_color_black(), 0);
    lv_obj_t* splashImg = lv_img_create(scrSplash);
    lv_img_set_src(splashImg, &splash_pigteam_img);
    lv_obj_align(splashImg, LV_ALIGN_TOP_LEFT, 0, 0); // image deja a la resolution exacte (536x240)
    lv_scr_load(scrSplash);

    buildStatusScreen();
    buildCircuitScreen();
    buildConnexionScreen();
    buildSessionListScreen();
    buildSessionLapsScreen();
    buildSettingsScreen();
    buildWifiScreen();
    buildNewCircuitScreen();
    lvglUnlock();
  }
  Serial.println("[5/5] Ecrans construits, splash affiche");

  delay(3500); // laisse le splash visible plus longtemps avant de basculer sur le Statut (etait 2000ms)

  if (lvglLock(-1)) {
    lv_scr_load(scrStatus);
    lvglUnlock();
  }

  Serial.println("Pret. Statut: PUSH avance (recherche->detecte->REC), BACK arrete le REC ou va sur Circuit. Anneau: swipe = change d'ecran, tap = selection, BACK = retour.");
  printSerialCommandsHelp();
}

void loop() {
  unsigned long nowMs = millis();
  pollGps();
  gpsUpdateFromLiveData();
  webServerManager.loop();
  handleSerialCommands();
  imuTick(); // inconditionnel (meme hors REC) -- garde le filtre d'angle "chaud"
  checkWheelieStoppie(); // no-op si REC off ou IMU absente

  if (newGpsData) {
    newGpsData = false;
    double lat = liveData.latitude / 1e7;
    double lng = liveData.longitude / 1e7;
    float altM = liveData.wgsAltitude / 1000.0f;
    float speedKnots = liveData.speedMmPerS / 514.444f; // mm/s -> noeuds (unite DovesLapTimer)
    processGpsFix(lat, lng, altM, speedKnots, nowMs);
    logGpsRow();          // no-op si REC off
    checkLapCompletion();  // no-op si REC off
  }

  logDiagRow(); // no-op si REC off ou avant l'intervalle -- volontairement hors du bloc newGpsData, cf. commentaire pres de sa definition

  // Idem : recalcul du debit RMC reel, hors du bloc newGpsData (doit
  // tourner meme si le GPS ne recoit plus rien du tout -- delta=0 =>
  // gpsMeasuredRmcHz=0, plus informatif que de rester fige sur l'ancienne
  // valeur). Tourne en continu, pas seulement pendant l'enregistrement --
  // alimente aussi l'ecran Connexion en direct.
  {
    unsigned long nowMsRmc = millis();
    if (nowMsRmc - lastRmcHzUpdateMs >= RMC_HZ_UPDATE_INTERVAL_MS) {
      unsigned long elapsed = nowMsRmc - lastRmcHzUpdateMs;
      uint32_t delta = gpsRmcSentenceCount - lastRmcCountSnapshot; // arithmetique non signee -- correct meme en cas de wrap (~13 ans a 10Hz, jamais en pratique)
      gpsMeasuredRmcHz = elapsed > 0 ? (delta * 1000.0f / (float)elapsed) : 0.0f;
      lastRmcCountSnapshot = gpsRmcSentenceCount;
      lastRmcHzUpdateMs = nowMsRmc;
    }
  }

  if (lvglLock(10)) {
    if (pushButtonPressed) {
      pushButtonPressed = false;
      handlePush(); // instantane -- plus de mode Pause a proteger (05/08)
    }
    if (backButtonPressed) {
      backButtonPressed = false;
      if (currentScreen == SCR_STATUS && recordingEnabled) {
        // Cas a risque (cf. commentaire pres de BACK_HOLD_STOP_MS) --
        // n'execute pas tout de suite, arme juste la verification.
        backHoldStopPendingSinceMs = millis();
      } else {
        handleBack();
      }
    }
    // Verifie si un appui BACK en attente de confirmation (ci-dessus) a
    // bien tenu assez longtemps -- filtre les impulsions parasites plus
    // courtes qu'un vrai appui volontaire. Tourne a chaque iteration tant
    // qu'une verification est en cours, independamment de backButtonPressed
    // (vrai uniquement au moment du front descendant lui-meme).
    if (backHoldStopPendingSinceMs != 0) {
      if (digitalRead(BACK_BUTTON) == HIGH) {
        backHoldStopPendingSinceMs = 0; // relache avant le delai -- faux contact probable, ignore
      } else if (millis() - backHoldStopPendingSinceMs >= BACK_HOLD_STOP_MS) {
        backHoldStopPendingSinceMs = 0;
        // Arret definitif direct (remplace l'ancien passage par
        // SCR_CONFIRM_STOP, cf. commentaire pres de BACK_HOLD_STOP_MS) --
        // reste sur SCR_STATUS, pas de changement d'ecran necessaire.
        if (recordingEnabled) {
          stopRecording();
          activateAutoMode(); // reset complet du courseManager + des flags d'armement
          lastDefinitiveStopMs = millis(); // cf. STATUS_REC_GRACE_AFTER_STOP_MS -- le geofencing peut rearmer le circuit en ~1s
        }
      }
    }

    // Extinction propre par appui long sur PWR (GPIO15) -- sans effet
    // sur USB, utile uniquement sur batterie.
    if (digitalRead(PWR_BUTTON) == LOW) {
      if (pwrHoldSinceMs == 0) {
        pwrHoldSinceMs = millis();
      } else if (millis() - pwrHoldSinceMs >= PWR_HOLD_OFF_MS) {
        Serial.println("PWR maintenu -- extinction.");
        if (recordingEnabled) {
          stopRecording(); // ferme proprement le fichier en cours avant de couper l'alim
        }
        delay(100); // laisse le temps au flush SD/LittleFS de se terminer
        BAT_OFF();
        delay(1000); // ne devrait pas etre atteint sur batterie (alim coupee) -- filet de securite si jamais sur USB
      }
    } else {
      pwrHoldSinceMs = 0;
    }
    // Rotation retiree avec le rotatif physique (27/07) -- l'anneau
    // (Circuit/Connexion/Session/Reglages) se navigue desormais par swipe
    // (cf. ringScreenGestureCb()), et la selection dans les listes par tap
    // direct (cf. commentaires "Tap: choisir" pres de chaque liste). Rien
    // ne remplace la mise en surbrillance par selection de
    // SCR_SESSION_LAPS (sessionLapSelection) -- ecran deja scrollable au
    // tactile (LV_DIR_VER), simple perte de confort, pas de blocage.
    lvglUnlock();
  }

  // WiFi.softAP()/stopDownloadMode() traites ICI, hors de tout verrou
  // LVGL -- ce sont des operations lentes (radio), jamais appelees
  // depuis un contexte verrouille (cf. commentaire pres des flags).
  if (wifiStartRequested) {
    wifiStartRequested = false;
    webServerManager.startDownloadMode();
  }
  if (wifiStopRequested) {
    wifiStopRequested = false;
    webServerManager.stopDownloadMode();
  }

  static unsigned long lastRender = 0;
  bool isLiveScreen = (currentScreen == SCR_STATUS || currentScreen == SCR_CONNEXION);
  bool timeToRender = isLiveScreen && (nowMs - lastRender >= 250);
  if (timeToRender || screenDirty) {
    lastRender = nowMs;
    if (lvglLock(10)) {
      refreshCurrentScreen(nowMs);
      lvglUnlock();
    }
  }
}
