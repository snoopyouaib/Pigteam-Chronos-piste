/**
 * Chrono GPS moto piste -- firmware ESP32-S3-AMOLED-1.91 (firmware_191)
 * ----------------------------------------------------------------------
 * Ex "display_only_191" (banc de test d'affichage 100% simule) --
 * devenu le vrai firmware : GPS reel (GpsManager), detection de
 * circuit/tour reelle (CourseManager, vendore depuis la lib publique
 * DovesLapTimer), sessions reelles (/sessions.csv, LittleFS), batterie
 * reelle (ADC interne), stockage SD reel (SdLogStorage, SDMMC).
 * WebServerManager (WiFi/telechargement) pas encore integre -- prochaine
 * etape.
 *
 * Ecran + encodeur EC11 (PUSH uniquement, cf. ci-dessous) + bouton BACK.
 *
 * Navigation (pas de menu liste separe) :
 *   - Depuis Statut : BACK -> ecran Circuit (1er ecran de l'anneau)
 *   - Anneau Circuit -> Nouveau circuit -> Connexion -> Session -> Reglages
 *     -> Circuit... (rotation encodeur ou swipe tactile)
 *   - Sur Circuit/Session/Reglages : PUSH entre en mode selection
 *     (l'encodeur change de role, selectionne un element de la liste),
 *     re-PUSH valide -- ou tap direct sur une ligne (equivalent en un
 *     seul geste).
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
#include <AiEsp32RotaryEncoder.h>
#include <limits.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <LittleFS.h>
#include <CourseManager.h>

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "display_bsp.h"
#include "fonts_teko.h"
#include "splash_pigteam.h"
#include "GpsManager.h"
#include "adc_bsp.h"
#include "SdLogStorage.h"
#include "WebServerManager.h"

// ===================== Pins (valides au bring-up 1.91) =====================
#define ENCODER_CLK   2
#define ENCODER_DT    3
#define ENCODER_PUSH  10
#define BACK_BUTTON   14

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ENCODER_DT, ENCODER_CLK, ENCODER_PUSH, -1, 4);
volatile bool backButtonPressed = false;
static unsigned long lastBackIsrMs = 0;
static const unsigned long BACK_DEBOUNCE_MS = 200;

void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}
void IRAM_ATTR backButtonISR() {
  unsigned long now = millis();
  if (now - lastBackIsrMs < BACK_DEBOUNCE_MS) return;
  lastBackIsrMs = now;
  backButtonPressed = true;
}

// mm:ss.mmm
static void formatLapTime(unsigned long ms, char* buf, size_t bufSize) {
  if (ms == 0) { snprintf(buf, bufSize, "--:--.---"); return; }
  unsigned long minutes = ms / 60000;
  unsigned long seconds = (ms / 1000) % 60;
  unsigned long millisPart = ms % 1000;
  snprintf(buf, bufSize, "%lu:%02lu.%03lu", minutes, seconds, millisPart);
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

// Inverse de formatLapTime() -- "M:SS.mmm" -> millisecondes. ULONG_MAX si
// le format ne correspond pas (ex. "--:--.---" = pas de temps).
static unsigned long parseLapTimeStr(const String& s) {
  int colon = s.indexOf(':');
  int dot = s.indexOf('.');
  if (colon < 0 || dot < 0) return ULONG_MAX;
  long minutes = s.substring(0, colon).toInt();
  long seconds = s.substring(colon + 1, dot).toInt();
  long millisPart = s.substring(dot + 1).toInt();
  if (minutes == 0 && seconds == 0 && millisPart == 0 && s.charAt(0) != '0') return ULONG_MAX;
  return (unsigned long)(minutes * 60000 + seconds * 1000 + millisPart);
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
static bool detectionEffectivelyComplete() {
  return manualOverrideActive || courseManager->isDetectionComplete();
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

    String fld[6];
    if (splitCsvLine(line, fld, 6) < 6) continue;
    LapDetail lap;
    lap.lapNumber = fld[2].toInt();
    lap.lapMs = parseLapTimeStr(fld[3]);
    lap.circuit = fld[5];
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
static char currentSessionCompactKey[16] = "";
static bool recordingEnabled = false;

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
  logFile.println("local_time,millis_boot,lat,lng,speed_kmh,fix,sats,laps,circuit,current_lap_ms");
  logFile.flush();
  loggingOk = true;
  recordingEnabled = true;
  lastLapCount = 0;
  appendSessionLine(String("# session demarree ") + currentSessionCompactKey);
  Serial.printf("REC ON : %s\n", currentLogPath);
}

static void stopRecording() {
  if (!recordingEnabled) return;
  if (loggingOk) { logFile.flush(); logFile.close(); loggingOk = false; }
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

  logFile.printf("%s,%lu,%.7f,%.7f,%.1f,%u,%u,%d,%s,%lu\n",
                 timeBuf, millis(), lat, lng, speedKmh,
                 gpsFixStatus, gpsNumSVs, lapsCount,
                 getActiveCourseNameForDisplay(), currentLapMs);

  static unsigned long lastFlush = 0;
  if (millis() - lastFlush >= 5000) { lastFlush = millis(); logFile.flush(); }
}

static void checkLapCompletion() {
  if (!recordingEnabled) return;
  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);
  if (lapsCount <= lastLapCount) return;
  lastLapCount = lapsCount;

  char dateBuf[12], timeBuf[10];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  char lapBuf[16], bestBuf[16];
  formatLapTime(getLastFinishedLapMs(), lapBuf, sizeof(lapBuf));
  formatLapTime(hasBest ? bestLapMs : 0, bestBuf, sizeof(bestBuf));

  char line[128];
  snprintf(line, sizeof(line), "%s,%s,%d,%s,%s,%s",
           dateBuf, timeBuf, lapsCount, lapBuf, bestBuf, getActiveCourseNameForDisplay());
  appendSessionLine(line);
  Serial.printf("Tour %d enregistre : %s (meilleur : %s)\n", lapsCount, lapBuf, bestBuf);
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

static void printSerialCommandsHelp() {
  Serial.println("=== Commandes Serial disponibles (tape la lettre, pas besoin d'Entree) ===");
  Serial.println("  l : liste les logs GPS detailles (SD ou LittleFS)");
  Serial.println("  d : dump du log en cours/dernier ferme");
  Serial.println("  s : dump du carnet de sessions (/sessions.csv)");
  Serial.println("  c : efface tous les logs GPS detailles");
  Serial.println("  x : efface le carnet de sessions (/sessions.csv)");
  Serial.println("  b : tension/pourcentage batterie");
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

  } else if (c == '?' || c == 'h') {
    printSerialCommandsHelp();
  }
}

// ===================== Navigation =====================

enum AppScreen { SCR_STATUS, SCR_CIRCUIT, SCR_CONNEXION, SCR_SESSION_LIST, SCR_SESSION_LAPS, SCR_SETTINGS, SCR_WIFI, SCR_NEW_CIRCUIT, SCR_CONFIRM_STOP };
static AppScreen currentScreen = SCR_STATUS;
// Duree max sur SCR_CONFIRM_STOP avant arret definitif automatique --
// filet de securite si on oublie de valider (BACK) ou de reprendre
// (tactile/PUSH) en quittant la piste. Sans ca, l'ecran resterait arme
// indefiniment et un faux contact tactile pourrait relancer
// l'enregistrement (cf. bug du 27/07 -- circuit jamais desarme apres stop).
static const unsigned long CONFIRM_STOP_TIMEOUT_MS = 120000UL; // 2 minutes
static unsigned long confirmStopEnteredMs = 0;
// Faux contact electrique (bruit sur l'alim, pas forcement un vrai doigt)
// = evenement quasi instantane. Un vrai choix humain de REPRENDRE prend
// toujours au moins quelques centaines de ms de reaction -- on ignore donc
// REPRENDRE (tactile/PUSH) pendant cette fenetre courte apres l'ouverture
// de l'ecran. BACK (arret definitif) n'est PAS concerne : c'est toujours
// la direction "sure", pas besoin de la retarder.
static const unsigned long CONFIRM_STOP_INPUT_GRACE_MS = 600UL;
static bool selectingMode = false; // true = encodeur en mode "selection dans la liste" (Circuit/Session/Reglages)

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
static lv_obj_t* scrConfirmStop;

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
  rotaryEncoder.setBoundaries(0, 4, true);
  rotaryEncoder.setEncoderValue(ringIndex);
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
    case SCR_CONFIRM_STOP: lv_scr_load(scrConfirmStop); break;
  }
}

static void goToRingScreen(int idx) {
  // idx: 0=Circuit,1=NouveauCircuit,2=Connexion,3=Session,4=Reglages --
  // appele quand l'encodeur tourne pour changer d'ecran dans l'anneau
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
// en complement de la rotation encodeur. Ignore en mode selection
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
static lv_obj_t* btnRec;
static lv_obj_t* lblBtnRecText;
static lv_obj_t* lblDernier;
static lv_obj_t* lblBest;
static lv_obj_t* lblTours;

// Tap sur le bouton REC (visible uniquement en mode "circuit detecte") --
// equivalent tactile du PUSH encodeur pour cette transition precise.
static void btnRecClickedCb(lv_event_t* e) {
  if (detectionEffectivelyComplete() && !recordingEnabled) {
    startRecording();
  }
}

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
  lv_obj_set_style_text_font(lblBig, &lv_font_teko_bold_84, 0);
  lv_obj_set_style_text_color(lblBig, lv_color_white(), 0);

  lblClock = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblClock, &lv_font_teko_bold_56, 0); // taille intermediaire (etait bold_84, trop gros)
  lv_obj_set_style_text_color(lblClock, lv_color_white(), 0);

  btnRec = lv_btn_create(scrStatus);
  lv_obj_set_size(btnRec, 220, 70);
  lv_obj_set_style_bg_color(btnRec, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_set_style_bg_color(btnRec, lv_palette_darken(LV_PALETTE_RED, 2), LV_STATE_PRESSED); // retour visuel a l'appui
  lv_obj_set_style_radius(btnRec, 14, 0);
  lv_obj_add_event_cb(btnRec, btnRecClickedCb, LV_EVENT_CLICKED, NULL);

  lblBtnRecText = lv_label_create(btnRec);
  lv_obj_set_style_text_font(lblBtnRecText, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lblBtnRecText, lv_color_white(), 0);
  lv_label_set_text(lblBtnRecText, "REC");
  lv_obj_center(lblBtnRecText);

  lblDernier = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblDernier, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lblDernier, lv_color_white(), 0);
  lv_obj_align(lblDernier, LV_ALIGN_TOP_LEFT, 4, 168);

  lblBest = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblBest, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lblBest, lv_color_white(), 0);
  lv_obj_align(lblBest, LV_ALIGN_TOP_LEFT, 4, 204);

  lblTours = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblTours, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lblTours, lv_color_white(), 0);
  lv_obj_align(lblTours, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
}

// ===================== Confirmation d'arret (a la RaceChrono) =====================
// BACK depuis SCR_STATUS pendant l'enregistrement ne stoppe plus
// directement -- il ouvre cet ecran. Un tap sur REPRENDRE (ou le PUSH
// encodeur, equivalent materiel) relance aussitot, le circuit etant reste
// arme (courseManager pas reset par stopRecording()). BACK confirme
// l'arret definitif (desarme le circuit, cf. handleBack()). Un timeout de
// securite (CONFIRM_STOP_TIMEOUT_MS) confirme automatiquement l'arret si
// on oublie de choisir avant de prendre la route -- cf. discussion sur le
// faux contact tactile en voiture qui relancait l'enregistrement.
static lv_obj_t* lblConfirmCountdown;

static void btnReprendreClickedCb(lv_event_t* e) {
  if (millis() - confirmStopEnteredMs < CONFIRM_STOP_INPUT_GRACE_MS) return; // cf. commentaire pres de la constante
  startRecording();
  goToScreen(SCR_STATUS);
}

static void buildConfirmStopScreen() {
  scrConfirmStop = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrConfirmStop, lv_color_black(), 0);
  lv_obj_clear_flag(scrConfirmStop, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lblTitle = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblTitle, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lblTitle, lv_palette_main(LV_PALETTE_ORANGE), 0);
  lv_label_set_text(lblTitle, "Enregistrement en pause");
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t* btnReprendre = lv_btn_create(scrConfirmStop);
  lv_obj_set_size(btnReprendre, 320, 90);
  lv_obj_set_style_bg_color(btnReprendre, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_set_style_bg_color(btnReprendre, lv_palette_darken(LV_PALETTE_GREEN, 2), LV_STATE_PRESSED);
  lv_obj_set_style_radius(btnReprendre, 14, 0);
  lv_obj_align(btnReprendre, LV_ALIGN_CENTER, 0, -10);
  lv_obj_add_event_cb(btnReprendre, btnReprendreClickedCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lblBtnReprendre = lv_label_create(btnReprendre);
  lv_obj_set_style_text_font(lblBtnReprendre, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lblBtnReprendre, lv_color_white(), 0);
  lv_label_set_text(lblBtnReprendre, "REPRENDRE");
  lv_obj_center(lblBtnReprendre);

  lv_obj_t* lblBack = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblBack, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lblBack, lv_color_white(), 0);
  lv_label_set_text(lblBack, "PRESS BACK pour stop definitif");
  lv_obj_align(lblBack, LV_ALIGN_CENTER, 0, 60);

  lblConfirmCountdown = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblConfirmCountdown, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lblConfirmCountdown, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(lblConfirmCountdown, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void refreshConfirmStopScreen() {
  long remainingS = (long)(CONFIRM_STOP_TIMEOUT_MS - (millis() - confirmStopEnteredMs)) / 1000;
  if (remainingS < 0) remainingS = 0;
  char buf[32];
  snprintf(buf, sizeof(buf), "Arret auto dans %lds", remainingS);
  lv_label_set_text(lblConfirmCountdown, buf);
}

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
    snprintf(buf, sizeof(buf), "%d km/h", (int)gpsSpeedKmh);
    lv_label_set_text(lblBig, buf);
    lv_obj_align(lblBig, LV_ALIGN_TOP_MID, 0, 40);

    char timeBuf[10];
    getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));
    lv_label_set_text(lblClock, timeBuf);
    lv_obj_align(lblClock, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_clear_flag(lblClock, LV_OBJ_FLAG_HIDDEN);

    if (circuitDetected) {
      lv_obj_align(btnRec, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
      lv_obj_clear_flag(btnRec, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(btnRec, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(lblDernier, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblBest, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lblTours, LV_OBJ_FLAG_HIDDEN);

  } else {
    if (currentLapMs > 0) {
      // Ligne franchie (getRaceStarted() true cote CourseManager/DovesLapTimer) -- chrono actif.
      formatLapTime(currentLapMs, buf, sizeof(buf));
    } else {
      // REC actif mais course pas encore demarree -- vitesse plus utile qu'un chrono fige.
      snprintf(buf, sizeof(buf), "%d km/h", (int)gpsSpeedKmh);
    }
    lv_label_set_text(lblBig, buf);
    lv_obj_align(lblBig, LV_ALIGN_TOP_MID, 0, 50);

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
    lv_obj_add_flag(btnRec, LV_OBJ_FLAG_HIDDEN);
  }
}

// ===================== Helpers d'ecran =====================
//
// createListRow : medium_34 (agrandi, etait medium_26) -- pour les
// listes courtes (Circuit ~5 items, Reglages 1 item) qui ont la place.
// createListRowSmall : medium_26 -- pour les listes qui peuvent avoir
// jusqu'a 8 lignes (Session/Tours), ou medium_34 deborderait.
// createHint : deplace a cote du titre (etait en bas d'ecran) -- libere
// toute la hauteur restante pour les lignes de liste, plus grandes.

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

static lv_obj_t* createHint(lv_obj_t* parent) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, &lv_font_teko_medium_26, 0);
  lv_obj_set_style_text_color(lbl, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -4, 4); // a cote du titre (etait en bas d'ecran)
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
  lv_obj_set_size(cont, 536, 240 - yTop - bottomMargin);
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
static lv_obj_t* lblCircuitHint;

static void buildCircuitScreen() {
  scrCircuit = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrCircuit, lv_color_black(), 0);
  enableRingSwipe(scrCircuit);
  createTitle(scrCircuit, "Circuit");

  circuitListCont = createScrollList(scrCircuit, 44); // pleine hauteur -- plus de bouton en bas, "Nouveau circuit" a son propre ecran dans l'anneau
  lblCircuitHint = createHint(scrCircuit);
}

static void circuitRowTappedCb(lv_event_t* e) {
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
  lv_label_set_text(lblCircuitHint, selectingMode ? "PUSH: valider  BACK: annuler  (ou tap direct)" : "Tap: choisir   BACK: statut");
}

// ===================== Ecran Connexion =====================

static lv_obj_t* lblConnGps;
static lv_obj_t* lblConnCircuit;

static void buildConnexionScreen() {
  scrConnexion = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrConnexion, lv_color_black(), 0);
  enableRingSwipe(scrConnexion);
  createTitle(scrConnexion, "Connexion");

  lblConnGps = createListRow(scrConnexion, 44);
  lblConnCircuit = createListRow(scrConnexion, 84);
  lv_obj_t* hint = createHint(scrConnexion);
  lv_label_set_text(hint, "BACK: statut");
}

static void refreshConnexionScreen() {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s  Fix:%d  Sat:%d", gpsActive ? "GPS OK" : "GPS --", gpsFixStatus, gpsNumSVs);
  lv_label_set_text(lblConnGps, buf);
  lv_label_set_text(lblConnCircuit, getActiveCourseNameForDisplay());
}

// ===================== Ecran Session (liste) =====================

static lv_obj_t* sessionListCont;
static lv_obj_t* lblSessionEmpty;
static lv_obj_t* lblSessionHint;
static std::vector<SessionSummary> sessionListCache;

static void buildSessionListScreen() {
  scrSessionList = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSessionList, lv_color_black(), 0);
  enableRingSwipe(scrSessionList);
  createTitle(scrSessionList, "Sessions");

  sessionListCont = createScrollList(scrSessionList, 44);
  lblSessionEmpty = createListRow(scrSessionList, 44);
  lblSessionHint = createHint(scrSessionList);
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

  lv_label_set_text(lblSessionHint, selectingMode ? "PUSH: voir tours  BACK: annuler" : "Tap: voir les tours  BACK: statut");
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
static lv_obj_t* lblLapHint;
static std::vector<LapDetail> lapListCache;

// Factorisee -- utilisee a la fois par handleBack() (bouton physique)
// et par le nouveau bouton tactile de retour (haut droite de l'ecran).
static void backFromSessionLapsToList() {
  rotaryEncoder.setBoundaries(0, (int)sessionListCache.size() - 1, false);
  rotaryEncoder.setEncoderValue(sessionListSelection);
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
  lblLapHint = createHint(scrSessionLaps);
  lv_obj_align(lblLapHint, LV_ALIGN_TOP_RIGHT, -4, 48); // sous le bouton retour, pas de chevauchement
  lv_label_set_text(lblLapHint, "Glisse: defile  BACK: retour");
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
    char buf[48];
    snprintf(buf, sizeof(buf), "%sTour %d : %s%s", sel ? "> " : "  ", lap.lapNumber, lapBuf, isBest ? "  (best)" : "");
    lv_label_set_text(row, buf);
  }
}


// ===================== Ecran Reglages =====================

static lv_obj_t* lblSettingsRow;
static lv_obj_t* lblSettingsHint;

static void settingsRowTappedCb(lv_event_t* e) {
  selectingMode = false;
  wifiStartRequested = true; // traite dans loop(), pas ici (cf. commentaire pres de la declaration)
  goToScreen(SCR_WIFI); // tap direct = ouvre WiFi en un seul geste
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
  lblSettingsHint = createHint(scrSettings);
}

static void refreshSettingsScreen() {
  bool sel = selectingMode;
  lv_obj_set_style_text_color(lblSettingsRow, sel ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
  lv_label_set_text(lblSettingsRow, sel ? "> WiFi telechargement" : "  WiFi telechargement");
  lv_label_set_text(lblSettingsHint, sel ? "PUSH: ouvrir   BACK: annuler" : "Tap: ouvrir   BACK: statut");
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

  lv_obj_t* hint = createHint(scrWifi);
  lv_label_set_text(hint, "BACK: statut"); // raccourci -- la version longue debordait sur le titre
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

  lv_obj_t* hint = createHint(scrNewCircuit);
  lv_label_set_text(hint, "BACK: statut");
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
    case SCR_CONFIRM_STOP: refreshConfirmStopScreen(); break;
  }
  screenDirty = false;
}

// ===================== Gestion PUSH / BACK selon l'ecran courant =====================

// Applique la selection d'un item Circuit (index i) -- factorise pour
// etre appelee aussi bien par la validation PUSH (2 temps, encodeur)
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
  const SessionSummary* s = selectedSession();
  int lapCount = s ? s->lapCount : 0;
  rotaryEncoder.setBoundaries(0, lapCount > 0 ? lapCount - 1 : 0, false);
  rotaryEncoder.setEncoderValue(0);
  goToScreen(SCR_SESSION_LAPS);
  screenDirty = true;
}

static void handlePush() {
  switch (currentScreen) {
    case SCR_STATUS:
      if (!recordingEnabled && detectionEffectivelyComplete()) { // BACK arrete desormais (pas PUSH), cf. handleBack
        startRecording();
      }
      break;

    case SCR_CONFIRM_STOP:
      // Equivalent materiel du tap sur REPRENDRE -- cf. btnReprendreClickedCb.
      if (millis() - confirmStopEnteredMs < CONFIRM_STOP_INPUT_GRACE_MS) break; // cf. commentaire pres de la constante
      startRecording();
      goToScreen(SCR_STATUS);
      break;

    case SCR_CIRCUIT:
      if (!selectingMode) {
        selectingMode = true;
        rotaryEncoder.setBoundaries(0, myTracks.courseCount, true);
        rotaryEncoder.setEncoderValue(circuitSelection);
        screenDirty = true;
      } else {
        applyCircuitSelection(circuitSelection); // valide la selection encodeur (2e PUSH)
      }
      break;

    case SCR_CONNEXION:
      break; // rien a selectionner ici

    case SCR_SESSION_LIST:
      if (!sessionListCache.empty()) {
        if (!selectingMode) {
          selectingMode = true;
          rotaryEncoder.setBoundaries(0, (int)sessionListCache.size() - 1, false);
          rotaryEncoder.setEncoderValue(sessionListSelection);
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
        rotaryEncoder.setBoundaries(0, 0, false);
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
      if (recordingEnabled) {
        // BACK ne stoppe plus directement -- passe par un ecran de
        // confirmation (cf. SCR_CONFIRM_STOP). stopRecording() ici agit
        // comme une pause : le fichier de log est ferme mais le circuit
        // reste arme (courseManager pas reset), pour permettre une reprise
        // rapide si c'etait un arret involontaire/entre deux runs.
        stopRecording();
        confirmStopEnteredMs = millis();
        goToScreen(SCR_CONFIRM_STOP);
      } else {
        cancelNewCircuitCapture(); // no-op si rien n'est arme
        goToScreen(SCR_CIRCUIT);
      }
      break;

    case SCR_CONFIRM_STOP:
      // Arret definitif : desarme le circuit (auto ou force manuellement)
      // pour qu'aucun faux contact ne puisse relancer l'enregistrement une
      // fois qu'on a quitte la piste. Le timeout dans loop() couvre le cas
      // ou on oublie de confirmer avant de prendre la route.
      activateAutoMode(); // reset complet du courseManager + des flags d'armement
      goToScreen(SCR_STATUS);
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
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== firmware_191 -- GPS/CourseManager/SD reels (Circuit/Nouveau circuit/Connexion/Session/Reglages) ===");

  I2C_master_Init();
  Serial.println("[1/5] I2C partage ok");
  Touch_Init();
  Serial.println("[2/5] Tactile ok");
  displayInit();
  Serial.println("[3/5] Ecran + LVGL ok");

  initGps();
  Serial.println("[3.5/5] GPS ok (reel -- plus simule)");

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: echec de montage (meme apres formatage) -- circuits/sessions indisponibles.");
  }
  bool sdOk = initSdLogStorage();
  Serial.printf("SD (SDMMC) : %s -- logs GPS detailles sur %s.\n", sdOk ? "montee" : "absente/en panne", sdOk ? "carte SD" : "LittleFS (repli)");
  migrateLittleFsLogsToSd();

  loadActiveCircuitsIntoTracks();
  courseManager = new CourseManager(myTracks, 7.0, &Serial);

  webServerManager.begin("ChronoMotoAMOLED", SESSION_LOG_PATH, CIRCUITS_FILE_PATH, *gpsLogFs, nullptr, nullptr, flushLogsCallback, getStatusCallback);

  adc_bsp_init();

  pinMode(BACK_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BACK_BUTTON), backButtonISR, FALLING);
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, 100, false);
  rotaryEncoder.setAcceleration(0);
  Serial.println("[4/5] Encodeur + BACK ok");

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
    buildConfirmStopScreen();
    lvglUnlock();
  }
  Serial.println("[5/5] Ecrans construits, splash affiche");

  delay(3500); // laisse le splash visible plus longtemps avant de basculer sur le Statut (etait 2000ms)

  if (lvglLock(-1)) {
    lv_scr_load(scrStatus);
    lvglUnlock();
  }

  Serial.println("Pret. Statut: PUSH avance (recherche->detecte->REC), BACK arrete le REC ou va sur Circuit. Anneau: tourne = change d'ecran, PUSH = selection, BACK = retour.");
  printSerialCommandsHelp();
}

void loop() {
  unsigned long nowMs = millis();
  pollGps();
  gpsUpdateFromLiveData();
  webServerManager.loop();
  handleSerialCommands();

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

  if (lvglLock(10)) {
    if (rotaryEncoder.isEncoderButtonClicked()) {
      handlePush();
    }
    if (backButtonPressed) {
      backButtonPressed = false;
      handleBack();
    }
    if (rotaryEncoder.encoderChanged()) {
      int val = rotaryEncoder.readEncoder();
      if (!selectingMode && (currentScreen == SCR_CIRCUIT || currentScreen == SCR_CONNEXION ||
                             currentScreen == SCR_SESSION_LIST || currentScreen == SCR_SETTINGS)) {
        goToRingScreen(val);
      } else if (selectingMode && currentScreen == SCR_CIRCUIT) {
        circuitSelection = val;
        screenDirty = true;
      } else if (selectingMode && currentScreen == SCR_SESSION_LIST) {
        sessionListSelection = val;
        screenDirty = true;
      } else if (currentScreen == SCR_SESSION_LAPS) {
        sessionLapSelection = val;
        screenDirty = true;
      }
    }
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

  if (currentScreen == SCR_CONFIRM_STOP &&
      (nowMs - confirmStopEnteredMs) >= CONFIRM_STOP_TIMEOUT_MS) {
    // Timeout de securite : personne n'a choisi (BACK ou REPRENDRE) --
    // on confirme l'arret tout seul plutot que de laisser cet ecran arme
    // indefiniment (cf. commentaire pres de CONFIRM_STOP_TIMEOUT_MS).
    activateAutoMode();
    goToScreen(SCR_STATUS);
  }

  static unsigned long lastRender = 0;
  bool isLiveScreen = (currentScreen == SCR_STATUS || currentScreen == SCR_CONNEXION ||
                       currentScreen == SCR_CONFIRM_STOP); // decompte du timeout -- doit se rafraichir seul
  bool timeToRender = isLiveScreen && (nowMs - lastRender >= 250);
  if (timeToRender || screenDirty) {
    lastRender = nowMs;
    if (lvglLock(10)) {
      refreshCurrentScreen(nowMs);
      lvglUnlock();
    }
  }
}
