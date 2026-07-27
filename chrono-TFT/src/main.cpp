/**
 * Firmware Chrono GPS moto piste -- variante TFT ST7789 (320x240) + EC11 + 1 bouton
 * ---------------------------------------------------------------------------------
 * Ecran statut + menu principal (Circuit / Connexion / Session / Reglages
 * / New track), geofencing (reconnaissance instantanee d'un circuit
 * connu au 1er fix GPS), CourseManager (detection auto/proximite),
 * logs CSV (session + carnet), WebServerManager (WiFi/telechargement,
 * module partage avec la variante OLED), monitoring batterie (GPIO1,
 * cf. plan de brochage ci-dessous). Cf. README.md pour le detail de
 * chaque brique.
 */

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <limits.h>
#include <vector>
#include <algorithm>
#include <LittleFS.h>
#include <CourseManager.h>
#include "GpsManager.h"
#include <WiFi.h>
#include "WebServerManager.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <AiEsp32RotaryEncoder.h>
#include <SD.h>
#include "SdLogStorage.h"
#include "splash_pigteam.h"

// splash_cochon.h et splash_full.h retires du projet (identiques a
// splash_pigteam.h une fois affiches -- inutile de garder les 3).

// Polices Teko (Google Fonts, licence SIL Open Font License -- cf.
// fonts/OFL-Teko.txt), convertie en GFXfont via l'outil fontconvert
// d'Adafruit -- portees depuis le sous-projet display_only (banc de test
// affichage, cf. son README pour le detail de calibration des tailles).
#include "fonts/TekoBold40pt7b.h"    // gros chrono (mode REC) / grosse vitesse (recherche/detecte)
#include "fonts/TekoBold21pt7b.h"    // PRESS REC (bas d'ecran, clignotant)
#include "fonts/TekoMedium20pt7b.h"  // heure (recherche/detecte) / Dernier+Best (mode REC)
#include "fonts/TekoMedium15pt7b.h"  // nom du circuit, REC/batterie (coin haut droit)
#include "fonts/TekoRegular11pt7b.h" // texte fin (ligne GPS, "Tours: N")

// ===================== Pins (TEST -- migration vers ESP32-S3-Tiny N8R8) =====================
//
// Nouveau plan de brochage pour le montage final (ESP + TFT soudes
// directement, module USB amovible) -- remplace le plan S3-Zero
// ci-dessus le temps de ce test. Cote droit du Tiny (GPIO 11-18) +
// broches TX/RX dediees, reaffectees ici a BACK/PUSH (au lieu du GPS,
// cf. note ci-dessous) :
//   GPIO 11 : TFT SCL (SCLK)
//   GPIO 12 : TFT SDA (MOSI)
//   GPIO 13 : TFT RES (RST)
//   GPIO 14 : TFT DC
//   GPIO 15 : TFT CS
//   GPIO 16 : TFT BLK (retroeclairage)
//   GPIO 17 : Encodeur A (CLK)
//   GPIO 18 : Encodeur B (DT)
//   TX (GPIO43, U0TXD -- chip pin 49 sur le boitier QFN56) : BACK (K0)
//   RX (GPIO44, U0RXD -- chip pin 50) : Encodeur PUSH
//
// GPS DESACTIVE TEMPORAIREMENT pour ce test (pas branche sur cette
// maquette) -- ses pins habituelles (TX/RX) sont justement celles
// reutilisees ci-dessus pour BACK/PUSH, d'ou le conflit si les deux
// etaient actifs en meme temps. Repere GPS_ENABLED plus bas (initGps()/
// pollGps()) pour le reactiver une fois de nouvelles broches choisies
// et le module reconnecte. SD (GPIO4/5) et batterie (GPIO1) laisses en
// l'etat (plan S3-Zero, pins non reaffectees ici) -- non cables sur
// cette maquette non plus, mais sans conflit avec le nouveau plan
// ci-dessus donc pas besoin de les desactiver activement (SD.begin()
// echoue simplement et retombe sur LittleFS, lecture batterie flottante
// sans consequence).

#define ENCODER_CLK   17   // A
#define ENCODER_DT    18   // B
#define ENCODER_PUSH  44   // RX -- clic axe -- valider / REC
#define BACK_BUTTON   43   // TX -- bouton unique -- retour / menu

// GPS -- module extrait dans GpsManager.h/.cpp (mutualise avec la
// variante OLED, memes GpsData/liveData/newGpsData/gpsActive/initGps()/
// pollGps() -- cf. ce fichier pour le detail cablage/protocole).
// Reactive -- cable sur GPIO5/6 (cf. GpsManager.cpp pour le detail et le
// sens RX/TX).
#define GPS_ENABLED 1

// TFT -- pins passes en arguments au constructeur Adafruit_ST7789 (pas de
// build_flags ici, contrairement a TFT_eSPI -- cf. note platformio.ini sur
// le changement de bibliotheque).
#define TFT_MOSI 12
#define TFT_SCLK 11
#define TFT_CS   15
#define TFT_DC   14
#define TFT_RST  13
#define TFT_BL   16

// SD -- bus SPI DEDIE (HSPI, second bus materiel de l'ESP32-S3, en plus
// du FSPI deja utilise par l'ecran) -- ne partage plus MOSI/SCLK avec le
// TFT comme sur le plan precedent (S3-Zero), la maquette Tiny dispose de
// suffisamment de GPIO libres pour se le permettre. Ordre MOSI/MISO/
// SCLK/CS choisi arbitrairement (convention SPI standard) -- a corriger
// facilement si le cablage reel differe, aucune contrainte particuliere
// sur ces 4 broches.
#define SD_MOSI  7
#define SD_MISO  8
#define SD_SCLK  9
#define SD_CS    10

// Batterie -- dernier GPIO libre, pont diviseur 10k/10k entre BAT et GND
// (jonction sur ce pin). ADC1_CH0 sur l'ESP32-S3.
#define BATTERY_ADC_PIN 1

// ===================== Ecran (Adafruit_ST7789) =====================
//
// Bus SPI dedie (pas le bus par defaut) car nos pins MOSI/SCLK/CS ne sont
// pas les pins FSPI par defaut de l'ESP32-S3 -- necessaire pour utiliser
// des pins custom avec le SPI materiel sur ce coeur Arduino-ESP32.
SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// Bus SPI dedie a la SD (HSPI) -- separe de tftSPI depuis le passage a
// des pins dedies (cf. note SD_MOSI/SD_MISO/SD_SCLK/SD_CS plus haut).
SPIClass sdSPI(HSPI);

// gpsLogFs/gpsLogsOnSd/migrateLittleFsLogsToSd()/sdUsedBytes()/
// sdTotalBytes() vivent desormais dans SdLogStorage.h/.cpp -- module
// partage tel quel avec la variante OLED (cf. README_FS.md). Ce fichier
// ne garde que ce qui differe structurellement selon la variante : ici,
// le bus SPI est PARTAGE avec l'ecran (tftSPI, deja en SPIClass(FSPI)),
// contrairement a l'OLED qui a son propre bus SPI dedie (ecran en I2C).
// SdLogStorage ne fait jamais spi.begin() lui-meme -- ce cablage reste
// ici (cf. tftSPI.begin() plus bas, dans setup()).

// ===================== Double buffering (canvas hors-ecran) =====================
//
// Toutes les fonctions d'affichage des ecrans (statut, menus, sessions,
// WiFi...) dessinent maintenant dans ce canvas en RAM plutot que
// directement sur l'ecran physique -- meme principe que la Pendule
// Paddock/OLED (composer l'image complete hors-ecran, puis l'envoyer en
// un seul bloc). Elimine le clignotement residuel qui subsistait malgre
// les optimisations precedentes (largeur fixe, uiDirty) : celles-ci
// reduisaient la FREQUENCE des dessins visibles, mais chaque dessin
// restait visible en train de se faire (effacement puis reecriture
// directement sur l'ecran). Avec le canvas, l'ecran physique ne recoit
// plus jamais qu'une image deja complete (cf. pushCanvasToDisplay(),
// appelee une seule fois a la fin de chaque rendu dans loop()).
//
// 320x240 x 2 octets/pixel (RGB565) = 150KB -- alloue automatiquement en
// PSRAM par le malloc() interne de GFXcanvas16 (le N8R8 a 8MB de PSRAM
// octale, deja active via qio_opi dans platformio.ini -- confirme au
// banc, pas besoin de configuration supplementaire).
GFXcanvas16 canvas(320, 240);

// Copie le canvas vers l'ecran physique en un seul flux SPI continu --
// meme technique que drawRgb565Splash() (setAddrWindow + writePixels
// dans un seul startWrite()/endWrite() NON imbrique). A l'epoque du
// splash, writePixels() semblait donner de mauvaises couleurs -- mais
// c'etait en realite le bug d'inversion d'ecran (cf. tft.invertDisplay()
// plus bas), corrige depuis ; writePixels() fonctionne correctement.
static void pushCanvasToDisplay() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, canvas.width(), canvas.height());
  tft.writePixels(canvas.getBuffer(), canvas.width() * canvas.height());
  tft.endWrite();
}

// Splash -- nouvel export brut Piskel (splash_pigteam.h), format
// different des essais precedents : tableau de uint32_t en ARGB8888
// (alpha/rouge/vert/bleu, 8 bits chacun), pas du RGB565 "byte-swapped
// pour TFT_eSPI". Deja aux bonnes dimensions/orientation paysage
// (284x240, cf. SPLASH_PIGTEAM_FRAME_WIDTH/HEIGHT dans le .h) -- plus
// besoin de rotation logicielle. Conversion en RGB565 faite ici
// nous-memes (pas de swap16 -- ca n'a rien a voir avec le pipeline
// TFT_eSPI d'avant). Alpha < 128 = pixel transparent, laisse le fond
// noir (fillScreen prealable) visible a travers.
static void drawSplashArgb8888(const uint32_t* data, int16_t w, int16_t h) {
  int16_t x0 = (tft.width() - w) / 2;
  int16_t y0 = (tft.height() - h) / 2;
  for (int16_t row = 0; row < h; row++) {
    for (int16_t col = 0; col < w; col++) {
      uint32_t px = data[(uint32_t)row * w + col];
      uint8_t a = (px >> 24) & 0xFF;
      if (a < 128) continue; // transparent -- on laisse le fond noir
      // Format reel : 0xAABBGGRR (Piskel stocke R,G,B,A en memoire --
      // sur ce processeur little-endian, relu comme un uint32_t ca
      // inverse R et B par rapport a la lecture "naive" 0xAARRGGBB,
      // constate au banc : le cochon ressortait bleu au lieu de rose).
      uint8_t b = (px >> 16) & 0xFF;
      uint8_t g = (px >> 8) & 0xFF;
      uint8_t r = px & 0xFF;
      uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      tft.drawPixel(x0 + col, y0 + row, rgb565);
    }
  }
}

static void drawBootSplash() {
  tft.fillScreen(ST77XX_BLACK);
  drawSplashArgb8888(splash_pigteam_data[0], SPLASH_PIGTEAM_FRAME_WIDTH, SPLASH_PIGTEAM_FRAME_HEIGHT);
  delay(1800); // meme duree que sur la variante OLED
}

// ===================== Encodeur EC11 + bouton unique =====================

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ENCODER_DT, ENCODER_CLK, ENCODER_PUSH, -1, 4);

void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

// BACK lu par interruption materielle (comme sur la variante OLED) --
// evite de rater un appui court si la boucle principale est ralentie
// (webServerManager.loop() pendant le WiFi, une fois porte).
// FALLING (pas CHANGE) : ne declenche qu'a l'appui, pas au relachement --
// evite de compter le front montant comme un 2e appui.
volatile bool backButtonFlag = false;
static unsigned long lastBackIsrMs = 0;
static const unsigned long BACK_DEBOUNCE_MS = 200; // bouton bruyant sur le banc -- 30ms etait trop court, rebond mecanique capte comme plusieurs appuis

void IRAM_ATTR backButtonISR() {
  unsigned long now = millis();
  if (now - lastBackIsrMs < BACK_DEBOUNCE_MS) return; // anti-rebond
  lastBackIsrMs = now;
  backButtonFlag = true;
}

static bool pollBackPress() {
  if (backButtonFlag) {
    backButtonFlag = false;
    return true;
  }
  return false;
}

// ===================== Setup / Loop =====================

// ===================== Horodatage local (a partir du fix GPS, UTC -> heure locale) =====================
//
// Le GPS ne donne que l'heure UTC (liveData.year/month/.../second) --
// conversion vers l'heure locale via le TZ pose dans setup() (tzset()).
// utcTmToEpoch() est une implementation portable (algorithme
// "days_from_civil" de Howard Hinnant) plutot que mktime()/timegm(), qui
// dependent de la TZ du systeme et donneraient un resultat faux ici (TZ
// deja positionnee sur l'heure locale, pas UTC).
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

// ===================== CourseManager (detection auto / mode proximite) =====================
//
// Repris de la variante OLED. Charge circuits.csv, CourseManager
// detecte/chronometre -- cf. plus bas dans ce fichier pour le
// geofencing (reconnaissance instantanee au 1er fix), le mode manuel
// (menu Circuit), la capture de nouveau circuit et l'ecriture des logs,
// tous ajoutes par la suite sur cette base.

static const char* CIRCUITS_FILE_PATH = "/circuits.csv";
static const char* SESSION_LOG_PATH = "/sessions.csv"; // declare ici (avant loadSessionSummaries()/loadLapsForSession() plus bas, qui en ont besoin) plutot que pres du reste du bloc Enregistrement
static char courseNameBuf[MAX_COURSES][32]; // stockage stable pour CourseConfig::name (simple const char*, pas de copie interne cote lib)
static TrackConfig myTracks = { "Mes circuits PIGTEAM", "PIGTEAM", {}, 0 }; // rempli par loadActiveCircuitsIntoTracks()
static CourseManager* courseManager = nullptr;
static int lastLapCount = 0; // remis a zero a chaque changement de mode (geofencing, futur menu manuel...)

// Coupe une ligne CSV en champs -- generique, jusqu'a maxFields.
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

// Charge les circuits coches "actif" depuis /circuits.csv (uploade via
// `pio run -t uploadfs` -- cf. data/circuits.csv dans ce projet, copie
// de ton fichier reel) dans myTracks. Si le fichier est absent (uploadfs
// pas encore fait), demarre simplement avec 0 circuit connu -- le mode
// proximite ("Lap Anything") prend le relai automatiquement, rien ne
// plante.
static void loadActiveCircuitsIntoTracks() {
  if (!LittleFS.exists(CIRCUITS_FILE_PATH)) {
    Serial.println("Circuits: /circuits.csv absent -- as-tu fait 'pio run -t uploadfs' ? Demarrage en mode proximite uniquement.");
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
    if (firstLine) { firstLine = false; continue; } // ligne d'en-tete

    String fld[17];
    if (splitCsvLine(line, fld, 17) < 17) continue; // ligne malformee -- ignoree
    if (fld[0].toInt() != 1) continue; // pas coche actif

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

// ===================== Mode manuel (force un circuit precis, court-circuite la detection) =====================
//
// CourseManager n'expose pas de methode pour forcer directement un
// circuit -- seule la detection auto existe. Solution (identique a la
// variante OLED) : un DovesLapTimer independant, configure avec la ligne
// du circuit choisi. Quand actif, remplace entierement courseManager
// dans l'affichage/les logs. Ici, seul le geofencing peut l'activer (pas
// encore de menu manuel -- viendra avec le portage du menu circuit).
static DovesLapTimer manualTimer(7.0, &Serial);
static bool manualOverrideActive = false;
static int manualCourseIndex = -1;

// Etat de la capture automatique de nouveau circuit -- declare ici (avant
// activateManualCourse/activateAutoMode, qui doivent pouvoir le remettre
// a zero) plutot que pres des fonctions qui l'utilisent vraiment (cf.
// section "Capture automatique de nouveau circuit" plus bas).
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
  newCircuitCaptureArmed = false; // un circuit vient d'etre force -- abandonne toute capture en attente
  pendingNewCircuitCapture = false;
  Serial.printf("Circuit force (geofencing) : %s\n", c.name);
}

static bool geofenceCheckDone = false; // declare ici (avant activateAutoMode) -- utilise par les deux

// Retour en detection auto/proximite -- desactive le mode manuel (menu ou
// geofencing) et repart de zero. Utilise par le menu circuit ("Auto
// (detection)").
static void activateAutoMode() {
  manualOverrideActive = false;
  manualCourseIndex = -1;
  courseManager->reset();
  geofenceCheckDone = false; // autorise un nouveau check geofencing au prochain fix -- coherent avec un "reset" complet
  lastLapCount = 0;
  newCircuitCaptureArmed = false; // idem activateManualCourse() -- abandonne toute capture en attente
  pendingNewCircuitCapture = false;
  Serial.println("Retour en mode detection automatique.");
}

// ===================== Geofencing (reconnaissance quasi instantanee sur circuit connu) =====================
//
// L'algorithme natif de CourseManager ne connait pas la position des
// circuits, seulement leur longueur de tour -- il faut boucler au moins
// un tour avant de confirmer un circuit. Inutile si les circuits actifs
// sont espaces de plusieurs km : une simple distance a vol d'oiseau au
// premier fix GPS suffit a lever l'ambiguite, avant meme d'avoir bouge.
// Verifie une seule fois (au premier fix GPS valide).
static const float GEOFENCE_MAX_DISTANCE_M = 15000.0f; // 15km -- large marge

static void checkCircuitGeofence(double lat, double lng) {
  geofenceCheckDone = true;

  int bestIdx = -1;
  double bestDist = 1e18;
  for (int i = 0; i < myTracks.courseCount; i++) {
    CourseConfig& c = myTracks.courses[i];
    double midLat = (c.startALat + c.startBLat) / 2.0;
    double midLng = (c.startALng + c.startBLng) / 2.0;
    double d = geoHaversine(lat, lng, midLat, midLng); // geoHaversine vient de GeoMath.h, inclus transitivement via CourseManager.h
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

// ===================== Historique GPS court terme (pour calcul de cap) =====================
//
// Buffer circulaire des dernieres positions -- utilise uniquement pour
// estimer le cap de la route au moment ou un nouveau circuit est capture
// automatiquement (cf. section suivante). Alimente a chaque point GPS
// traite, quel que soit le mode -- cout negligeable (~640 octets fixes).
struct GpsHistoryPoint { double lat; double lng; unsigned long timeMs; };
static const int GPS_HISTORY_SIZE = 40; // ~4s a 10Hz, large marge pour la fenetre de 2s utilisee plus bas
static GpsHistoryPoint gpsHistory[GPS_HISTORY_SIZE];
static int gpsHistoryCount = 0;
static int gpsHistoryHead = 0; // prochain slot a ecrire

static void pushGpsHistory(double lat, double lng, unsigned long timeMs) {
  gpsHistory[gpsHistoryHead] = { lat, lng, timeMs };
  gpsHistoryHead = (gpsHistoryHead + 1) % GPS_HISTORY_SIZE;
  if (gpsHistoryCount < GPS_HISTORY_SIZE) gpsHistoryCount++;
}

// Point le plus ancien de l'historique respectant au moins windowMs
// d'ecart avec nowMs -- ou le plus ancien disponible si l'historique est
// plus court que la fenetre demandee.
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

// Cap (degres, 0=Nord/90=Est) et point de destination -- memes formules
// que generate_line.py (script compagnon PC, hors firmware), reimplementees
// ici pour la capture embarquee. Aucune dependance externe, juste
// <math.h> (deja disponible via Arduino.h).
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
  outLng = fmod(degrees(lambda2) + 540.0, 360.0) - 180.0; // normalise a [-180, 180]
}

// ===================== Capture automatique de nouveau circuit =====================
//
// Repris de la variante OLED. Declenchee UNIQUEMENT par l'entree de menu
// "Nouveau circuit (capture)" (cf. armNewCircuitCapture(), appelee depuis
// loop() -- remplace le bouton CONFIRM de l'OLED, absent ici) --
// volontairement pas sur un repli automatique en proximite. Le mode
// proximite/comptage de tours fonctionne pareil dans tous les cas ;
// seule l'ECRITURE d'un nouveau circuit dans circuits.csv est reservee au
// geste intentionnel (choisir l'entree de menu), pour eviter qu'une
// balade improvisee la ou rien n'est reconnu ne finisse par polluer la
// liste de circuits toute seule.
//
// Capture en DEUX temps, volontairement decouples :
//  1. Des que le waypoint interne de CourseManager est pose (position
//     reelle, on est encore physiquement dessus) -- capture du cap et
//     calcul de la ligne A/B perpendiculaire. Attendre le tour valide
//     pour ca donnerait un cap totalement faux (calcule a un endroit
//     different, potentiellement loin de la ligne).
//  2. Ecriture effective dans circuits.csv seulement au premier tour
//     VALIDE (getRaceStarted()) ET seulement si on est toujours en mode
//     proximite a ce moment (lapAnythingEffective()) -- si entre-temps un
//     vrai circuit connu ou le geofencing a fini par activer un circuit,
//     on abandonne la capture silencieusement plutot que de creer une
//     entree parasite.
static const double NEW_CIRCUIT_LINE_OFFSET_M = 4.0; // ~8m de large au total

// (newCircuitCaptureArmed, pendingNewCircuitCapture, etc. declares plus
// haut dans le fichier, avant activateManualCourse/activateAutoMode)

// Declenchee depuis le menu circuit -- "Nouveau circuit (capture)". Ignore
// (avec trace Serial) si un circuit est deja force/detecte, pour ne
// jamais ecraser une vraie detection par erreur.
// Geste intentionnel (menu) -- force la sortie de tout mode actif
// (manuel ou geofencing) et suspend le geofencing tant que la capture
// est en cours, sinon il reactiverait le circuit connu des le prochain
// fix GPS si on est a moins de 15km (cas constate au banc, pres de
// PigTeam_track). Le geofencing est reactive normalement par
// activateAutoMode()/activateManualCourse() (choisir Auto ou un circuit
// depuis le menu), pas de reglage a faire ailleurs.
static void armNewCircuitCapture() {
  manualOverrideActive = false;
  manualCourseIndex = -1;
  courseManager->reset();
  geofenceCheckDone = true; // suspend le geofencing -- cf. commentaire ci-dessus
  lastLapCount = 0;
  newCircuitCaptureArmed = true;
  newCircuitAutoSaved = false;
  pendingNewCircuitCapture = false;
  prevWaypointLat = 0; prevWaypointLng = 0;
  Serial.println("Capture de nouveau circuit armee -- roule un tour complet pour l'enregistrer.");
}

// BACK annule une capture en attente si presse par erreur (cf. loop()) --
// ne fait rien si rien n'est arme, et ne fait rien non plus si une
// capture a deja ete ECRITE dans circuits.csv (a ce stade annuler n'a
// plus de sens, le fichier existe deja -- supprimable depuis la page
// WiFi si besoin, une fois portee).
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
  // 18 champs, meme format que le reste du fichier -- active=0, pas de
  // secteurs, locked=0 (facilement supprimable si le resultat ne convient pas).
  f.printf("0,%s,%.1f,%.7f,%.7f,%.7f,%.7f,0,0,0,0,0,0,0,0,0,0,0\n",
           name, lengthFt, pendingSaLat, pendingSaLng, pendingSbLat, pendingSbLng);
  f.close();
  Serial.printf("Nouveau circuit capture et sauvegarde (inactif) : %s -- a activer plus tard (edition manuelle de circuits.csv ou future page web dediee).\n", name);
}

static void checkAutoCircuitCapture(unsigned long timeMs) {
  if (!newCircuitCaptureArmed || manualOverrideActive || newCircuitAutoSaved) return;

  WaypointLapTimer* t = courseManager->getLapAnythingTimer();
  double wpLat = t->getWaypointLat();
  double wpLng = t->getWaypointLng();

  // Transition (0,0) -> position reelle = le waypoint vient d'etre pose --
  // capture le cap maintenant, tant qu'on est physiquement dessus.
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

  // Premier tour valide, et toujours en mode proximite a cet instant --
  // on committe dans le fichier.
  if (pendingNewCircuitCapture && t->getRaceStarted() && lapAnythingEffective()) {
    char compactDateTime[16];
    getLocalDateTimeCompact(compactDateTime, sizeof(compactDateTime));
    char name[32];
    snprintf(name, sizeof(name), "Nouveau_%s", compactDateTime);

    float lengthFt = t->getLastLapDistance() * 3.28084f; // getLastLapDistance() en metres
    appendAutoCircuitToFile(name, lengthFt);

    newCircuitAutoSaved = true;
    pendingNewCircuitCapture = false;
  }
}

// Traite un fix GPS -- geofencing au premier fix, puis soit le timer
// manuel (circuit reconnu par position), soit CourseManager (detection
// normale par tour+longueur / mode proximite).
static void processGpsFix(double lat, double lng, float altM, float speedKnots, unsigned long timeMs) {
  if (liveData.fixStatus < 2) return;

  pushGpsHistory(lat, lng, timeMs); // alimente l'historique court terme, utilise par checkAutoCircuitCapture() ci-dessus

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

// Etat d'affichage courant (tour en cours/meilleur tour/nombre de tours),
// quel que soit le mode actif (manuel/geofence, circuit auto-detecte, ou proximite).
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

// mm:ss.mmm -- format standard chrono moto, utilise partout a l'affichage.
static void formatLapTime(unsigned long ms, char* buf, size_t bufSize) {
  if (ms == 0) { snprintf(buf, bufSize, "--:--.---"); return; }
  unsigned long minutes = ms / 60000;
  unsigned long seconds = (ms / 1000) % 60;
  unsigned long millisPart = ms % 1000;
  snprintf(buf, bufSize, "%lu:%02lu.%03lu", minutes, seconds, millisPart);
}

// ===================== Lecture du carnet de session (menu Session) =====================
//
// Repris de la logique OLED : regroupement des lignes de /sessions.csv
// par blocs delimites par les marqueurs "# session demarree/arretee"
// (ecrits par startRecording()/stopRecording(), cf. plus bas). Le
// "meilleur temps" affiche est RECALCULE ici comme le vrai minimum sur
// tous les tours de la session -- pas simplement la derniere colonne
// best_lap_time du CSV, qui n'est que le meilleur *connu au moment de ce
// tour precis* (une best-so-far qui progresse tour apres tour) : donner
// cette valeur telle quelle donnerait l'impression que le Best "suit" le
// tour affiche en navigant (bug identifie et corrige cote OLED).

// Inverse de formatLapTime() -- "M:SS.mmm" -> millisecondes. Renvoie
// ULONG_MAX si le format ne correspond pas (ex. "--:--.---" = pas de temps).
static unsigned long parseLapTimeStr(const String& s) {
  int colon = s.indexOf(':');
  int dot = s.indexOf('.');
  if (colon < 0 || dot < 0) return ULONG_MAX;
  long minutes = s.substring(0, colon).toInt();
  long seconds = s.substring(colon + 1, dot).toInt();
  long millisPart = s.substring(dot + 1).toInt();
  if (minutes == 0 && seconds == 0 && millisPart == 0 && s.charAt(0) != '0') return ULONG_MAX; // "--:--.---" -> toInt() donne 0 partout
  return (unsigned long)(minutes * 60000 + seconds * 1000 + millisPart);
}

struct SessionSummaryTft {
  String compactKey; // "AAAAMMJJ_HHMMSS", tel qu'ecrit par startRecording()
  int lapCount;
  unsigned long bestLapMs; // ULONG_MAX si aucun tour exploitable
};

struct LapDetailTft {
  int lapNumber;
  unsigned long lapMs;
  String circuit;
};

// Parcourt /sessions.csv une seule fois, construit un resume par session
// (nb de tours, meilleur temps recalcule). Ne charge pas le detail des
// tours -- cf. loadLapsForSession() plus bas pour ca, appelee seulement
// une fois une session choisie dans la liste.
static std::vector<SessionSummaryTft> loadSessionSummaries() {
  std::vector<SessionSummaryTft> result;
  File f = LittleFS.open(SESSION_LOG_PATH, "r");
  if (!f) return result;

  bool inSession = false;
  SessionSummaryTft cur;
  static const char* MARK_START = "# session demarree ";
  static const char* MARK_STOP = "# session arretee";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith(MARK_START)) {
      if (inSession) result.push_back(cur); // securite si "arretee" manquant (carte eteinte pendant REC)
      cur = SessionSummaryTft();
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
    if (!inSession) continue; // ligne orpheline (avant le premier marqueur) -- ignoree

    String fld[6];
    if (splitCsvLine(line, fld, 6) < 6) continue; // ligne malformee -- ignoree
    cur.lapCount++;
    unsigned long lapMs = parseLapTimeStr(fld[3]);
    if (lapMs != ULONG_MAX && lapMs < cur.bestLapMs) cur.bestLapMs = lapMs;
  }
  if (inSession) result.push_back(cur); // session encore ouverte a la lecture (jamais arretee)
  f.close();
  return result;
}

// Detail tour par tour d'UNE session (identifiee par son compactKey) --
// relit le fichier en entier, ne garde que les lignes du bloc concerne.
static std::vector<LapDetailTft> loadLapsForSession(const String& compactKey) {
  std::vector<LapDetailTft> result;
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
      if (inTarget) break; // fini de lire ce qui nous interesse
      continue;
    }
    if (!inTarget) continue;

    String fld[6];
    if (splitCsvLine(line, fld, 6) < 6) continue;
    LapDetailTft lap;
    lap.lapNumber = fld[2].toInt();
    lap.lapMs = parseLapTimeStr(fld[3]);
    lap.circuit = fld[5];
    result.push_back(lap);
  }
  f.close();
  return result;
}

// "AAAAMMJJ_HHMMSS" -> "JJ/MM HH:MM", plus lisible/compact pour la liste
// des sessions a l'ecran.
static String formatCompactKeyShort(const String& key) {
  if (key.length() < 15) return key; // format inattendu -- affiche tel quel plutot que planter
  return key.substring(6, 8) + "/" + key.substring(4, 6) + " " + key.substring(9, 11) + ":" + key.substring(11, 13);
}

// ===================== Enregistrement (logs CSV) =====================
//
// Deux fichiers, comme sur la variante OLED :
//  - /log_AAAAMMJJ_HHMMSS.csv : detail GPS complet de la session (1 ligne
//    par fix), pour /compare et /lap plus tard (WebServerManager, pas
//    encore porte -- pour l'instant juste stocke sur la flash).
//  - /sessions.csv : carnet cumulatif leger, 1 ligne par tour termine,
//    delimite par des marqueurs "# session demarree/arretee".
// PUSH declenche start/stopRecording() (cf. loop()).

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
  if (!liveData.year) { // pas encore de fix GPS -- pas d'horodatage fiable pour nommer le fichier
    Serial.println("REC refuse : pas encore de fix GPS (horodatage necessaire pour nommer le log).");
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

  char dateBuf[12], timeBuf[10];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
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

static void toggleRecording() {
  if (recordingEnabled) stopRecording(); else startRecording();
}

// Ecrit une ligne de detail GPS -- appelee a chaque fix GPS traite tant
// que l'enregistrement est actif (cf. loop()).
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
                 liveData.fixStatus, liveData.numSVs, lapsCount,
                 getActiveCourseNameForDisplay(), currentLapMs);

  static unsigned long lastFlush = 0;
  if (millis() - lastFlush >= 5000) {
    lastFlush = millis();
    logFile.flush();
  }
}

// Appelee a chaque fix GPS traite -- detecte un tour fraichement termine
// (lapsCount qui augmente) et l'ajoute au carnet /sessions.csv. A
// appeler APRES processGpsFix() (le nouveau tour doit deja etre compte
// cote CourseManager).
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

// Parcourt les fichiers /log_*.csv sur gpsLogFs (SD si disponible, sinon LittleFS).
template<typename Fn>
static void forEachGpsLogFile(Fn callback) {
  File root = gpsLogFs->open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.indexOf("/log_") >= 0 && name.endsWith(".csv")) callback(name, (uint32_t)f.size());
    f = root.openNextFile();
  }
}

// ===================== Batterie (pont diviseur sur GPIO1) =====================
//
// Diviseur 10k/10k entre le pin BAT du Lipo Rider Plus et GND -- ramene
// 3.0-4.2V (plage Li-Po) vers 1.5-2.1V sur l'ADC, large marge sous les
// 3.3V. 10k/10k retenu plutot que 100k/100k : impedance de source trop
// elevee en 100k pour le convertisseur ADC de l'ESP32-S3 (~3.4% d'erreur
// constatee au banc) -- 10k/10k ramene l'erreur a ~0.05%, jugee
// suffisante pour un indicateur (pas de facteur de calibration
// logiciel ajoute). Facteur 2.0f ci-dessous = ratio du diviseur.
static float readBatteryVoltage() {
  const int samples = 16; // moyenne pour lisser le bruit ADC
  uint32_t sumMv = 0;
  for (int i = 0; i < samples; i++) {
    sumMv += analogReadMilliVolts(BATTERY_ADC_PIN); // lecture calibree (mV), gere l'attenuation en interne
  }
  float adcMv = (float)sumMv / samples;
  return (adcMv * 2.0f) / 1000.0f; // x2 = ratio diviseur, /1000 = mV -> V
}

// Estimation grossiere du pourcentage -- courbe Li-Ion approximative, non
// lineaire (volontairement pessimiste sous 3.5V ou la tension chute vite
// en fin de decharge). A affiner au banc si besoin de precision -- pas
// destine a remplacer les 4 LEDs fuel gauge du module, juste a avoir une
// valeur numerique exploitable sur l'ecran.
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

// Commandes Serial -- en complement du WebServerManager (telechargement
// WiFi a la demande, cf. WebServerManager.h) : 'l' liste les logs GPS
// detailles, 'd' dump le dernier en Serial (copier-coller vers un .csv),
// 's' dump le carnet de session, 'c' efface tous les logs GPS detailles
// (garde le carnet de session intact), 'b' affiche tension/pourcentage
// batterie (deja repris dans drawStatusScreen(), pratique pour verifier
// rapidement sans allumer l'ecran).
static void handleSerialCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c == 'b') {
    float v = readBatteryVoltage();
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
  }
}

// ===================== WebServerManager (module partage avec la variante OLED) =====================
//
// Module complet repris tel quel de la variante OLED (WebServerManager.h/
// .cpp, pas de copie/adaptation) -- ainsi toute evolution future cote
// OLED (nouvelle page, correctif...) se reporte directement ici en
// remplacant simplement ces deux fichiers, sans toucher a main.cpp.
// Le module ne connait rien du GPS/CourseManager -- il recoit des
// callbacks (flushLogs, getStatus) fournis juste en dessous, sur le
// meme principe de decouplage que cote OLED. bleStop/bleRestart passes
// a nullptr (pas de BLE ici, jamais utilise -- signature conservee pour
// rester strictement identique au module source).
static void flushLogsCallback() {
  if (loggingOk) logFile.flush();
}

static WebServerStatusInfo getStatusCallback() {
  WebServerStatusInfo s;
  s.bleConnected = gpsActive; // champ reutilise tel quel -- represente "GPS actif" ici (meme convention que cote OLED, qui l'a deja detourne de son nom d'origine)
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

  // Barre de stockage SD sur la page Sessions -- seulement si gpsLogFs
  // pointe reellement sur la SD (pas en repli LittleFS, deja couverte
  // par sa propre barre cote WebServerManager). sdUsedBytes()/
  // sdTotalBytes() (SdLogStorage.h) encapsulent la lecture SDFS-specifique
  // (pas dans l'interface commune fs::FS) et renvoient 0 si la SD n'est
  // pas active -- pas besoin de dupliquer ici la verification gpsLogsOnSd.
  if (gpsLogsOnSd) {
    s.hasSeparateLogsFs = true;
    s.logsFsLabel = "SD (logs GPS detailles)";
    s.logsFsUsedBytes = sdUsedBytes();
    s.logsFsTotalBytes = sdTotalBytes();
  }

  return s;
}

// migrateLittleFsLogsToSd() vit desormais dans SdLogStorage.cpp (module
// partage avec la variante OLED) -- cf. SdLogStorage.h.

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== Chrono GPS moto piste (TFT ST7789 + EC11 + 1 bouton) -- demarrage ===");

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  Serial.println("[1/8] TZ ok");

  analogReadResolution(12);
  // Pas d'analogSetPinAttenuation() ici -- ADC_11db (pleine echelle
  // 0-3.3V) est deja l'attenuation par defaut du core Arduino-ESP32 pour
  // tous les pins ADC. L'appel explicite generait un message
  // "__analogChannelConfig(): Pin is not configured as analog channel"
  // au boot -- bug cosmetique connu de cette fonction sur les versions
  // recentes du core (post 5.1.4/v3.0.1+, cf. discussions SimpleFOC),
  // sans consequence sur la lecture elle-meme, mais autant l'eviter en
  // ne l'appelant pas puisqu'elle ne changeait rien.
  Serial.println("[1c/8] ADC batterie ok");

  tftSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // -1 = pas de MISO -- SD a desormais son propre bus dedie (sdSPI), l'ecran n'a jamais eu besoin de lire
  Serial.println("[1b/8] bus SPI dedie (ecran) ok");
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  Serial.println("[1d/8] bus SPI dedie (SD) ok");
  tft.init(240, 320); // resolution logique -- cf. tft.setRotation() juste apres pour le paysage 320x240
  Serial.println("[2/8] tft.init() ok");
  // Ce panneau ST7789 demarre en mode inverse (INVON) par defaut chez
  // Adafruit_ST7789 (courant sur les dalles IPS) -- identifie via un
  // test de bandes de couleurs pures (blanc->noir, rouge->cyan,
  // vert->magenta, bleu->jaune = signature exacte d'une inversion bit a
  // bit complete). invertDisplay(true) n'avait aucun effet (deja actif
  // par defaut) -- il faut bien false pour le desactiver.
  tft.invertDisplay(false);
  tft.setRotation(3); // paysage, 320x240 -- demi-tour par rapport a la rotation 1 (confirme sur le banc)
  Serial.println("[3/8] tft.setRotation() ok");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH); // retroeclairage -- Adafruit_ST7789 ne le gere pas lui-meme, contrairement a TFT_eSPI
  drawBootSplash();
  Serial.println("[4/8] drawBootSplash() ok");

  pinMode(BACK_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BACK_BUTTON), backButtonISR, FALLING);
  Serial.println("[5/8] BACK_BUTTON ok");

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  // Bornes provisoires pour ce squelette -- seront reglees par ecran une
  // fois les menus portes (cf. variante OLED, setBoundaries() par sous-menu).
  rotaryEncoder.setBoundaries(0, 100, false);
  rotaryEncoder.setAcceleration(0);
  Serial.println("[6/8] rotaryEncoder ok");

#if GPS_ENABLED
  initGps();
  Serial.println("[7/8] GPS ok");
#else
  Serial.println("[7/8] GPS DESACTIVE (GPS_ENABLED=0) -- test de brochage TFT/encodeur/BACK sur Tiny N8R8");
#endif

  if (!LittleFS.begin(true)) { // true = formate automatiquement si le filesystem est illisible/absent
    Serial.println("[8/8] LittleFS: echec de montage -- circuits desactives, mode proximite uniquement.");
  } else {
    loadActiveCircuitsIntoTracks();
    Serial.println("[8/8] LittleFS + circuits ok");
  }

  // ----- SD, etape 2 : branchee sur le stockage des logs GPS -----
  // initSdLogStorage() (SdLogStorage.h, module partage avec la variante
  // OLED) fait SD.begin(SD_CS, sdSPI) et bascule gpsLogFs sur &SD en cas
  // de succes -- repli automatique sur LittleFS sinon (carte absente,
  // mal cablee, en panne -- la SD reste facultative, comme prevu des
  // l'origine dans WebServerManager.h). sdSPI doit deja avoir ete
  // initialisee (cf. sdSPI.begin() plus haut) -- SdLogStorage ne touche
  // jamais au bus lui-meme. Bus desormais DEDIE (pas partage avec
  // l'ecran, contrairement au plan S3-Zero -- la maquette Tiny a assez
  // de GPIO libres pour se le permettre). Le vrai test (ecriture reelle
  // d'un log_*.csv) se fera au premier REC -- inutile de dupliquer ici le
  // test lecture/ecriture deja valide a l'etape 1 (bas niveau, meme
  // cablage).
  Serial.println("[8b] test SD...");
  if (!initSdLogStorage(SD_CS, sdSPI)) {
    Serial.println("[8b] SD : absente ou en panne -- repli sur LittleFS pour les logs GPS detailles.");
  } else {
    uint64_t sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[8b] SD ok -- carte detectee (~%llu Mo) -- logs GPS detailles bascules dessus.\n", sizeMb);
    migrateLittleFsLogsToSd(); // sessions restees sur LittleFS avant ce demarrage (enregistrees ou importees pre-SD) -- deplacees ici pour redevenir visibles cote WebServerManager et liberer la place qu'elles occupaient
  }

  courseManager = new CourseManager(myTracks, 7.0, &Serial); // &Serial = logs de detection verbeux -- utile pour les premiers essais, passer nullptr pour une version silencieuse plus tard

  webServerManager.begin("ChronoMotoTFT", SESSION_LOG_PATH, CIRCUITS_FILE_PATH, *gpsLogFs, nullptr, nullptr, flushLogsCallback, getStatusCallback);

  Serial.println("Pret. Ecran statut GPS+tours actif -- REC via PUSH ecrit /log_*.csv + /sessions.csv, BACK ouvre le menu principal.");
  Serial.println("Commandes Serial : 'l' liste les logs, 'd' dump le dernier, 's' dump le carnet de session, 'c' efface les logs GPS, 'b' tension/pourcentage batterie.");
}

// ===================== Etat d'ecran (statut / menu principal / sous-menus) =====================
enum ScreenState {
  SCREEN_STATUS,
  SCREEN_MAIN_MENU,
  SCREEN_CIRCUIT_MENU,
  SCREEN_CONNEXION,
  SCREEN_SESSION_LIST,
  SCREEN_SESSION_LAPS,
  SCREEN_SETTINGS,
  SCREEN_WIFI,
  SCREEN_CONFIRM_STOP // pause avant arret definitif -- cf. discussion "faux contact en voiture"
};
static ScreenState screenState = SCREEN_STATUS;
// Duree max sur SCREEN_CONFIRM_STOP avant arret definitif automatique --
// filet de securite si on oublie de valider (BACK) ou de reprendre (PUSH)
// en quittant la piste. Sans ca, l'ecran resterait arme indefiniment et un
// faux contact (vibrations, etc.) pourrait relancer l'enregistrement.
static const unsigned long CONFIRM_STOP_TIMEOUT_MS = 300000UL; // 5 minutes
static unsigned long confirmStopEnteredMs = 0;
// Faux contact electrique = evenement quasi instantane. Un vrai choix
// humain de REPRENDRE prend toujours au moins quelques centaines de ms de
// reaction -- on ignore donc PUSH (reprendre) pendant cette fenetre courte
// apres l'ouverture de l'ecran. BACK (arret definitif) n'est PAS concerne.
static const unsigned long CONFIRM_STOP_INPUT_GRACE_MS = 600UL;
// Idem, mais cote ecran Statut : une fois l'arret DEFINITIF confirme, le
// geofencing (rayon 15km) peut redetecter le circuit et rearmer le
// declencheur REC en ~1s si on est pres de la piste (constate au banc sur
// l'AMOLED le 27/07, meme code de geofencing ici). Protege le PUSH de
// l'ecran Statut juste apres un arret definitif.
static unsigned long lastDefinitiveStopMs = 0;
static const unsigned long STATUS_REC_GRACE_AFTER_STOP_MS = 1500UL;
static int menuSelection = 0;      // reutilise pour le sous-menu Circuit (cf. plus bas)
static int mainMenuSelection = 0;  // menu principal -- 0=Circuit, 1=Connexion, 2=Session, 3=Reglages, 4=New track (capture)
static int sessionListSelection = 0;
static int sessionLapSelection = 0;
static std::vector<SessionSummaryTft> sessionListCache;
static std::vector<LapDetailTft> sessionLapsCache;
// Force un fillScreen() complet au prochain rendu -- utilise a chaque
// changement d'ecran (statut <-> menu), pour ne pas melanger l'ancien
// contenu avec le nouveau (les deux ecrans n'effacent sinon que leurs
// propres lignes, cf. clearLine()).
static bool screenNeedsFullRedraw = true;
// Distinct de screenNeedsFullRedraw (qui ne declenche qu'un fillScreen
// complet aux transitions d'ecran) -- uiDirty controle si un ecran de
// type "menu" (statique entre deux actions) doit meme etre redessine ce
// tick. Sans ca, ces ecrans etaient redessines integralement 4x/seconde
// (meme rythme que l'ecran statut, en temps reel lui) sans raison,
// causant un clignotement constate au banc alors que rien ne change
// entre deux frames identiques. Mis a true a chaque transition d'ecran
// et a chaque changement de selection (cf. les blocs encoderChanged()
// plus bas dans loop()) -- les ecrans "en direct" (statut, connexion)
// ne s'en servent pas, ils se redessinent en continu comme avant.
static bool uiDirty = true;

// Ecran statut -- fix/satellites/vitesse/position, batterie, nom du
// circuit/etat de detection, tour en cours/dernier/meilleur, REC.
// L'encodeur reste affiche en petit en bas (debug), la valeur brute
// n'est plus utilisee ailleurs que la navigation menu.
//
// Efface uniquement chaque ligne (fillRect cible) avant de la reecrire,
// plutot qu'un fillScreen() complet a chaque rafraichissement -- evite le
// scintillement constate au banc. Le fond entier n'est efface qu'une
// seule fois, au tout premier appel.
static const int16_t STATUS_LINE_H_SMALL  = 18; // textSize 2, police 6x8 -> ~16px + marge
static const int16_t STATUS_LINE_H_BIG    = 26; // textSize 3
static const int16_t STATUS_LINE_H_TINY   = 12; // textSize 1

static void clearLine(int16_t y, int16_t h) {
  canvas.fillRect(0, y, canvas.width(), h, ST77XX_BLACK);
}

// NOTE : on avait tente de regrouper tout ca dans un tft.startWrite()/
// endWrite() pour reduire encore le scintillement -- provoque un ecran
// blanc/fige avec cette version d'Adafruit_ST7789 (a eviter tel quel).
// On reste donc sur des appels fillRect()/print() individuels : un peu
// de scintillement residuel, mais fiable.
static const int16_t STATUS_LINE_H_XL = 34; // textSize 4

// Menu circuit -- ouvert par BACK depuis l'ecran statut. Option 0 =
// "Auto (detection)", options 1..courseCount = circuits actifs de
// circuits.csv, dans l'ordre charge par loadActiveCircuitsIntoTracks().
static void drawCircuitMenu() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.print("Choisir circuit");

  const int16_t itemY0 = 40, itemH = 24;
  int itemCount = myTracks.courseCount + 1; // Auto + circuits uniquement
  for (int i = 0; i < itemCount; i++) {
    int16_t y = itemY0 + i * itemH;
    clearLine(y, itemH);
    canvas.setCursor(4, y);
    bool selected = (i == menuSelection);
    canvas.setTextColor(selected ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK);
    canvas.print(selected ? "> " : "  ");
    if (i == 0) canvas.print("Auto (detection)");
    else canvas.print(myTracks.courses[i - 1].name);
  }
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setTextSize(1);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("PUSH: valider   BACK: retour au menu");
}

// ===================== Menu principal (BACK depuis l'ecran statut) =====================
// 5 entrees, meme structure que la variante OLED sans le mode Demo
// (ecarte deliberement ici, pas juste pas encore porte -- cf. README) :
// Circuit (sous-menu deja existant), Connexion (etat GPS + circuit
// actif), Session (carnet enregistre), Reglages (raccourci WiFi),
// New track (capture de nouveau circuit).
static const char* MAIN_MENU_ITEMS[5] = { "Circuit", "Connexion", "Session", "Reglages", "New track (capture)" };

static void drawMainMenuScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.print("Menu");

  const int16_t itemY0 = 40, itemH = 26;
  for (int i = 0; i < 5; i++) {
    int16_t y = itemY0 + i * itemH;
    clearLine(y, itemH);
    canvas.setCursor(4, y);
    bool selected = (i == mainMenuSelection);
    canvas.setTextColor(selected ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK);
    canvas.print(selected ? "> " : "  ");
    canvas.print(MAIN_MENU_ITEMS[i]);
  }
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setTextSize(1);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("PUSH: valider   BACK: retour au statut");
}

// ===================== Connexion (etat GPS + circuit actif) =====================
// Equivalent de l'ancien "ecran info" cote OLED. Rafraichi en continu
// (comme l'ecran statut) tant qu'affiche -- utile pour surveiller le fix
// sans avoir le chrono en cours qui defile.
static void drawConnexionScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.print("Connexion");

  clearLine(44, STATUS_LINE_H_SMALL);
  canvas.setTextSize(1);
  canvas.setCursor(4, 44);
  if (gpsActive) canvas.printf("GPS OK  Fix:%u  Sat:%u", liveData.fixStatus, liveData.numSVs);
  else canvas.print("GPS -- (recherche)");

  clearLine(70, STATUS_LINE_H_SMALL);
  canvas.setTextSize(2);
  canvas.setCursor(4, 70);
  canvas.print(getActiveCourseNameForDisplay());

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setTextSize(1);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("BACK : retour au menu");
}

// ===================== Session -- liste puis detail des tours =====================
//
// Recharge le resume (loadSessionSummaries()) a chaque entree dans la
// liste -- pas mis en cache entre deux visites, le fichier peut avoir
// grossi entre-temps (nouvelle session enregistree). Plus recentes en
// premier (README OLED). Limite d'affichage a 8 sessions pour l'instant
// (pas de scroll ni de pagination) -- les plus anciennes ne sont
// simplement pas listees au-dela.
static void enterSessionList() {
  sessionListCache = loadSessionSummaries();
  std::reverse(sessionListCache.begin(), sessionListCache.end());
  if (sessionListCache.size() > 8) sessionListCache.resize(8);
  sessionListSelection = 0;
}

static void drawSessionListScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.print("Sessions");

  const int16_t itemY0 = 36, itemH = 22;
  if (sessionListCache.empty()) {
    clearLine(itemY0, itemH);
    canvas.setTextSize(1);
    canvas.setCursor(4, itemY0);
    canvas.print("Aucune session enregistree.");
  }
  for (size_t i = 0; i < sessionListCache.size(); i++) {
    int16_t y = itemY0 + (int16_t)i * itemH;
    clearLine(y, itemH);
    canvas.setTextSize(1);
    canvas.setCursor(4, y);
    bool selected = ((int)i == sessionListSelection);
    canvas.setTextColor(selected ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK);
    canvas.print(selected ? "> " : "  ");
    char bestBuf[16];
    formatLapTime(sessionListCache[i].bestLapMs == ULONG_MAX ? 0 : sessionListCache[i].bestLapMs, bestBuf, sizeof(bestBuf));
    canvas.printf("%s  %d tours  %s", formatCompactKeyShort(sessionListCache[i].compactKey).c_str(), sessionListCache[i].lapCount, bestBuf);
  }
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("PUSH: voir les tours   BACK: retour");
}

// Dernier tour affiche en premier (comme cote OLED) -- plus utile a la
// sortie de piste que le premier tour, generalement un tour de lancement.
static void enterSessionLaps() {
  if (sessionListCache.empty()) { sessionLapsCache.clear(); sessionLapSelection = 0; return; }
  sessionLapsCache = loadLapsForSession(sessionListCache[sessionListSelection].compactKey);
  sessionLapSelection = sessionLapsCache.empty() ? 0 : (int)sessionLapsCache.size() - 1;
}

static void drawSessionLapsScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.print("Tours");

  if (sessionLapsCache.empty()) {
    clearLine(40, STATUS_LINE_H_SMALL);
    canvas.setTextSize(1);
    canvas.setCursor(4, 40);
    canvas.print("Aucun tour dans cette session.");
  } else {
    // Vrai meilleur temps recalcule sur tous les tours -- pas une colonne
    // du CSV (cf. commentaire en tete de section "Lecture du carnet de session").
    unsigned long trueBest = ULONG_MAX;
    for (const LapDetailTft& lap : sessionLapsCache) {
      if (lap.lapMs != ULONG_MAX && lap.lapMs < trueBest) trueBest = lap.lapMs;
    }

    const LapDetailTft& lap = sessionLapsCache[sessionLapSelection];
    char lapBuf[16], bestBuf[16];
    formatLapTime(lap.lapMs, lapBuf, sizeof(lapBuf));
    formatLapTime(trueBest == ULONG_MAX ? 0 : trueBest, bestBuf, sizeof(bestBuf));

    clearLine(40, STATUS_LINE_H_TINY);
    canvas.setTextSize(1);
    canvas.setCursor(4, 40);
    canvas.printf("Tour %d / %d", sessionLapSelection + 1, (int)sessionLapsCache.size());

    clearLine(56, STATUS_LINE_H_XL);
    canvas.setTextSize(4);
    canvas.setCursor(4, 56);
    canvas.print(lapBuf);

    clearLine(96, STATUS_LINE_H_SMALL);
    canvas.setTextSize(2);
    canvas.setCursor(4, 96);
    canvas.printf("Meilleur: %s", bestBuf);

    clearLine(120, STATUS_LINE_H_SMALL);
    canvas.setCursor(4, 120);
    canvas.print(lap.circuit);
  }

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setTextSize(1);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("Tourne: change de tour   BACK: retour");
}

// ===================== Reglages (raccourci WiFi) =====================
// Une seule entree pour l'instant, meme limitation que cote OLED.
static void drawSettingsScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.print("Reglages");

  canvas.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  canvas.setCursor(4, 40);
  canvas.print("> WiFi telechargement");
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setTextSize(1);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("PUSH: ouvrir   BACK: retour au menu");
}

// Ecran WiFi -- affiche une seule fois (contenu statique, pas besoin de
// redessiner a chaque tick comme l'ecran statut). BACK coupe le WiFi et
// revient a l'ecran statut (cf. loop()).
static void drawWifiScreen() {
  if (!screenNeedsFullRedraw) return; // deja affiche, rien a refaire
  canvas.fillScreen(ST77XX_BLACK);
  screenNeedsFullRedraw = false;
  canvas.setFont(NULL); // police par defaut -- pas celle du statut (canvas est un objet unique reutilise a chaque frame)

  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setTextSize(2);
  canvas.setCursor(4, 10);
  canvas.print("WiFi actif");

  canvas.setTextSize(1);
  canvas.setCursor(4, 50);
  canvas.printf("SSID: %s", webServerManager.getSsid().c_str());
  canvas.setCursor(4, 66);
  canvas.printf("IP:   %s", webServerManager.getIp().c_str());
  canvas.setCursor(4, 90);
  canvas.print("Connecte-toi au SSID ci-dessus,");
  canvas.setCursor(4, 102);
  canvas.print("puis ouvre l'IP dans un navigateur.");

  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("BACK : couper le WiFi et revenir");
}

// Offsets baseline<->haut-de-glyphe pour nos polices custom (mesures a la
// conversion sur un caractere representatif -- cf. glyphe 'yOffset' dans
// les .h). Avec un GFXfont, setCursor(x, y) positionne la BASELINE du
// texte, pas le coin haut-gauche comme avec la police par defaut -- ces
// constantes permettent de continuer a raisonner "position du HAUT du
// texte souhaitee" comme avant (baseline = haut_voulu + offset). Portees
// depuis le sous-projet display_only (banc de test affichage) -- cf. son
// README pour le detail de calibration de chaque taille.
// Valeurs = pire cas (yOffset le plus negatif) parmi les caracteres
// REELLEMENT affiches avec chaque police (mesure directe dans les .h,
// pas une approximation sur un seul caractere "representatif" -- source
// du rognage constate au banc : '/' dans "km/h" ou des lettres a
// jambage (circuit libre) montent nettement plus haut qu'un simple 'G'
// ou '0'), + 1px de marge de securite.
static const int16_t BASELINE_OFFSET_REGULAR11 = 15; // pire cas reel : '/' (14) dans "km/h"
static const int16_t BASELINE_OFFSET_MEDIUM15  = 20; // pire cas reel : lettres minuscules a jambage (19) -- circuit = texte libre
static const int16_t BASELINE_OFFSET_MEDIUM20  = 26; // pire cas reel : 'i' (25) dans "Dernier"/"Best"
static const int16_t BASELINE_OFFSET_BOLD40    = 55; // pire cas reel : '/' (54) dans "km/h"
static const int16_t BASELINE_OFFSET_BOLD21    = 26; // pire cas reel : 'C' (25) dans "PRESS REC"

// Ecran Statut -- 3 modes bien distincts, valides sur le banc display_only
// avant ce portage (cf. son README, section "Ce qui est simule" pour le
// detail de la demarche) :
//  1. Recherche (circuit pas encore detecte) -- rien sous le nom du
//     circuit ("Detection...") hormis la vitesse (grosse, centree) et
//     l'heure (centree) -- pertinent en paddock/pit lane (circulation
//     limitee).
//  2. Circuit detecte (detectionEffectivelyComplete()) -- idem, plus un
//     "PRESS REC" clignotant en bas d'ecran.
//  3. Enregistrement (recordingEnabled) -- REC (rouge) accole au %
//     batterie en haut a droite ; nom du circuit masque (gagne de la
//     place, le circuit actif est deja implicite) ; "Tours: N" (tour EN
//     COURS, pas le nb de tours completes) en haut ; chrono principal
//     remonte ; Dernier/Best agrandis (Medium20) avec un bon espacement.
//     Tant que la ligne de depart/arrivee n'a pas ete franchie
//     (currentLapMs == 0, cf. getDisplayState()/getRaceStarted()), le
//     chrono principal affiche la vitesse a la place de "--:--.---" --
//     plus utile pendant l'approche de la ligne.
static void drawStatusScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  char line[48];
  bool circuitDetected = detectionEffectivelyComplete();

  // ----- Zone du haut (GPS + REC/batterie + circuit ou "Tours: N") -----
  // Un seul clear pour toute la bande 0-48 -- des sous-zones separees
  // laissaient a chaque fois un interstice de 1-2px jamais efface entre
  // elles (source de petits artefacts residuels constates au banc,
  // heritage du texte du mode precedent) ; un seul rect contigu avec
  // celui plus bas (y=48) couvre tout l'ecran sans aucun trou possible.
  canvas.fillRect(0, 0, canvas.width(), 48, ST77XX_BLACK);

  // ----- Ligne GPS -----
  canvas.setFont(&Teko_Regular11pt7b);
  canvas.setCursor(4, 0 + BASELINE_OFFSET_REGULAR11);
  if (gpsActive) canvas.printf("GPS OK  Fix:%d Sat:%d", liveData.fixStatus, liveData.numSVs);
  else canvas.print("GPS -- (recherche)");

  // ----- REC + batterie (meme ligne, coin haut droit, REC avant le %) -----
  float battV = readBatteryVoltage();
  int battPct = batteryVoltageToPercent(battV);
  uint16_t battColor = (battPct > 50) ? ST77XX_GREEN : (battPct > 20 ? ST77XX_YELLOW : ST77XX_RED);
  canvas.setFont(&Teko_Regular11pt7b);
  int16_t x1, y1; uint16_t tw, th;
  int16_t battX = 316; // ancre a droite -- calcule la position du % en fonction de sa propre largeur (police proportionnelle)
  snprintf(line, sizeof(line), "%3d%%", battPct);
  canvas.getTextBounds(line, 0, 0, &x1, &y1, &tw, &th);
  battX -= tw;
  if (recordingEnabled) {
    canvas.setTextColor(ST77XX_RED, ST77XX_BLACK);
    canvas.getTextBounds("REC ", 0, 0, &x1, &y1, &tw, &th);
    canvas.setCursor(battX - tw, 0 + BASELINE_OFFSET_REGULAR11);
    canvas.print("REC ");
  }
  canvas.setTextColor(battColor, ST77XX_BLACK);
  canvas.setCursor(battX, 0 + BASELINE_OFFSET_REGULAR11);
  canvas.print(line);
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  // ----- Circuit / capture -----
  // Masque pendant l'enregistrement -- gagne de la place a l'ecran, le
  // circuit actif est deja implicite (on est en train d'y rouler).
  if (!recordingEnabled) {
    canvas.setFont(&Teko_Medium15pt7b);
    canvas.setTextColor(newCircuitCaptureArmed ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK); // capture en cours -- bien visible
    canvas.setCursor(4, 20 + BASELINE_OFFSET_MEDIUM15);
    canvas.print(getActiveCourseNameForDisplay());
    canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  }

  // ===================== Contenu sous la ligne circuit -- differe selon le mode =====================
  canvas.fillRect(0, 48, canvas.width(), canvas.height() - 48, ST77XX_BLACK); // jusqu'en bas d'ecran -- un seul clear, chaque mode redessine ce qu'il lui faut dedans (evite toute remanence, ex. PRESS REC pendant sa phase "eteinte")

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount);
  float speedKmh = liveData.speedMmPerS / 1000.0f * 3.6f;
  char buf[16];

  if (!recordingEnabled) {
    // ----- Recherche / circuit detecte (pas encore d'enregistrement) -----
    // Rien d'utile a afficher hormis la vitesse (paddock/pit lane =
    // circulation limitee, la vitesse reste pertinente) et l'heure -- tout
    // le reste (tour en cours, dernier, meilleur, tours) n'a pas de sens
    // tant que rien n'est enregistre. Pas de label ("Vitesse"/"Heure") --
    // evident au premier coup d'oeil, inutile d'encombrer l'ecran.
    canvas.setFont(&Teko_Bold40pt7b);
    snprintf(line, sizeof(line), "%d km/h", (int)speedKmh);
    canvas.getTextBounds(line, 0, 0, &x1, &y1, &tw, &th);
    canvas.setCursor((canvas.width() - (int16_t)tw) / 2, 76 + BASELINE_OFFSET_BOLD40);
    canvas.print(line);

    canvas.setFont(&Teko_Medium20pt7b);
    char timeBuf[10];
    getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));
    canvas.getTextBounds(timeBuf, 0, 0, &x1, &y1, &tw, &th);
    canvas.setCursor((canvas.width() - (int16_t)tw) / 2, 150 + BASELINE_OFFSET_MEDIUM20);
    canvas.print(timeBuf);

    // PRESS REC -- uniquement une fois le circuit detecte (pas en
    // recherche). Bien visible pour ne pas oublier de lancer
    // l'enregistrement. Clignote 600ms ON / 300ms OFF -- assez lent pour
    // rester lisible, assez rapide pour attirer l'oeil.
    if (circuitDetected) {
      unsigned long blinkPhase = millis() % 900;
      if (blinkPhase < 600) {
        const int16_t pressRecTop = 198;
        canvas.setFont(&Teko_Bold21pt7b);
        canvas.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        canvas.getTextBounds("PRESS REC", 0, 0, &x1, &y1, &tw, &th); // largeur reelle -- police proportionnelle, pas de calcul "N caracteres x largeur fixe" possible
        canvas.setCursor((canvas.width() - (int16_t)tw) / 2, pressRecTop + BASELINE_OFFSET_BOLD21);
        canvas.print("PRESS REC");
        canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      }
    }

  } else {
    // ----- Enregistrement -----
    // La ligne circuit est masquee (cf. plus haut) -- "Tours: N" prend la
    // place de l'ancien label "Tour en cours" (deja implicite au-dessus du
    // chrono) -- gagne la place occupee par la ligne du bas, reinvestie
    // pour agrandir Dernier/Best (plus lisibles, plus d'air entre les
    // deux). Pas de vitesse ici (utile seulement en recherche/detecte, en
    // piste l'info qui compte c'est le chrono).
    canvas.setFont(&Teko_Regular11pt7b);
    int toursDisplay = lapsCount + (currentLapMs > 0 ? 1 : 0); // affiche le tour EN COURS (passe a N+1 des que la ligne est franchie), pas le nb de tours completes
    snprintf(line, sizeof(line), "Tours: %d", toursDisplay);
    canvas.setCursor(4, 20 + BASELINE_OFFSET_REGULAR11);
    canvas.print(line);

    canvas.setFont(&Teko_Bold40pt7b);
    if (currentLapMs > 0) {
      // Ligne de depart/arrivee franchie (getRaceStarted() true cote
      // CourseManager/DovesLapTimer) -- chrono actif.
      formatLapTime(currentLapMs, buf, sizeof(buf));
    } else {
      // REC actif mais course pas encore demarree (avant le 1er
      // franchissement de ligne) -- la vitesse reste plus utile qu'un
      // "--:--.---" fige pendant cette attente.
      snprintf(buf, sizeof(buf), "%d km/h", (int)speedKmh);
    }
    canvas.setCursor(4, 46 + BASELINE_OFFSET_BOLD40);
    canvas.print(buf);

    canvas.setFont(&Teko_Medium20pt7b);
    formatLapTime(getLastFinishedLapMs(), buf, sizeof(buf));
    snprintf(line, sizeof(line), "Dernier:  %s", buf);
    canvas.setCursor(4, 112 + BASELINE_OFFSET_MEDIUM20);
    canvas.print(line);

    formatLapTime(hasBest ? bestLapMs : 0, buf, sizeof(buf));
    snprintf(line, sizeof(line), "Best:  %s", buf);
    canvas.setCursor(4, 158 + BASELINE_OFFSET_MEDIUM20);
    canvas.print(line);
  }

  canvas.setFont(NULL); // revient a la police par defaut -- les autres ecrans (menus...) ne doivent pas heriter de celle-ci (canvas est un objet unique, reutilise a chaque frame)
}

// ===================== Confirmation d'arret (PUSH depuis le statut en enregistrement) =====================
// Ecran intercalaire, a la RaceChrono : PUSH pendant l'enregistrement ne
// stoppe plus directement, il ouvre cet ecran. REPRENDRE (PUSH) relance
// aussitot (circuit toujours arme). BACK confirme l'arret definitif
// (desarme le circuit). Timeout de securite si on oublie de choisir --
// cf. CONFIRM_STOP_TIMEOUT_MS.
static void drawConfirmStopScreen() {
  if (screenNeedsFullRedraw) {
    canvas.fillScreen(ST77XX_BLACK);
    screenNeedsFullRedraw = false;
  }
  canvas.setFont(NULL);
  canvas.setTextSize(2);
  canvas.setCursor(4, 6);
  canvas.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  canvas.print("Enregistrement");
  canvas.setCursor(4, 30);
  canvas.print("en pause");

  canvas.setTextSize(3);
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.setCursor(4, 90);
  canvas.print("REPRENDRE");
  canvas.setTextSize(1);
  canvas.setCursor(4, 118);
  canvas.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  canvas.print("(PUSH)");

  long remainingS = (long)(CONFIRM_STOP_TIMEOUT_MS - (millis() - confirmStopEnteredMs)) / 1000;
  if (remainingS < 0) remainingS = 0;
  clearLine(150, STATUS_LINE_H_TINY);
  canvas.setCursor(4, 150);
  char buf[48];
  snprintf(buf, sizeof(buf), "Arret auto dans %lds si rien", remainingS);
  canvas.print(buf);

  clearLine(canvas.height() - 15, STATUS_LINE_H_TINY);
  canvas.setCursor(4, canvas.height() - 15);
  canvas.print("BACK: stop definitif   PUSH: reprendre");
}

void loop() {
#if GPS_ENABLED
  pollGps();
#endif
  handleSerialCommands();
  webServerManager.loop();

  if (newGpsData) {
    newGpsData = false;
    double lat = liveData.latitude / 1e7;
    double lng = liveData.longitude / 1e7;
    float altM = liveData.wgsAltitude / 1000.0f;
    float speedKnots = liveData.speedMmPerS / 514.444f; // mm/s -> noeuds (unite attendue par la lib DovesLapTimer)
    processGpsFix(lat, lng, altM, speedKnots, millis());
    logGpsRow();          // no-op si REC off (cf. garde loggingOk en tete de fonction)
    checkLapCompletion(); // idem, no-op si REC off
  }

  if (screenState == SCREEN_STATUS) {
    if (rotaryEncoder.encoderChanged()) {
    }

    if (rotaryEncoder.isEncoderButtonClicked() &&
        (millis() - lastDefinitiveStopMs) >= STATUS_REC_GRACE_AFTER_STOP_MS) {
      if (recordingEnabled) {
        // PUSH ne stoppe plus directement -- passe par un ecran de
        // confirmation (cf. SCREEN_CONFIRM_STOP). stopRecording() ici agit
        // comme une pause : le fichier de log est ferme mais le circuit
        // reste arme (courseManager pas reset), pour permettre une reprise
        // rapide si c'etait un arret involontaire/entre deux runs.
        stopRecording();
        confirmStopEnteredMs = millis();
        screenNeedsFullRedraw = true;
        uiDirty = true;
        screenState = SCREEN_CONFIRM_STOP;
      } else {
        startRecording();
      }
    }

    if (pollBackPress()) {
      cancelNewCircuitCapture(); // no-op si rien n'est arme -- cf. commentaire pres de sa declaration
      mainMenuSelection = 0;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(mainMenuSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_CONFIRM_STOP) {
    if (rotaryEncoder.isEncoderButtonClicked() &&
        (millis() - confirmStopEnteredMs) >= CONFIRM_STOP_INPUT_GRACE_MS) {
      // REPRENDRE -- le circuit etait toujours arme (pas de reset), donc
      // startRecording() repart immediatement sans repasser par la detection.
      startRecording();
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_STATUS;
    }

    bool timedOut = (millis() - confirmStopEnteredMs) >= CONFIRM_STOP_TIMEOUT_MS;
    if (pollBackPress() || timedOut) {
      // Arret definitif : desarme le circuit (auto ou force manuellement)
      // pour qu'aucun faux contact ne puisse relancer l'enregistrement une
      // fois qu'on a quitte la piste. Le timeout couvre le cas ou on
      // oublie de confirmer (BACK) avant de prendre la route.
      activateAutoMode(); // reset complet du courseManager + des flags d'armement
      lastDefinitiveStopMs = millis(); // cf. STATUS_REC_GRACE_AFTER_STOP_MS -- le geofencing peut rearmer le circuit en ~1s
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_STATUS;
    }

  } else if (screenState == SCREEN_MAIN_MENU) {
    if (rotaryEncoder.encoderChanged()) {
      mainMenuSelection = rotaryEncoder.readEncoder();
      uiDirty = true;
    }

    if (rotaryEncoder.isEncoderButtonClicked()) {
      screenNeedsFullRedraw = true;
      uiDirty = true;
      if (mainMenuSelection == 0) { // Circuit
        menuSelection = manualOverrideActive ? (manualCourseIndex + 1) : 0;
        rotaryEncoder.setBoundaries(0, myTracks.courseCount, true); // Auto + circuits uniquement -- plus de New track/WiFi ici
        rotaryEncoder.setEncoderValue(menuSelection);
        screenState = SCREEN_CIRCUIT_MENU;
      } else if (mainMenuSelection == 1) { // Connexion
        screenState = SCREEN_CONNEXION;
      } else if (mainMenuSelection == 2) { // Session
        enterSessionList();
        rotaryEncoder.setBoundaries(0, sessionListCache.empty() ? 0 : (int)sessionListCache.size() - 1, false);
        rotaryEncoder.setEncoderValue(sessionListSelection);
        screenState = SCREEN_SESSION_LIST;
      } else if (mainMenuSelection == 3) { // Reglages
        screenState = SCREEN_SETTINGS;
      } else { // New track (capture)
        armNewCircuitCapture();
        screenState = SCREEN_STATUS;
      }
    }

    if (pollBackPress()) {
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_STATUS;
    }

  } else if (screenState == SCREEN_CIRCUIT_MENU) {
    if (rotaryEncoder.encoderChanged()) {
      menuSelection = rotaryEncoder.readEncoder();
      uiDirty = true;
    }

    if (rotaryEncoder.isEncoderButtonClicked()) {
      if (menuSelection == 0) {
        activateAutoMode();
      } else {
        activateManualCourse(menuSelection - 1);
      }
      screenState = SCREEN_STATUS;
      screenNeedsFullRedraw = true;
      uiDirty = true;
    }

    if (pollBackPress()) {
      mainMenuSelection = 0;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(mainMenuSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_CONNEXION) {
    if (pollBackPress()) {
      mainMenuSelection = 1;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(mainMenuSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_SESSION_LIST) {
    if (rotaryEncoder.encoderChanged()) {
      sessionListSelection = rotaryEncoder.readEncoder();
      uiDirty = true;
    }

    if (rotaryEncoder.isEncoderButtonClicked() && !sessionListCache.empty()) {
      enterSessionLaps();
      rotaryEncoder.setBoundaries(0, sessionLapsCache.empty() ? 0 : (int)sessionLapsCache.size() - 1, false);
      rotaryEncoder.setEncoderValue(sessionLapSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_SESSION_LAPS;
    }

    if (pollBackPress()) {
      mainMenuSelection = 2;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(mainMenuSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_SESSION_LAPS) {
    if (rotaryEncoder.encoderChanged()) {
      sessionLapSelection = rotaryEncoder.readEncoder();
      uiDirty = true;
    }

    if (pollBackPress()) {
      rotaryEncoder.setBoundaries(0, sessionListCache.empty() ? 0 : (int)sessionListCache.size() - 1, false);
      rotaryEncoder.setEncoderValue(sessionListSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_SESSION_LIST;
    }

  } else if (screenState == SCREEN_SETTINGS) {
    if (rotaryEncoder.isEncoderButtonClicked()) {
      webServerManager.startDownloadMode();
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_WIFI;
    }

    if (pollBackPress()) {
      mainMenuSelection = 3;
      rotaryEncoder.setBoundaries(0, 4, true);
      rotaryEncoder.setEncoderValue(mainMenuSelection);
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_MAIN_MENU;
    }

  } else { // SCREEN_WIFI
    if (pollBackPress()) {
      webServerManager.stopDownloadMode();
      screenNeedsFullRedraw = true;
      uiDirty = true;
      screenState = SCREEN_STATUS;
    }
  }

  // Ecrans "en direct" (donnees GPS/chrono qui changent en continu) :
  // redessin regulier comme avant. Ecrans "menu" (statiques entre deux
  // actions) : seulement si quelque chose a change (uiDirty) -- evite de
  // redessiner 4x/seconde un menu identique a la frame precedente,
  // source du clignotement residuel constate au banc.
  bool isLiveScreen = (screenState == SCREEN_STATUS || screenState == SCREEN_CONNEXION ||
                       screenState == SCREEN_CONFIRM_STOP); // decompte du timeout -- doit se rafraichir seul
  static unsigned long lastRender = 0;
  bool timeToRender = isLiveScreen && (millis() - lastRender >= 250);
  if (timeToRender || (!isLiveScreen && uiDirty)) {
    lastRender = millis();
    uiDirty = false;
    switch (screenState) {
      case SCREEN_STATUS:       drawStatusScreen(); break;
      case SCREEN_MAIN_MENU:    drawMainMenuScreen(); break;
      case SCREEN_CIRCUIT_MENU: drawCircuitMenu(); break;
      case SCREEN_CONNEXION:    drawConnexionScreen(); break;
      case SCREEN_SESSION_LIST: drawSessionListScreen(); break;
      case SCREEN_SESSION_LAPS: drawSessionLapsScreen(); break;
      case SCREEN_SETTINGS:     drawSettingsScreen(); break;
      case SCREEN_CONFIRM_STOP: drawConfirmStopScreen(); break;
      default:                  drawWifiScreen(); break;
    }
    pushCanvasToDisplay(); // un seul envoi SPI complet, une fois l'ecran hors-ligne entierement compose
  }
}
