/**
 * Firmware Chrono GPS moto piste -- variante OLED + encodeur (ESP32-S3-Zero)
 * --------------------------------------------------------------------------
 * Pour la moto du fils : pas d'affichage live (reste concentre sur le
 * pilotage). L'ecran OLED n'affiche que "dernier tour / meilleur tour",
 * mis a jour uniquement quand un tour se termine -- pas un chrono qui
 * defile en continu comme la version TFT.
 *
 * GPS : module u-blox NEO-M8N en UART direct (UBX binaire, NAV-PVT a
 * 10Hz) -- remplace le RaceBox/BLE utilise dans les versions precedentes.
 * Plus de scan/connexion a gerer, plus de coexistence radio avec le WiFi
 * a arbitrer : le GPS est cable, il tourne en continu quel que soit
 * l'etat du WiFi.
 *
 * Meme logique CourseManager/logs/WiFi que le firmware TFT principal
 * (lap_timer_firmware) -- seuls l'affichage et l'entree (encodeur+boutons
 * plutot que TFT) changent. cf. main_reference.cpp pour la version TFT.
 *
 * Encodeur : sur l'ecran statut, PUSH (clic de l'encodeur) demarre/arrete
 * l'enregistrement, CONFIRM force le mode proximite si la detection auto
 * reste bloquee -- deux boutons, deux roles distincts (cf. section dediee
 * plus bas, pres de DETECTION_FALLBACK_DISTANCE_M). Ailleurs (menus) :
 * CONFIRM et PUSH restent redondants, un seul choix possible ("valider").
 * BACK = ouvre le menu de selection de circuit (rotation + PUSH/CONFIRM
 * pour valider, BACK pour annuler).
 */

#include <Arduino.h>
#include <string.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <AiEsp32RotaryEncoder.h>
#include <CourseManager.h>
#include <LittleFS.h>
#include <time.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include "SdLogStorage.h"
#include "WebServerManager.h"
#include "GpsManager.h"
#include <vector>
#include "logo_pigteam_xbm.h"

// ===================== Pins (ESP32-S3-Tiny N8R8 -- evite 19/20 USB natif, 33-37 PSRAM octal) =====================
// Numerotation choisie dans l'ordre physique du connecteur du module
// (CONFIRM, SDA, SCL, PUSH, A, B, BACK) -- facilite le cablage/soudure,
// les fils se suivent sans croisement.
// Migration depuis l'ESP32-S3-Zero (7-13) -> ESP32-S3-Tiny (12-18) :
// meme sequence de pins decalee de +5, GPS inchange (4, 5).

#define CONFIRM_BUTTON 12
#define I2C_SDA        13
#define I2C_SCL        14
#define ENCODER_PUSH   15
#define ENCODER_CLK    16
#define ENCODER_DT     17
#define BACK_BUTTON    18

// SD (logs GPS detailles) -- bus SPI dedie, pas de partage possible avec
// l'ecran comme sur la variante TFT (celui-ci est en I2C ici, aucun bus
// SPI existant a reutiliser). Cablage valide au banc (sketch isole
// sd-wiring-test-oled, cf. README_FS.md).
#define SD_CS    7
#define SD_MOSI  8
#define SD_MISO  9
#define SD_SCLK  10

// GPS : pins, config UBX et parsing NAV-PVT deplaces dans
// GpsManager.cpp/h (cf. cette section pour le detail module/pins).

// ===================== Affichage OLED (U8g2) =====================
//
// SH1106 par defaut (le plus probable pour ce module 1.3" 128x64 d'apres
// la fiche produit). Si l'image sort deformee/dedoublee, essaie la ligne
// SH1107 commentee juste en dessous -- aucun risque, juste un affichage
// illisible en cas d'erreur de driver.

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
// U8G2_SH1107_64X128_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

static String formatLapTime(unsigned long ms) {
  unsigned long minutes = ms / 60000;
  unsigned long seconds = (ms / 1000) % 60;
  unsigned long millis_ = ms % 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02lu.%03lu", minutes, seconds, millis_);
  return String(buf);
}

// Inverse de formatLapTime() -- "M:SS.mmm" -> ms. Utilise pour recalculer
// le VRAI meilleur temps d'une session a l'affichage (cf.
// drawSessionLapsScreen()) plutot que de se fier a la colonne
// "meilleur_temps" du CSV, qui n'est que le meilleur connu AU MOMENT de
// ce tour precis (une best-so-far qui progresse tour apres tour), pas le
// meilleur final de toute la session. Retourne -1 si illisible.
static long lapTimeStrToMs(const String& t) {
  int colon = t.indexOf(':');
  if (colon < 0) return -1;
  long minutes = t.substring(0, colon).toInt();
  float seconds = t.substring(colon + 1).toFloat();
  return minutes * 60000L + (long)(seconds * 1000.0f + 0.5f);
}

// ===================== Encodeur + boutons =====================

AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ENCODER_DT, ENCODER_CLK, ENCODER_PUSH, -1, 4); // A/B inverses ici (pas dans les #define ni le cablage) pour inverser le sens de rotation logique

void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

// CONFIRM et BACK lus par interruption materielle (comme l'encodeur),
// plutot que par sondage du GPIO a chaque tour de boucle -- le sondage
// peut manquer un appui court si la boucle principale est momentanement
// ralentie (webServerManager.loop() pendant le WiFi, par exemple).
// L'ISR se contente d'enregistrer le NIVEAU BRUT actuel + l'instant du
// dernier changement -- la confirmation "c'est un vrai appui stable, pas
// du rebond" se fait dans la boucle principale en
// verifiant que ce niveau est reste stable au moins BUTTON_DEBOUNCE_MS.
// (Un anti-rebond fait entierement dans l'ISR avec un seul horodatage
// partage entre appui/relachement peut a tort avaler l'un des deux s'ils
// sont rapprochs -- evite ici.)

const unsigned long BUTTON_DEBOUNCE_MS = 25;

volatile bool isr_confirmRawLow = false;
volatile unsigned long isr_confirmEdgeAt = 0;
volatile bool isr_backRawLow = false;
volatile unsigned long isr_backEdgeAt = 0;
// PUSH (axe de l'encodeur) -- meme traitement ISR que CONFIRM/BACK ci-dessus,
// en plus de ce que gere deja la lib AiEsp32RotaryEncoder en interne (son
// isEncoderButtonClicked() reste utilise ailleurs pour les menus). On a
// besoin ici de notre propre mesure de duree d'appui (cf. pollPushRelease()),
// que la lib n'expose pas facilement sans bloquer la boucle -- son
// isEncoderButtonClicked() attend jusqu'a 300ms en interne (delay()) avant
// de rendre la main, ce qu'on veut eviter ici comme pour CONFIRM/BACK.
volatile bool isr_pushRawLow = false;
volatile unsigned long isr_pushEdgeAt = 0;

void IRAM_ATTR confirmButtonISR() {
  isr_confirmRawLow = (digitalRead(CONFIRM_BUTTON) == LOW);
  isr_confirmEdgeAt = millis();
}

void IRAM_ATTR backButtonISR() {
  isr_backRawLow = (digitalRead(BACK_BUTTON) == LOW);
  isr_backEdgeAt = millis();
}

void IRAM_ATTR pushButtonISR() {
  isr_pushRawLow = (digitalRead(ENCODER_PUSH) == LOW);
  isr_pushEdgeAt = millis();
}

// true exactement au moment ou un appui CONFIRM stable est confirme
// (transition vers LOW maintenue depuis au moins BUTTON_DEBOUNCE_MS).
static bool pollConfirmPress() {
  static bool confirmedLow = false;
  bool rawLow; unsigned long edgeAt;
  noInterrupts();
  rawLow = isr_confirmRawLow;
  edgeAt = isr_confirmEdgeAt;
  interrupts();

  if (rawLow != confirmedLow && millis() - edgeAt >= BUTTON_DEBOUNCE_MS) {
    confirmedLow = rawLow;
    return confirmedLow; // true seulement sur la transition VERS l'appui, pas vers le relachement
  }
  return false;
}

// true exactement au moment ou un relachement BACK stable est confirme
// (apres avoir ete stable appuye) -- remplit heldMsOut avec la duree de l'appui.
static bool pollBackRelease(unsigned long& heldMsOut) {
  static bool confirmedLow = false;
  static unsigned long pressedSinceMs = 0;
  bool rawLow; unsigned long edgeAt;
  noInterrupts();
  rawLow = isr_backRawLow;
  edgeAt = isr_backEdgeAt;
  interrupts();

  if (rawLow != confirmedLow && millis() - edgeAt >= BUTTON_DEBOUNCE_MS) {
    confirmedLow = rawLow;
    if (confirmedLow) {
      pressedSinceMs = millis();
    } else {
      heldMsOut = millis() - pressedSinceMs;
      return true;
    }
  }
  return false;
}

// true exactement au moment ou un relachement CONFIRM stable est confirme --
// meme logique que pollBackRelease(), duree d'appui en sortie pour
// distinguer court/long depuis l'ecran statut (cf. SCREEN_STATUS plus bas).
// Fonctionne en parallele de pollConfirmPress() (statics separes, meme
// source ISR) -- pollConfirmPress() reste utilise tel quel dans les menus,
// qui n'ont pas besoin de distinguer court/long.
static bool pollConfirmRelease(unsigned long& heldMsOut) {
  static bool confirmedLow = false;
  static unsigned long pressedSinceMs = 0;
  bool rawLow; unsigned long edgeAt;
  noInterrupts();
  rawLow = isr_confirmRawLow;
  edgeAt = isr_confirmEdgeAt;
  interrupts();

  if (rawLow != confirmedLow && millis() - edgeAt >= BUTTON_DEBOUNCE_MS) {
    confirmedLow = rawLow;
    if (confirmedLow) {
      pressedSinceMs = millis();
    } else {
      heldMsOut = millis() - pressedSinceMs;
      return true;
    }
  }
  return false;
}

// true exactement au moment ou un relachement PUSH (axe encodeur) stable
// est confirme -- meme logique que pollBackRelease()/pollConfirmRelease().
static bool pollPushRelease(unsigned long& heldMsOut) {
  static bool confirmedLow = false;
  static unsigned long pressedSinceMs = 0;
  bool rawLow; unsigned long edgeAt;
  noInterrupts();
  rawLow = isr_pushRawLow;
  edgeAt = isr_pushEdgeAt;
  interrupts();

  if (rawLow != confirmedLow && millis() - edgeAt >= BUTTON_DEBOUNCE_MS) {
    confirmedLow = rawLow;
    if (confirmedLow) {
      pressedSinceMs = millis();
    } else {
      heldMsOut = millis() - pressedSinceMs;
      return true;
    }
  }
  return false;
}

// timegm() n'est pas disponible sur ce toolchain (extension GNU absente de la
// libc embarquee) -- calcul manuel equivalent : jours ecoules depuis 1970-01-01.
static time_t utcTmToEpoch(const struct tm& utcTm) {
  static const int cumDaysBeforeMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int year = utcTm.tm_year + 1900;
  int month = utcTm.tm_mon; // 0-11

  long days = 0;
  for (int y = 1970; y < year; y++) {
    days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
  }
  days += cumDaysBeforeMonth[month];
  bool isLeapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month > 1 && isLeapYear) days += 1; // 29 fevrier de l'annee en cours, si on est apres fevrier
  days += utcTm.tm_mday - 1;

  return (time_t)(days * 86400L + utcTm.tm_hour * 3600L + utcTm.tm_min * 60L + utcTm.tm_sec);
}

// Convertit les champs UTC du GPS (liveData) en date/heure locale francaise,
// avec gestion automatique de l'heure d'ete (CET/CEST) -- pas besoin d'ajuster
// quoi que ce soit entre l'hiver et l'ete, le calcul se fait pour la date exacte.
static void getLocalDateTime(char* dateBuf, size_t dateBufSize, char* timeBuf, size_t timeBufSize) {
  struct tm utcTm = {};
  utcTm.tm_year = liveData.year - 1900;
  utcTm.tm_mon  = liveData.month - 1;
  utcTm.tm_mday = liveData.day;
  utcTm.tm_hour = liveData.hour;
  utcTm.tm_min  = liveData.minute;
  utcTm.tm_sec  = liveData.second;

  time_t utcEpoch = utcTmToEpoch(utcTm);
  struct tm localTm;
  localtime_r(&utcEpoch, &localTm); // reconvertit en heure locale selon TZ (cf. setup())

  if (dateBuf) snprintf(dateBuf, dateBufSize, "%04d-%02d-%02d", localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday);
  if (timeBuf) snprintf(timeBuf, timeBufSize, "%02d:%02d:%02d", localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
}

// Variante compacte "AAAAMMJJ_HHMMSS", utilisee pour nommer les fichiers de log par session.
static void getLocalDateTimeCompact(char* buf, size_t bufSize) {
  struct tm utcTm = {};
  utcTm.tm_year = liveData.year - 1900;
  utcTm.tm_mon  = liveData.month - 1;
  utcTm.tm_mday = liveData.day;
  utcTm.tm_hour = liveData.hour;
  utcTm.tm_min  = liveData.minute;
  utcTm.tm_sec  = liveData.second;

  time_t utcEpoch = utcTmToEpoch(utcTm);
  struct tm localTm;
  localtime_r(&utcEpoch, &localTm);

  snprintf(buf, bufSize, "%04d%02d%02d_%02d%02d%02d",
           localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
           localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
}

// ===================== Chrono multi-circuits (CourseManager) =====================
//
// Les circuits ne sont plus codes en dur : ils vivent dans /circuits.csv
// (LittleFS), editables depuis la page web /circuits (cf.
// WebServerManager.cpp -- qui parse ce meme fichier independamment, meme
// principe de decouplage que /sessions.csv). Seuls les circuits coches
// "actif" sont charges ici dans myTracks/CourseManager au demarrage --
// max MAX_COURSES (8, limite fixe de la lib DovesLapTimer), impose deja
// cote page web pour eviter toute confusion si plus de 8 sont coches.
//
// La detection se base d'abord sur la longueur de tour parcourue (~1
// tour), puis valide le candidat par un vrai franchissement de ligne
// avant de l'activer -- donc robuste meme si deux circuits ont des
// longueurs proches (Ales/Clastres par exemple).
//
// Si aucun des circuits actifs ne correspond, bascule automatique en
// mode "Lap Anything" (proximite, comme le test pate de maison) apres
// quelques tours non valides -- pas besoin d'intervenir.

static const char* CIRCUITS_FILE_PATH = "/circuits.csv";
// 18e colonne "locked" geree cote WebServerManager.cpp uniquement (page
// /circuits -- protection contre suppression accidentelle) -- main.cpp
// n'en a pas besoin, sa lecture s'arrete a 17 champs (cf.
// loadActiveCircuitsIntoTracks() plus bas) et l'ignore silencieusement.
static const char* CIRCUITS_CSV_HEADER =
  "active,name,length_ft,sa_lat,sa_lng,sb_lat,sb_lng,has2,s2a_lat,s2a_lng,s2b_lat,s2b_lng,has3,s3a_lat,s3a_lng,s3b_lat,s3b_lng,locked";

// Stockage stable pour CourseConfig::name -- c'est un simple const char*
// cote lib (pas de copie interne des caracteres), donc il faut des
// buffers qui restent valides pendant toute la duree de vie du firmware,
// pas des String temporaires qui pourraient bouger en memoire.
static char courseNameBuf[MAX_COURSES][32];

// Les 8 circuits calibres pendant le projet -- utilises uniquement pour
// creer circuits.csv au tout premier demarrage (fichier absent).
struct DefaultCircuit {
  const char* name; float lengthFt;
  double saLat, saLng, sbLat, sbLng;
  double s2aLat, s2aLng, s2bLat, s2bLng;
  double s3aLat, s3aLng, s3bLat, s3bLng;
};
static const DefaultCircuit DEFAULT_CIRCUITS[] = {
  { "Croix-en-Ternois", 5868.0f,
    50.3789502, 2.2963331, 50.3790493, 2.2962665,
    50.3791311, 2.2982143, 50.3790356, 2.2982926,
    50.3778342, 2.2965775, 50.3777539, 2.2964647 },
  { "Carole", 6698.9f,
    48.9787465, 2.5228866, 48.9787541, 2.5227100,
    48.9796083, 2.5214165, 48.9795791, 2.5215943,
    48.9798820, 2.5200980, 48.9798562, 2.5199926 },
  { "Pau-Arnos", 9747.0f,
    43.4470894, -0.5326126, 43.4471380, -0.5324800,
    43.4457163, -0.5338977, 43.4456372, -0.5339986,
    43.4464610, -0.5325110, 43.4464108, -0.5326424 },
  { "Ledenon", 10241.7f,
    43.9232182, 4.5040888, 43.9231970, 4.5042355,
    43.9223838, 4.5062905, 43.9224288, 4.5064265,
    43.9248721, 4.5081974, 43.9248387, 4.5080551 },
  { "Ales", 8031.4f,
    44.1550805, 4.0715175, 44.1550719, 4.0713678,
    44.1583572, 4.0738910, 44.1583988, 4.0740296,
    44.1550463, 4.0737947, 44.1550150, 4.0739385 },
  { "Clastres", 7770.0f,
    49.7506247, 3.2110847, 49.7506568, 3.2112440,
    49.7514874, 3.2081266, 49.7514101, 3.2080103,
    49.7521115, 3.2045187, 49.7521830, 3.2043939 },
  { "Los Arcos", 12757.8f,
    42.5591516, -2.1679944, 42.5591107, -2.1681299,
    42.5605311, -2.1654386, 42.5605685, -2.1655758,
    42.5603685, -2.1639580, 42.5602760, -2.1638828 },
  { "Le Mans Bugatti", 14050.3f,
    47.9498714, 0.2075227, 47.9498732, 0.2072545,
    47.9575404, 0.2118249, 47.9576314, 0.2117387,
    47.9561026, 0.2148935, 47.9562427, 0.2147255 },
};
static const int DEFAULT_CIRCUITS_COUNT = sizeof(DEFAULT_CIRCUITS) / sizeof(DEFAULT_CIRCUITS[0]);

static TrackConfig myTracks = { "Mes circuits PIGTEAM", "PIGTEAM", {}, 0 }; // rempli au demarrage par loadActiveCircuitsIntoTracks() plus bas

// Cree circuits.csv avec le jeu de circuits d'origine (tous actifs,
// tous proteges -- ce sont les references calibrees du projet, autant
// eviter qu'un fat-finger sur "Supprimer" fasse perdre Croix-en-Ternois
// juste avant le track day). Appele uniquement si le fichier n'existe
// pas encore (premier demarrage apres cette mise a jour du firmware).
static void seedDefaultCircuitsFile() {
  File f = LittleFS.open(CIRCUITS_FILE_PATH, "w");
  if (!f) { Serial.println("Circuits: impossible de creer circuits.csv."); return; }
  f.println(CIRCUITS_CSV_HEADER);
  for (int i = 0; i < DEFAULT_CIRCUITS_COUNT; i++) {
    const DefaultCircuit& c = DEFAULT_CIRCUITS[i];
    f.printf("1,%s,%.1f,%.7f,%.7f,%.7f,%.7f,1,%.7f,%.7f,%.7f,%.7f,1,%.7f,%.7f,%.7f,%.7f,1\n",
             c.name, c.lengthFt, c.saLat, c.saLng, c.sbLat, c.sbLng,
             c.s2aLat, c.s2aLng, c.s2bLat, c.s2bLng,
             c.s3aLat, c.s3aLng, c.s3bLat, c.s3bLng);
  }
  f.close();
  Serial.println("Circuits: circuits.csv cree avec les 8 circuits d'origine (tous actifs, tous proteges).");
}

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

// Charge les circuits coches "actif" dans myTracks (max MAX_COURSES) --
// cree circuits.csv avec le jeu par defaut si le fichier n'existe pas
// encore. Le reste des circuits (inactifs) n'est pas charge ici -- seule
// la page web /circuits les lit tous, pour l'edition (cf.
// WebServerManager.cpp). atof() plutot que String::toFloat() pour les
// coordonnees : simple precision (float) perdrait des chiffres
// significatifs sur des latitudes/longitudes a 7 decimales, ce qui
// degraderait la precision de la ligne de detection.
static void loadActiveCircuitsIntoTracks() {
  if (!LittleFS.exists(CIRCUITS_FILE_PATH)) seedDefaultCircuitsFile();

  File f = LittleFS.open(CIRCUITS_FILE_PATH, "r");
  if (!f) {
    Serial.println("Circuits: impossible d'ouvrir circuits.csv -- aucun circuit charge (mode proximite par defaut).");
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

// Construit dans setup(), une fois circuits.csv charge dans myTracks --
// impossible de le construire directement ici comme avant (initialisation
// statique globale, avant meme que LittleFS soit monte).
static CourseManager* courseManager = nullptr;
static int lastLapCount = 0;

// ----- Contournement maison du fallback Lap Anything de la lib -----
//
// Constate sur le terrain (portion de rocade, 5.45km, 5 passages a moins
// de 10m du point de depart sur 5min26 -- log GPS a l'appui) : le
// mecanisme integre de CourseDetector peut rester bloque en detection
// indefiniment meme apres plusieurs vrais bouclages, sans jamais
// atteindre les 3 rejets necessaires pour basculer tout seul vers le
// mode proximite (raison exacte pas identifiee avec certitude sans
// logs Serial en direct -- possiblement le waypoint de reference pose
// avant meme le debut de l'enregistrement). Plutot que de patcher la
// lib (fork), on la contourne depuis main.cpp : le chrono proximite
// (_lapAnythingTimer, expose via getLapAnythingTimer()) tourne de toute
// facon en permanence en tache de fond, meme quand le flag interne de
// la lib ne bascule jamais -- on s'en sert directement des qu'une
// distance raisonnable a ete parcourue sans que la detection n'ait
// abouti, sans dependre du mecanisme de rejets de la lib.
//
// Seuil choisi nettement au-dessus du plus long circuit connu (Le Mans
// Bugatti, 14050ft/4283m) pour laisser une vraie chance a la detection
// auto de reconnaitre un circuit calibre avant de forcer la bascule.
// Volontairement plus un filet de securite qu'un mecanisme principal
// depuis l'ajout du bouton CONFIRM dedie (cf. forceLapAnythingManually()) --
// peut rester relativement genereux, le pilote n'a plus besoin d'attendre
// dessus s'il sait deja que la detection ne peut pas aboutir.
static const float DETECTION_FALLBACK_DISTANCE_M = 4500.0f;
static bool forcedLapAnything = false; // remis a false a chaque courseManager->reset() (cf. activateAutoMode())

// A utiliser partout dans l'affichage/logs a la place de
// courseManager->isLapAnythingActive() / ->isDetectionComplete() --
// couvre a la fois la vraie bascule de la lib ET notre contournement.
static bool lapAnythingEffective() {
  return courseManager->isLapAnythingActive() || forcedLapAnything;
}
static bool detectionEffectivelyComplete() {
  return courseManager->isDetectionComplete() || forcedLapAnything;
}

// ===================== Mode manuel (forcer un circuit, contourne la detection auto) =====================
//
// CourseManager n'expose pas de methode pour forcer directement un circuit
// (verifie dans son API -- seule la detection auto existe). Solution : un
// DovesLapTimer independant, configure avec la ligne du circuit choisi a la
// main via le menu. Quand actif, ce timer remplace totalement le CourseManager
// dans les fonctions d'affichage/log -- les deux sont mutuellement exclusifs.

static DovesLapTimer manualTimer(7.0, &Serial);
static bool manualOverrideActive = false;
static int manualCourseIndex = -1; // index dans myTracks.courses, -1 = aucun

// Force le mode proximite immediatement, depuis le bouton CONFIRM
// (ecran statut, appui court -- bouton dedie depuis que PUSH a recupere
// seul le demarrage/arret d'enregistrement) -- pour le pilote qui sait
// deja que la detection ne peut pas aboutir (ex : portion de route sans
// rapport avec aucun circuit calibre) et ne veut pas attendre
// DETECTION_FALLBACK_DISTANCE_M.
// No-op (avec trace Serial) si deja resolu d'une facon ou d'une autre --
// evite d'ecraser une vraie detection de circuit connu par erreur.
// Arme la capture UNIQUEMENT sur declenchement intentionnel (CONFIRM,
// cf. forceLapAnythingManually() juste apres) -- pas sur un repli
// automatique en proximite (3 rejets natifs, ou notre contournement
// 4500m/tour deja boucle). Sans ce garde, une balade improvisee ou rien
// n'est reconnu finirait par ecrire un circuit parasite dans
// circuits.csv sans que ce soit demande. Le mode proximite/comptage de
// tours, lui, continue de fonctionner normalement dans tous les cas --
// seule l'ecriture du fichier est concernee par ce flag.
// Capture automatique de nouveau circuit (cf. section dediee plus bas,
// juste avant processGpsFix()) -- declare ici pour la meme raison que
// geofenceCheckDone plus bas : activateAutoMode() (et cancelForcedLapAnything()
// juste apres) doivent pouvoir les remettre a zero.
static bool pendingNewCircuitCapture = false;
static double pendingSaLat = 0, pendingSaLng = 0, pendingSbLat = 0, pendingSbLng = 0;
static bool newCircuitAutoSaved = false;
static double prevWaypointLat = 0, prevWaypointLng = 0;

static bool newCircuitCaptureArmed = false;

static void forceLapAnythingManually() {
  if (manualOverrideActive) {
    Serial.println("CONFIRM ignore -- circuit deja force manuellement (menu Circuit).");
    return;
  }
  if (courseManager->isDetectionComplete()) {
    Serial.println("CONFIRM ignore -- detection deja aboutie (circuit connu ou proximite deja active).");
    return;
  }
  forcedLapAnything = true;
  newCircuitCaptureArmed = true; // seul declencheur autorise pour la capture auto de nouveau circuit -- cf. commentaire pres de sa declaration
  Serial.println("Mode proximite force manuellement (CONFIRM) -- capture de nouveau circuit armee.");
}

// Annule un CONFIRM presse par erreur -- appelee depuis BACK sur l'ecran
// statut (cf. plus bas). Ne fait rien si rien n'a ete force (cas normal,
// BACK ouvre juste le menu), et ne fait rien non plus si une capture a
// deja ete ECRITE dans circuits.csv (newCircuitAutoSaved) -- a ce stade
// annuler n'aurait pas de sens, le fichier existe deja ; le pilote peut
// toujours la supprimer depuis /circuits si besoin.
static void cancelForcedLapAnything() {
  if (!forcedLapAnything && !newCircuitCaptureArmed) return; // rien a annuler
  if (newCircuitAutoSaved) {
    Serial.println("BACK : capture de circuit deja ecrite dans circuits.csv -- rien a annuler ici, supprime-la sur /circuits si besoin.");
    return;
  }
  forcedLapAnything = false;
  newCircuitCaptureArmed = false;
  pendingNewCircuitCapture = false;
  prevWaypointLat = 0; prevWaypointLng = 0;
  Serial.println("BACK : mode proximite force annule -- retour a la detection normale.");
}


// Geofencing (cf. section dediee juste apres activateAutoMode()) --
// declare ici, avant activateManualCourse()/activateAutoMode(), car ce
// dernier remet geofenceCheckDone a false.
static const float GEOFENCE_MAX_DISTANCE_M = 15000.0f; // 15km -- large marge, cf. discussion sur l'espacement reel des circuits
static bool geofenceCheckDone = false;

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
  Serial.printf("Circuit force manuellement : %s\n", c.name);
}

static void activateAutoMode() {
  manualOverrideActive = false;
  manualCourseIndex = -1;
  courseManager->reset();
  forcedLapAnything = false; // nouvelle detection depuis zero -- le contournement doit repartir de zero aussi
  geofenceCheckDone = false; // idem -- autorise un nouveau check au prochain fix GPS
  lastLapCount = 0;
  pendingNewCircuitCapture = false; // idem -- capture de nouveau circuit repart de zero aussi
  newCircuitAutoSaved = false;
  newCircuitCaptureArmed = false; // "Auto (detection)" desarme la capture -- il faudra un nouvel appui sur CONFIRM pour la reactiver
  prevWaypointLat = 0; prevWaypointLng = 0;
  Serial.println("Retour en mode detection automatique.");
}

// ===================== Geofencing (reconnaissance quasi instantanee sur circuit connu) =====================
//
// L'algorithme de la lib (CourseDetector) ne connait pas la position des
// circuits, seulement leur longueur de tour -- il faut donc boucler au
// moins un tour avant de confirmer un circuit (cf. discussion). Inutile
// dans ton cas : tes circuits sont espaces de dizaines de km, donc une
// simple distance a vol d'oiseau au premier fix GPS suffit a lever toute
// ambiguite, avant meme d'avoir bouge -- comme RaceChrono.
//
// Verifie une seule fois (au premier fix GPS valide, ou apres un retour
// explicite en "Auto (detection)") -- les circuits sont fixes, inutile de
// re-verifier en continu. Si un circuit actif est a moins de
// GEOFENCE_MAX_DISTANCE_M, il est active directement (reutilise
// activateManualCourse() -- meme mecanisme que le choix manuel au menu,
// juste declenche automatiquement). Sinon, aucun effet : l'algorithme de
// detection normal (par tour + longueur, cf. plus haut) prend le relais
// sans rien savoir de cette verification.

static void checkCircuitGeofence(double lat, double lng) {
  geofenceCheckDone = true;
  if (manualOverrideActive) return; // deja force manuellement (menu) avant meme le premier fix -- ne pas ecraser le choix du pilote

  int bestIdx = -1;
  double bestDist = 1e18;
  for (int i = 0; i < myTracks.courseCount; i++) {
    CourseConfig& c = myTracks.courses[i];
    // Point milieu de la ligne depart/arrivee -- suffisant, pas besoin
    // d'une precision fine puisque le seuil (15km) est tres large devant
    // la taille d'un circuit (quelques centaines de metres a 4km).
    double midLat = (c.startALat + c.startBLat) / 2.0;
    double midLng = (c.startALng + c.startBLng) / 2.0;
    double d = geoHaversine(lat, lng, midLat, midLng); // geoHaversine vient de GeoMath.h, inclus transitivement via CourseManager.h
    if (d < bestDist) { bestDist = d; bestIdx = i; }
  }

  if (bestIdx >= 0 && bestDist <= GEOFENCE_MAX_DISTANCE_M) {
    Serial.printf("Geofencing : a %.1fkm de \"%s\" -- activation directe (pas besoin d'attendre un tour).\n",
                  bestDist / 1000.0, myTracks.courses[bestIdx].name);
    activateManualCourse(bestIdx);
  } else {
    Serial.println("Geofencing : aucun circuit actif a moins de 15km -- detection normale par tour+longueur.");
  }
}

// Calcule l'etat d'affichage courant, quel que soit le mode actif : circuit
// force manuellement, circuit auto-detecte, ou repli proximite (Lap Anything).
static void getDisplayState(unsigned long& currentLapMs, unsigned long& bestLapMs, bool& hasBest, int& lapsCount, float& totalDistanceM) {
  currentLapMs = 0; bestLapMs = 0; hasBest = false; lapsCount = 0; totalDistanceM = 0;

  if (manualOverrideActive) {
    if (manualTimer.getRaceStarted()) currentLapMs = manualTimer.getCurrentLapTime();
    hasBest = manualTimer.getBestLapNumber() > 0;
    bestLapMs = manualTimer.getBestLapTime();
    lapsCount = manualTimer.getLaps();
    totalDistanceM = manualTimer.getTotalDistanceTraveled();
  } else if (lapAnythingEffective()) {
    WaypointLapTimer* t = courseManager->getLapAnythingTimer();
    if (t->getRaceStarted()) currentLapMs = t->getCurrentLapTime();
    hasBest = t->getBestLapNumber() > 0;
    bestLapMs = t->getBestLapTime();
    lapsCount = t->getLaps();
    totalDistanceM = t->getTotalDistanceTraveled();
  } else if (DovesLapTimer* t = courseManager->getActiveTimer()) {
    if (t->getRaceStarted()) currentLapMs = t->getCurrentLapTime();
    hasBest = t->getBestLapNumber() > 0;
    bestLapMs = t->getBestLapTime();
    lapsCount = t->getLaps();
    totalDistanceM = t->getTotalDistanceTraveled();
  }
}

static unsigned long getLastFinishedLapMs() {
  if (manualOverrideActive) return manualTimer.getLastLapTime();
  if (lapAnythingEffective()) return courseManager->getLapAnythingTimer()->getLastLapTime();
  if (DovesLapTimer* t = courseManager->getActiveTimer()) return t->getLastLapTime();
  return 0;
}

// Nom du circuit actif, pour l'affichage -- couvre les 3 modes possibles.
static const char* getActiveCourseNameForDisplay() {
  if (manualOverrideActive) return myTracks.courses[manualCourseIndex].name;
  if (!detectionEffectivelyComplete()) return "Detection...";
  if (lapAnythingEffective()) return "Inconnu (proximite)";
  return courseManager->getActiveCourseName();
}

// ===================== Enregistrement (style RaceBox) =====================
//
// Plus de bouton physique dedie -- CONFIRM (ecran statut) fait office de
// bouton REC. Le chrono tourne TOUJOURS pour l'affichage ; ceci ne controle
// que l'ECRITURE dans les logs, comme le bouton du RaceBox controle SON
// enregistrement standalone independamment du flux BLE live.

static bool recordingEnabled = false;

// ===================== Mode debogage (rejeu d'un vrai log dans le VRAI pipeline) =====================
//
// Ce mode rejoue un fichier au MEME format que ceux produits par le
// chrono en usage normal (log_AAAAMMJJ_HHMMSS.csv) -- renomme/uploade
// sous /debug_replay.csv -- a travers le VRAI pipeline de production :
// processGpsFix(), donc geofencing, detection/contournement, demarrage
// auto de l'enregistrement, logs. Objectif : valider la logique de
// detection sur un vrai trajet enregistre, sans avoir a rouler a chaque
// modification de code.
//
// A n'activer QUE sur un ESP sans GPS reel branche (ESP de rechange,
// cf. discussion) -- si un GPS repond, ce mode refuse de s'activer
// (garde ci-dessous) : les deux flux de donnees (GPS reel + rejeu)
// ecriraient tous les deux dans liveData/courseManager, resultat
// incoherent garanti.
static void processGpsFix(double lat, double lng, float altM, float speedKnots, unsigned long timeMs); // definie juste avant loop(), plus bas dans le fichier

static const char* DEBUG_REPLAY_FILE_PATH = "/debug_replay.csv";
static File debugReplayFile;
static bool debugReplayActive = false;
static long debugReplayFirstTimestampMs = -1;
static unsigned long debugReplayStartedAtMillis = 0;
const float DEBUG_REPLAY_SPEED_MULTIPLIER = 10.0f;

// Convertit "HH:MM:SS" (colonne local_time du log) en millisecondes
// depuis minuit -- suffisant pour calculer un delta entre deux lignes,
// pas besoin de la date (une session ne traverse jamais minuit en
// pratique).
static long parseLocalTimeToMs(const String& hhmmss) {
  if (hhmmss.length() < 8) return 0;
  int h = hhmmss.substring(0, 2).toInt();
  int m = hhmmss.substring(3, 5).toInt();
  int s = hhmmss.substring(6, 8).toInt();
  return ((long)h * 3600L + m * 60L + s) * 1000L;
}

static void debugReplayTick() {
  if (!debugReplayActive || !debugReplayFile) return;

  unsigned long realElapsedMs = millis() - debugReplayStartedAtMillis;
  long targetElapsedMs = (long)(realElapsedMs * DEBUG_REPLAY_SPEED_MULTIPLIER);

  while (true) {
    uint32_t posBeforeLine = debugReplayFile.position();
    if (!debugReplayFile.available()) break;
    String line = debugReplayFile.readStringUntil('\n');
    line.trim();
    if (line.length() < 8) continue; // ligne vide -- ignoree

    // Reutilise splitCsvLine() (definie plus haut pour circuits.csv) --
    // meme decoupage generique, format different.
    String fld[10];
    if (splitCsvLine(line, fld, 10) < 10) continue; // ligne malformee -- ignoree

    long ts = parseLocalTimeToMs(fld[0]); // colonne local_time
    if (debugReplayFirstTimestampMs < 0) debugReplayFirstTimestampMs = ts;
    long elapsed = ts - debugReplayFirstTimestampMs;
    if (elapsed < 0) elapsed += 24L * 3600L * 1000L; // passage minuit -- improbable pour une session, gratuit a couvrir

    if (elapsed > targetElapsedMs) {
      debugReplayFile.seek(posBeforeLine); // pas encore l'heure de cette ligne -- retraitee au prochain tick
      break;
    }

    double lat = atof(fld[2].c_str());
    double lng = atof(fld[3].c_str());
    float speedKmh = fld[4].toFloat();
    uint8_t fix = fld[5].toInt();
    uint8_t sats = fld[6].toInt();

    // Peuple liveData comme le ferait pollGps() en conditions reelles --
    // processGpsFix() et tout ce qu'elle appelle (logRow(),
    // getDisplayState()...) lisent exclusivement liveData, jamais de
    // parametres separes -- rejouer revient donc a "mentir" sur la
    // source des donnees, le reste du pipeline ne voit aucune difference.
    liveData.latitude = (int32_t)(lat * 1e7);
    liveData.longitude = (int32_t)(lng * 1e7);
    liveData.speedMmPerS = (uint32_t)(speedKmh / 3.6f * 1000.0f);
    liveData.fixStatus = fix;
    liveData.numSVs = sats;
    liveData.wgsAltitude = 0;

    float speedKnots = speedKmh / 1.852f;
    processGpsFix(lat, lng, 0.0f, speedKnots, (unsigned long)ts);
  }

  if (!debugReplayFile.available()) {
    debugReplayFile.close();
    debugReplayFile = LittleFS.open(DEBUG_REPLAY_FILE_PATH, "r");
    debugReplayFile.readStringUntil('\n'); // sauter l'en-tete a nouveau
    debugReplayFirstTimestampMs = -1;
    debugReplayStartedAtMillis = millis();
    Serial.println("Debogage : fin du fichier -- on reboucle depuis le debut.");
  }
}

static void toggleDebugReplay() {
  if (!debugReplayActive && gpsActive) {
    Serial.println("Debogage : un GPS reel repond sur ce chrono -- ce mode ne doit tourner que sans GPS branche (ESP de rechange). Debranche-le ou utilise un autre ESP.");
    return;
  }

  debugReplayActive = !debugReplayActive;
  if (debugReplayActive) {
    debugReplayFile = LittleFS.open(DEBUG_REPLAY_FILE_PATH, "r");
    if (!debugReplayFile) {
      Serial.printf("Debogage : %s introuvable -- envoye via 'pio run -t uploadfs' ?\n", DEBUG_REPLAY_FILE_PATH);
      debugReplayActive = false;
      return;
    }
    debugReplayFile.readStringUntil('\n'); // sauter la ligne d'en-tete
    debugReplayFirstTimestampMs = -1;
    debugReplayStartedAtMillis = millis();

    // Etat totalement vierge, comme un vrai boot -- sinon on testerait le
    // geofencing/detection avec un etat deja pollue par un essai
    // precedent sur ce meme firmware.
    manualOverrideActive = false;
    manualCourseIndex = -1;
    courseManager->reset();
    forcedLapAnything = false;
    geofenceCheckDone = false;
    lastLapCount = 0;
    pendingNewCircuitCapture = false;
    newCircuitAutoSaved = false;
    newCircuitCaptureArmed = false; // etat vierge -- pas de capture en cours au lancement du rejeu
    prevWaypointLat = 0; prevWaypointLng = 0;
    Serial.println("Debogage ACTIVE -- rejeu de /debug_replay.csv (x10) dans le vrai pipeline courseManager.");
  } else {
    if (debugReplayFile) debugReplayFile.close();
    Serial.println("Debogage DESACTIVE.");
  }
}

// ===================== Log CSV par session (LittleFS) =====================

static File logFile;
static bool loggingOk = false;
static char currentLogPath[40] = "";
static char lastLogPath[40] = "";
static unsigned long lastLogWrite = 0;
const unsigned long LOG_INTERVAL_MS = 100;

static void initFilesystem() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS: echec d'initialisation, log desactive.");
    return;
  }
  Serial.printf("LittleFS pret (%u / %u octets utilises)\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

// ===================== Stockage des logs GPS (LittleFS ou SD) =====================
//
// gpsLogFs/gpsLogsOnSd/migrateLittleFsLogsToSd() vivent desormais dans
// SdLogStorage.h/.cpp -- module partage tel quel avec la variante TFT
// (meme principe que GpsManager/WebServerManager, cf. README_FS.md).
//
// Bus SPI dedie a la SD -- pins custom (pas les pins FSPI par defaut de
// l'ESP32-S3), necessite un objet SPIClass explicite comme sur la
// variante TFT (a la difference que la, il n'y a rien a partager : cet
// OLED est en I2C, aucun bus SPI existant a reutiliser). Cet objet
// reste ici (pas dans SdLogStorage) car sa gestion differe
// structurellement selon la variante -- SdLogStorage se contente de
// recevoir un SPIClass& deja initialise.
SPIClass sdSPI(FSPI);

static void startNewGpsLog() {
  char compactDateTime[16];
  getLocalDateTimeCompact(compactDateTime, sizeof(compactDateTime));
  snprintf(currentLogPath, sizeof(currentLogPath), "/log_%s.csv", compactDateTime);

  logFile = gpsLogFs->open(currentLogPath, "w");
  if (!logFile) {
    Serial.printf("Stockage logs: impossible de creer %s\n", currentLogPath);
    loggingOk = false;
    return;
  }
  logFile.println("local_time,millis_boot,lat,lng,speed_kmh,fix,sats,laps,circuit,current_lap_ms");
  logFile.flush();
  loggingOk = true;
  snprintf(lastLogPath, sizeof(lastLogPath), "%s", currentLogPath);
  Serial.printf("Nouveau log de session : %s (%s)\n", currentLogPath, gpsLogsOnSd ? "SD" : "LittleFS");
}

static void stopGpsLog() {
  if (loggingOk) {
    logFile.flush();
    logFile.close();
    loggingOk = false;
    Serial.printf("Log de session ferme : %s\n", currentLogPath);
  }
}

template<typename Fn>
static void forEachGpsLogFile(Fn callback) {
  File root = gpsLogFs->open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.indexOf("/log_") >= 0 && name.endsWith(".csv")) {
      callback(name, (uint32_t)f.size());
    }
    f = root.openNextFile();
  }
}

static void logRow() {
  if (!loggingOk) return;
  char timeBuf[16];
  getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));

  double lat  = liveData.latitude / 1e7;
  double lng  = liveData.longitude / 1e7;
  float speedKmh = liveData.speedMmPerS * 3.6f / 1000.0f;

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount; float totalDistanceM;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount, totalDistanceM);

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

// ===================== Carnet de session (resume par tour, leger) =====================

static const char* SESSION_LOG_PATH = "/sessions.csv";
static File sessionLogFile;
static bool sessionLoggingOk = false;

static void initSessionLog() {
  bool isNewFile = !LittleFS.exists(SESSION_LOG_PATH) || LittleFS.open(SESSION_LOG_PATH, "r").size() == 0;
  sessionLogFile = LittleFS.open(SESSION_LOG_PATH, "a");
  if (!sessionLogFile) {
    Serial.println("LittleFS: impossible d'ouvrir le carnet de session.");
    return;
  }
  if (isNewFile) {
    sessionLogFile.println("date,local_time,lap_number,lap_time,best_lap_time,circuit");
  }
  sessionLogFile.flush();
  sessionLoggingOk = true;
  Serial.printf("Carnet de session actif : %s (taille actuelle %u octets)\n",
                SESSION_LOG_PATH, (unsigned)LittleFS.open(SESSION_LOG_PATH, "r").size());
}

static void logLapToSessionFile(int lapNumber, unsigned long lapTimeMs, unsigned long bestLapTimeMs) {
  if (!sessionLoggingOk) return;
  char dateBuf[12], timeBuf[12];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));

  sessionLogFile.printf("%s,%s,%d,%s,%s,%s\n", dateBuf, timeBuf, lapNumber,
                         formatLapTime(lapTimeMs).c_str(), formatLapTime(bestLapTimeMs).c_str(),
                         getActiveCourseNameForDisplay());
  sessionLogFile.flush();
}

// ===================== Flush des logs pour le WiFi =====================
//
// Plus besoin de couper/relancer quoi que ce soit autour du cycle WiFi :
// le GPS est cable (UART), il continue de tourner normalement pendant
// que le point d'acces est actif -- contrairement au BLE, il n'y a plus
// de contention radio a arbitrer entre GPS et WiFi. Seul le flush des
// logs avant telechargement reste necessaire.

static void wifiCallback_flushLogs() {
  if (loggingOk) logFile.flush();
  if (sessionLoggingOk) sessionLogFile.flush();
}

// ===================== Ecran et menu (OLED) =====================
//
// Trois ecrans : statut (par defaut, juste tour precedent/meilleur tour --
// pas de chrono live qui defile), info (BLE/Fix/Sat/circuit, BACK court),
// et menu (selection manuelle du circuit, BACK long).

enum ScreenState {
  SCREEN_STATUS,       // ecran principal -- tour/best, par defaut
  SCREEN_MAIN_MENU,    // menu principal : Circuit / Connexion / Session / Reglages
  SCREEN_CIRCUIT_MENU, // sous-menu : Auto + 8 circuits
  SCREEN_CONNEXION,    // BLE/Fix/Sat/circuit (ex SCREEN_INFO)
  SCREEN_SESSION_LIST, // liste des sessions du carnet, la plus recente en premier
  SCREEN_SESSION_LAPS, // detail des tours d'une session choisie
  SCREEN_SETTINGS,     // vide pour l'instant, juste un emplacement reserve
  SCREEN_WIFI
};
static ScreenState screenState = SCREEN_STATUS;
static int menuSelection = 0; // reutilise pour chaque sous-menu (le sens depend de screenState)

// ----- Menu principal -----

const int MAIN_MENU_COUNT = 4;
static const char* mainMenuEntryName(int idx) {
  switch (idx) {
    case 0: return "Circuit";
    case 1: return "Connexion";
    case 2: return "Session";
    default: return "Reglages";
  }
}

// ----- Sous-menu Circuit (Auto + 8 circuits uniquement -- WiFi et BT deplaces dans Reglages) -----

static const char* circuitMenuEntryName(int idx) {
  if (idx == 0) return "Auto (detection)";
  return myTracks.courses[idx - 1].name;
}

// Affiche une liste verticale generique (4 lignes visibles, defilement
// autour de la selection) -- factorise le rendu commun a tous les menus
// a une seule colonne de ce firmware.
static void drawSelectableList(int total, int selected, const char* (*nameForIndex)(int)) {
  display.setFont(u8g2_font_6x10_tf);
  int firstVisible = selected - 1;
  if (firstVisible < 0) firstVisible = 0;
  if (firstVisible > total - 4) firstVisible = total - 4;
  if (firstVisible < 0) firstVisible = 0;

  for (int row = 0; row < 4 && (firstVisible + row) < total; row++) {
    int idx = firstVisible + row;
    int y = 12 + row * 13;
    if (idx == selected) {
      display.drawBox(0, y - 10, 128, 13);
      display.setDrawColor(0);
      display.drawStr(2, y, nameForIndex(idx));
      display.setDrawColor(1);
    } else {
      display.drawStr(2, y, nameForIndex(idx));
    }
  }
}

static void drawMainMenuScreen() {
  display.clearBuffer();
  drawSelectableList(MAIN_MENU_COUNT, menuSelection, mainMenuEntryName);
  display.sendBuffer();
}

static void drawCircuitMenuScreen() {
  display.clearBuffer();
  drawSelectableList(myTracks.courseCount + 1, menuSelection, circuitMenuEntryName);
  display.sendBuffer();
}

static void drawStatusScreen() {
  display.clearBuffer();

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount;
  float totalDistanceM;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount, totalDistanceM);
  bool showLiveTimer = recordingEnabled; // meme logique que le point REC -- visible seulement quand ca enregistre vraiment

  // Ligne du haut : petit chrono qui tourne en continu pendant le tour en
  // cours, remis a zero au franchissement de la ligne. Volontairement en
  // petite police (discret) pour ne pas reproduire l'effet "chrono qui
  // defile" d'un compteur principal -- juste un repere, pas l'info
  // principale.
  //
  // Cette ligne reste vide tant que l'enregistrement n'a pas demarre
  // (rien a chronometrer) -- on en profite pour y rappeler "PRESS REC"
  // des qu'un circuit CONNU est pret (meme condition que l'affichage du
  // nom du circuit juste en dessous), plutot que de remplacer ce nom :
  // les deux infos sont utiles en meme temps en sortant des stands
  // (lequel + qu'il reste a appuyer). L'enregistrement etant desormais
  // 100% manuel (PUSH, plus d'auto-demarrage), cet oubli est possible.
  bool circuitReadyForRecording = gpsActive &&
                                   (manualOverrideActive || detectionEffectivelyComplete()) &&
                                   !lapAnythingEffective();
  display.setFont(u8g2_font_6x10_tf);
  if (!recordingEnabled && circuitReadyForRecording) {
    display.drawStr(0, 10, "PRESS REC");
  } else if (showLiveTimer) {
    display.drawStr(0, 10, formatLapTime(currentLapMs).c_str());
  }

  if (lapsCount > 0) {
    display.setFont(u8g2_font_logisoso24_tn); // police "Logisoso" 24px, concue pour les affichages numeriques type chrono -- plus grande que la precedente (10x20)
    unsigned long lastLapMs = getLastFinishedLapMs();
    display.drawStr(0, 43, formatLapTime(lastLapMs).c_str());
  } else {
    display.setFont(u8g2_font_7x14B_tf); // police plus petite ici -- ces libelles deborderaient en 10x20
    // Avant, "En attente..." s'affichait dans tous les cas ici (pas de
    // fix GPS, detection en cours, mode proximite actif...) -- aucune
    // distinction visible, donc aucun moyen de savoir si le systeme a
    // reagi ou non a un nouveau circuit (source de confusion constatee
    // lors d'un test terrain). On distingue maintenant les etats.
    if (!gpsActive) {
      display.drawStr(0, 43, "GPS ?");
    } else if (!manualOverrideActive && !detectionEffectivelyComplete()) {
      display.drawStr(0, 43, "Detection...");
    } else if (!manualOverrideActive && lapAnythingEffective()) {
      display.drawStr(0, 43, "Nv. circuit"); // mode proximite actif -- circuit inconnu detecte, en attente du 1er tour
    } else {
      // Circuit CONNU deja selectionne (geofencing, menu, ou detection
      // normale aboutie) mais aucun tour encore boucle -- afficher son nom
      // leve l'ambiguite plutot qu'un "En attente..." generique. Constat
      // terrain : rien ne distinguait "circuit correctement reconnu, prêt"
      // de "en train de chercher", alors que le point REC (deja affiche,
      // coin haut droit) confirme que l'enregistrement a bien demarre --
      // il manquait juste de savoir SUR QUOI.
      char nameBuf[19];
      snprintf(nameBuf, sizeof(nameBuf), "%.18s", getActiveCourseNameForDisplay());
      display.drawStr(0, 43, nameBuf);
    }
  }

  display.setFont(u8g2_font_7x14B_tf);
  char bestBuf[24];
  if (hasBest) snprintf(bestBuf, sizeof(bestBuf), "B %s", formatLapTime(bestLapMs).c_str());
  else snprintf(bestBuf, sizeof(bestBuf), "B --:--.---");
  display.drawStr(0, 62, bestBuf);

  // Indicateur REC -- petit point fixe (pas de clignotement), discret a
  // dessein pour ne pas attirer l'oeil pendant le pilotage. Absent si
  // l'enregistrement n'est pas actif.
  if (recordingEnabled) {
    display.drawDisc(124, 4, 2); // petit point plein, coin haut droit
  }

  display.sendBuffer();
}

// Ecran connexion -- BLE/Fix/Sat/circuit actif (ex "ecran info").
static void drawConnexionScreen() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);

  char line1[24];
  snprintf(line1, sizeof(line1), "%s", gpsActive ? "GPS OK" : "GPS -- (recherche)");
  display.drawStr(0, 12, line1);

  char line2[24];
  snprintf(line2, sizeof(line2), "Fix:%u Sat:%u", liveData.fixStatus, liveData.numSVs);
  display.drawStr(0, 26, line2);

  char line3[24];
  snprintf(line3, sizeof(line3), "%.20s", getActiveCourseNameForDisplay());
  display.drawStr(0, 40, line3);

  display.drawStr(0, 60, "BACK : retour");

  display.sendBuffer();
}

// Ecran WiFi -- affiche le SSID et l'IP pendant que le telechargement est
// actif, pour pouvoir s'y connecter sans avoir besoin du moniteur Serial
// (utile une fois le boitier monte sur la moto, sans cable a portee de main).
static void drawWifiScreen() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 12, "WiFi actif");
  char line2[24];
  snprintf(line2, sizeof(line2), "Reseau: %s", webServerManager.getSsid().c_str());
  display.drawStr(0, 26, line2);
  char line3[24];
  snprintf(line3, sizeof(line3), "IP: %s", webServerManager.getIp().c_str());
  display.drawStr(0, 40, line3);
  display.drawStr(0, 60, "BACK : couper le WiFi");

  display.sendBuffer();
}

// Ecran reglages -- emplacement reserve, vide pour l'instant.
// ----- Sous-menu Reglages (WiFi telechargement uniquement -- plus de
// toggle BT desormais que le GPS est cable et tourne en continu) -----

const int SETTINGS_MENU_COUNT = 1;
static const char* settingsMenuEntryName(int idx) {
  return "WiFi telechargement";
}

static void drawSettingsScreen() {
  display.clearBuffer();
  drawSelectableList(SETTINGS_MENU_COUNT, menuSelection, settingsMenuEntryName);
  display.sendBuffer();
}

// ===================== Consultation des sessions enregistrees =====================
//
// Parcourt /sessions.csv et regroupe les lignes par session (delimitees
// par les marqueurs "# session demarree/arretee" deja ecrits par
// toggleRecording()). Garde tout en RAM le temps de la consultation --
// le fichier reste petit (quelques Ko meme pour beaucoup de tours), donc
// pas de souci de memoire a charger une session entiere.

struct SessionLapEntry {
  String lapNumber;
  String lapTime;
  String bestLapTime;
  String circuit;
};

struct SessionSummary {
  String date;
  String startTime;
  std::vector<SessionLapEntry> laps;
};

static std::vector<SessionSummary> loadedSessions; // la plus recente en dernier dans le fichier -> on inversera a l'affichage
static int sessionListSelection = 0;
static int sessionLapSelection = 0;
static int viewingSessionIndex = -1;

static void parseSessionsFile() {
  loadedSessions.clear();
  File f = LittleFS.open(SESSION_LOG_PATH, "r");
  if (!f) return;

  SessionSummary* current = nullptr;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("date,")) continue; // en-tete CSV

    if (line.startsWith("# session demarree")) {
      loadedSessions.push_back(SessionSummary());
      current = &loadedSessions.back();
      // Deux formats possibles selon qui a ecrit le marqueur :
      //  - enregistrement reel (toggleRecording()) : "AAAA-MM-JJ HH:MM:SS"
      //  - import RaceChrono (handleImportRaceChronoUpload()) : "AAAAMMJJ_HHMMSS"
      //    (meme forme compacte que le nom de fichier log_*.csv)
      // L'ancien code ne cherchait qu'un espace separateur -- absent du
      // format compact, ce qui laissait date/startTime vides (session
      // visible dans la liste OLED mais ligne blanche). On normalise donc
      // d'abord vers des chiffres purs (meme principe que
      // stripSeparators()/prettyDate()/prettyTime() cote WebServerManager,
      // qui doit deja jongler avec les deux formats pour recoller carnet
      // et logs par date), puis on reformate pour l'affichage.
      String rest = line.substring(line.indexOf("demarree") + 9);
      String digits;
      for (size_t i = 0; i < rest.length(); i++) {
        char c = rest[i];
        if (c >= '0' && c <= '9') digits += c;
      }
      if (digits.length() >= 14) {
        current->date = digits.substring(0, 4) + "-" + digits.substring(4, 6) + "-" + digits.substring(6, 8);
        current->startTime = digits.substring(8, 10) + ":" + digits.substring(10, 12) + ":" + digits.substring(12, 14);
      }
      continue;
    }
    if (line.startsWith("# session arretee")) {
      current = nullptr; // ferme la session courante -- une ligne perdue hors session est ignoree
      continue;
    }
    if (!current) continue; // ligne de tour hors de toute session connue -- ignoree (ne devrait pas arriver)

    // Format : date,heure,numero_tour,temps_tour,meilleur_temps,circuit
    SessionLapEntry entry;
    int idx = 0, start = 0;
    String fields[6];
    for (int i = 0; i < (int)line.length() && idx < 6; i++) {
      if (line[i] == ',' || i == (int)line.length() - 1) {
        int end = (line[i] == ',') ? i : i + 1;
        fields[idx++] = line.substring(start, end);
        start = i + 1;
      }
    }
    if (idx >= 6) {
      entry.lapNumber = fields[2];
      entry.lapTime = fields[3];
      entry.bestLapTime = fields[4];
      entry.circuit = fields[5];
      current->laps.push_back(entry);
    }
  }
  f.close();
}

static int sessionCount() { return (int)loadedSessions.size(); }

// Les sessions sont stockees dans l'ordre du fichier (plus ancienne en
// premier) -- on les affiche en ordre inverse (plus recente en haut).
static SessionSummary& sessionAt(int displayIndex) {
  return loadedSessions[loadedSessions.size() - 1 - displayIndex];
}

static const char* sessionListEntryName(int idx) {
  static char buf[24];
  SessionSummary& s = sessionAt(idx);
  snprintf(buf, sizeof(buf), "%s %s", s.date.c_str(), s.startTime.c_str());
  return buf;
}

static void drawSessionListScreen() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  if (sessionCount() == 0) {
    display.drawStr(0, 20, "Aucune session");
    display.drawStr(0, 36, "enregistree.");
    display.drawStr(0, 60, "BACK : retour");
  } else {
    drawSelectableList(sessionCount(), sessionListSelection, sessionListEntryName);
  }
  display.sendBuffer();
}

static void drawSessionLapsScreen() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);

  if (viewingSessionIndex < 0 || viewingSessionIndex >= sessionCount()) {
    display.drawStr(0, 20, "Session introuvable");
    display.sendBuffer();
    return;
  }
  SessionSummary& s = sessionAt(viewingSessionIndex);
  if (s.laps.empty()) {
    display.drawStr(0, 12, (s.date + " " + s.startTime).c_str());
    display.drawStr(0, 30, "Aucun tour enregistre");
    display.drawStr(0, 60, "BACK : retour");
    display.sendBuffer();
    return;
  }

  if (sessionLapSelection >= (int)s.laps.size()) sessionLapSelection = s.laps.size() - 1;
  if (sessionLapSelection < 0) sessionLapSelection = 0;
  SessionLapEntry& lap = s.laps[sessionLapSelection];

  // Meilleur temps REEL de la session (min sur tous ses tours) -- pas la
  // colonne "meilleur_temps" de lap (best-so-far au moment de CE tour,
  // cf. commentaire sur lapTimeStrToMs()). Recalcule a chaque affichage --
  // une session ne depasse jamais quelques dizaines de tours, cout negligeable.
  long overallBestMs = -1;
  for (const SessionLapEntry& l : s.laps) {
    long ms = lapTimeStrToMs(l.lapTime);
    if (ms >= 0 && (overallBestMs < 0 || ms < overallBestMs)) overallBestMs = ms;
  }
  String overallBestStr = (overallBestMs >= 0) ? formatLapTime((unsigned long)overallBestMs) : lap.bestLapTime;

  char header[24];
  snprintf(header, sizeof(header), "%s (%d/%d)", s.startTime.c_str(), sessionLapSelection + 1, (int)s.laps.size());
  display.drawStr(0, 10, header);

  display.setFont(u8g2_font_7x14B_tf);
  display.drawStr(0, 30, ("T" + lap.lapNumber + " " + lap.lapTime).c_str());
  display.drawStr(0, 48, ("B " + overallBestStr).c_str());

  display.setFont(u8g2_font_6x10_tf);
  // Baseline a 61 (pas 63) : la police u8g2_font_6x10_tf a un jambage
  // de 2px sous la baseline (lettres g/j/p/q/y) -- a 63 (derniere ligne
  // visible sur un ecran 64px, lignes 0-63), ce jambage depassait de
  // l'ecran et se retrouvait coupe (ex. bas du "g" de "PigTeam_track").
  // T<tour>/B<meilleur> remontes du meme delta (32->30, 52->50) pour
  // garder un espacement coherent entre les 3 lignes de l'ecran.
  display.drawStr(0, 61, lap.circuit.c_str());

  display.sendBuffer();
}

// ===================== Commandes Serial =====================

static void handleSerialCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c == 'd') {
    if (loggingOk) logFile.flush();
    if (strlen(lastLogPath) == 0) {
      Serial.println("Aucune session enregistree encore.");
    } else {
      Serial.printf("=== DEBUT DUMP %s ===\n", lastLogPath);
      File f = gpsLogFs->open(lastLogPath, "r");
      while (f && f.available()) Serial.write(f.read());
      f.close();
      Serial.printf("=== FIN DUMP %s ===\n", lastLogPath);
    }
  } else if (c == 'l') {
    Serial.printf("=== Fichiers de log GPS (%s) ===\n", gpsLogsOnSd ? "SD" : "LittleFS");
    int count = 0;
    forEachGpsLogFile([&](const String& name, uint32_t size) {
      Serial.printf("  %s (%u octets)\n", name.c_str(), (unsigned)size);
      count++;
    });
    if (count == 0) Serial.println("  (aucun)");
  } else if (c == 'c') {
    stopGpsLog();
    String toDelete[20];
    int count = 0;
    forEachGpsLogFile([&](const String& name, uint32_t size) {
      if (count < 20) toDelete[count++] = name;
    });
    for (int i = 0; i < count; i++) gpsLogFs->remove(toDelete[i]);
    lastLogPath[0] = '\0';
    Serial.printf("%d fichier(s) de log GPS efface(s).\n", count);
  } else if (c == 's') {
    sessionLogFile.flush();
    Serial.println("=== DEBUT CARNET DE SESSION ===");
    File f = LittleFS.open(SESSION_LOG_PATH, "r");
    while (f && f.available()) Serial.write(f.read());
    f.close();
    Serial.println("=== FIN CARNET DE SESSION ===");
  } else if (c == 'x') {
    sessionLogFile.close();
    LittleFS.remove(SESSION_LOG_PATH);
    Serial.println("Carnet de session efface.");
    initSessionLog();
  } else if (c == 'w') {
    if (webServerManager.isActive()) webServerManager.stopDownloadMode();
    else webServerManager.startDownloadMode();
  } else if (c == 'g') {
    toggleDebugReplay();
  }
}

// ===================== Demarrage/arret de l'enregistrement =====================
//
// Demarrage AUTOMATIQUE des qu'un circuit est actif (auto-detecte ou
// force depuis le menu) -- comme RaceChrono. Arret toujours MANUEL via
// CONFIRM (jamais automatique) : le pilote garde la main sur le moment
// ou il considere sa sortie terminee, pas de coupure surprise pendant
// qu'il roule encore.

static void startRecording() {
  if (recordingEnabled) return;
  recordingEnabled = true;
  char dateBuf[12], timeBuf[12];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  Serial.println("Enregistrement DEMARRE.");
  startNewGpsLog();
  if (sessionLoggingOk) sessionLogFile.printf("# session demarree %s %s\n", dateBuf, timeBuf);
}

static void stopRecording() {
  if (!recordingEnabled) return;
  recordingEnabled = false;
  char dateBuf[12], timeBuf[12];
  getLocalDateTime(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));
  Serial.println("Enregistrement ARRETE.");
  stopGpsLog();
  if (sessionLoggingOk) { sessionLogFile.printf("# session arretee %s %s\n", dateBuf, timeBuf); sessionLogFile.flush(); }
}

// PUSH demarre/arrete l'enregistrement -- desormais le SEUL declencheur,
// plus d'auto-demarrage a la detection d'un circuit (cf. discussion plus
// haut, dans loop()).
static void toggleRecording() {
  if (recordingEnabled) stopRecording();
  else startRecording();
}

// ===================== Callbacks supplementaires pour WebServerManager =====================
//
// Pages "Statut" et "Enregistrement" du serveur web -- meme logique de
// callbacks que pour le WiFi plus haut (le module ne connait rien du
// detail interne du firmware).

// migrateLittleFsLogsToSd() vit desormais dans SdLogStorage.cpp (module
// partage avec la variante TFT) -- cf. SdLogStorage.h.

static WebServerStatusInfo webServerCallback_getStatus() {
  WebServerStatusInfo s;
  s.bleConnected = gpsActive; // champ conserve tel quel (cote WebServerManager) -- represente desormais "GPS actif"
  s.fixStatus = liveData.fixStatus;
  s.numSats = liveData.numSVs;
  s.circuitName = getActiveCourseNameForDisplay();
  // Position live -- utilisee par la page /circuits pour le bouton
  // "capturer la position GPS actuelle" (le GPS tourne aussi pendant le
  // WiFi, plus de coexistence radio a arbitrer, cf. notes GPS cable).
  if (s.fixStatus >= 2) {
    s.hasGpsFix = true;
    s.latitude = liveData.latitude / 1e7;
    s.longitude = liveData.longitude / 1e7;
  }
  s.recordingEnabled = recordingEnabled;

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount; float totalDistanceM;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount, totalDistanceM);
  s.lapsCount = lapsCount;
  if (lapsCount > 0) s.lastLapTime = formatLapTime(getLastFinishedLapMs());
  if (hasBest) s.bestLapTime = formatLapTime(bestLapMs);

  // Barre de stockage SD sur la page Sessions -- seulement si gpsLogFs
  // pointe reellement sur la SD (pas en repli LittleFS, deja couverte
  // par sa propre barre cote WebServerManager). sdUsedBytes()/
  // sdTotalBytes() (SdLogStorage.h) encapsulent la lecture SDFS-specifique
  // (pas dans l'interface commune fs::FS) et renvoient 0 sans risque si
  // la SD n'est pas active.
  if (gpsLogFs == &SD) {
    s.hasSeparateLogsFs = true;
    s.logsFsLabel = "SD (logs GPS detailles)";
    s.logsFsUsedBytes = sdUsedBytes();
    s.logsFsTotalBytes = sdTotalBytes();
  }

  return s;
}

// Pas de callback "toggleRecording" pour le serveur web -- absent depuis
// l'origine du projet (a l'epoque, demarrer un enregistrement pendant le
// WiFi n'aurait rien ecrit puisque le BLE etait coupe). Ce n'est plus le
// cas avec le GPS cable (il tourne aussi pendant le WiFi) -- ajouter ce
// callback deviendrait possible si le besoin se presente, mais pas fait
// ici pour rester dans le perimetre de la migration GPS.

// ===================== Setup / Loop =====================

// ===================== Splash de demarrage (logo PigTeam) =====================
//
// Version monochrome du logo couleur de la Pendule Paddock (logo_pigteam.h,
// RGB565 64x64) -- convertie en bitmap 1 bit/pixel (cf. logo_pigteam_xbm.h)
// pour etre affichee sur cet OLED monochrome via drawXBM(). Affiche
// brievement au tout debut de setup(), avant le reste de l'initialisation.

static void drawBootSplash() {
  display.clearBuffer();
  // Centre horizontalement (ecran 128px large, logo 64px) -- pleine hauteur (64px = 64px ecran)
  display.drawXBM((128 - LOGO_PIGTEAM_XBM_WIDTH) / 2, 0, LOGO_PIGTEAM_XBM_WIDTH, LOGO_PIGTEAM_XBM_HEIGHT, logo_pigteam_xbm);
  display.sendBuffer();
  delay(1800); // assez long pour etre vu, assez court pour ne pas agacer a chaque demarrage
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== Chrono GPS moto piste (OLED+encodeur) -- demarrage ===");

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin();
  drawBootSplash();

  pinMode(CONFIRM_BUTTON, INPUT_PULLUP);
  pinMode(BACK_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CONFIRM_BUTTON), confirmButtonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BACK_BUTTON), backButtonISR, CHANGE);

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PUSH), pushButtonISR, CHANGE); // apres begin() -- le pinMode INPUT_PULLUP du pin PUSH est pose par la lib juste au-dessus
  rotaryEncoder.setBoundaries(0, MAIN_MENU_COUNT - 1, true); // valeur de depart -- reglee a nouveau a chaque entree de sous-menu (cf. loop())
  rotaryEncoder.setAcceleration(0);

  initFilesystem();
  initSessionLog();

  // ----- SD : branchee sur le stockage des logs GPS detailles -----
  // gpsLogFs/gpsLogsOnSd vivent dans SdLogStorage.h (module partage avec
  // la variante TFT) -- gpsLogFs vaut &LittleFS par defaut, repli
  // automatique si initSdLogStorage() echoue (carte absente, mal cablee,
  // en panne) -- la SD reste facultative, comme prevu des l'origine dans
  // WebServerManager.h. main.cpp reste responsable du bus SPI lui-meme
  // (dedie ici, contrairement au TFT qui partage avec l'ecran) --
  // SdLogStorage ne fait jamais spi.begin(). Cablage (CS/MOSI/MISO/SCLK)
  // deja valide au banc via le sketch isole sd-wiring-test-oled --
  // inutile de dupliquer ici ce test bas niveau.
  Serial.println("Test SD...");
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!initSdLogStorage(SD_CS, sdSPI)) {
    Serial.println("SD : absente ou en panne -- repli sur LittleFS pour les logs GPS detailles.");
  } else {
    uint64_t sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("SD ok -- carte detectee (~%llu Mo) -- logs GPS detailles bascules dessus.\n", sizeMb);
    migrateLittleFsLogsToSd(); // sessions restees sur LittleFS avant ce demarrage (enregistrees ou importees pre-SD) -- deplacees ici pour redevenir visibles cote WebServerManager et liberer la place qu'elles occupaient
  }

  // Circuits charges depuis LittleFS *avant* de construire courseManager --
  // myTracks doit etre rempli avant le constructeur de CourseManager, qui
  // copie sa config une seule fois (cf. commentaire plus haut, section
  // "Chrono multi-circuits").
  loadActiveCircuitsIntoTracks();
  courseManager = new CourseManager(myTracks, 7.0, &Serial); // &Serial = logs de detection verbeux, utile pour les premiers essais ; passer NULL pour une version silencieuse

  webServerManager.begin("ChronoMoto", SESSION_LOG_PATH, CIRCUITS_FILE_PATH, *gpsLogFs, nullptr, nullptr, wifiCallback_flushLogs,
                          webServerCallback_getStatus);
  Serial.println("Log GPS par session : 'l' (lister) / 'd' (recuperer la derniere) / 'c' (tout effacer). Carnet de session : 's' (recuperer) / 'x' (effacer).");
  Serial.println("PUSH (clic encodeur) : demarre/arrete l'enregistrement. CONFIRM : force le mode proximite (Nouveau circuit). BACK : menu de selection de circuit.");
  Serial.println("'g' : mode debogage (rejeu de /debug_replay.csv dans le vrai pipeline -- ESP sans GPS uniquement).");
 
  initGps();
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
// que generate_line.py, reimplementees ici pour la capture embarquee (pas
// de dependance externe, juste <math.h> deja disponible via Arduino.h).
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
// Declenchee UNIQUEMENT par CONFIRM (cf.
// forceLapAnythingManually(), qui arme newCircuitCaptureArmed) --
// volontairement pas sur un repli automatique en proximite (3 rejets
// natifs de la lib, ou notre contournement 4500m/tour deja boucle). Le
// mode proximite/comptage de tours fonctionne pareil dans tous les cas ;
// seule l'ECRITURE d'un nouveau circuit dans circuits.csv est reservee au
// geste intentionnel, pour eviter qu'une balade improvisee la ou rien
// n'est reconnu ne finisse par polluer la liste de circuits toute seule.
//
// Une fois armee, une ligne A/B est deduite automatiquement au premier
// tour valide (perpendiculaire au cap de la route au moment ou le
// waypoint a ete pose -- meme principe que generate_line.py, mais calcule
// ici en direct depuis l'historique GPS embarque) et ecrite directement
// dans circuits.csv -- active=0, non protege, nom generique
// ("Nouveau_AAAAMMJJ_HHMMSS"). A finaliser ensuite sur /circuits (nom
// definitif, activation) -- toujours aucune saisie de texte depuis le
// chrono, cf. discussion sur l'ergonomie encodeur vs page web.
//
// Capture en DEUX temps, volontairement decouples :
//  1. Des que le waypoint interne est pose (position reelle, on est encore
//     physiquement dessus) -- capture du cap et calcul de la ligne A/B.
//     Attendre le tour valide pour ca donnerait un cap totalement faux
//     (calcule a un endroit different, potentiellement loin de la ligne).
//  2. Ecriture effective dans circuits.csv seulement au premier tour
//     VALIDE (getRaceStarted()) ET seulement si on est toujours en mode
//     proximite a ce moment (lapAnythingEffective()) -- si entre-temps un
//     vrai circuit connu a fini par etre detecte, on abandonne la capture
//     silencieusement plutot que de creer une entree parasite.
static const double NEW_CIRCUIT_LINE_OFFSET_M = 4.0; // meme reglage par defaut que generate_line.py (~8m de large)

static void appendAutoCircuitToFile(const char* name, float lengthFt) {
  File f = LittleFS.open(CIRCUITS_FILE_PATH, "a");
  if (!f) { Serial.println("Capture circuit : impossible d'ouvrir circuits.csv en ecriture."); return; }
  // 18 champs, meme format que le reste du fichier (cf. CIRCUITS_CSV_HEADER)
  // -- active=0, pas de secteurs, locked=0 (facilement supprimable si le
  // resultat ne convient pas).
  f.printf("0,%s,%.1f,%.7f,%.7f,%.7f,%.7f,0,0,0,0,0,0,0,0,0,0,0\n",
           name, lengthFt, pendingSaLat, pendingSaLng, pendingSbLat, pendingSbLng);
  f.close();
  Serial.printf("Nouveau circuit capture et sauvegarde (inactif) : %s -- a finaliser sur /circuits.\n", name);
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

  // Premier tour valide, et toujours en mode proximite a cet instant (pas
  // un vrai circuit connu entre-temps detecte) -- on committe dans le fichier.
  if (pendingNewCircuitCapture && t->getRaceStarted() && lapAnythingEffective()) {
    char compactDateTime[16];
    getLocalDateTimeCompact(compactDateTime, sizeof(compactDateTime));
    char name[32];
    snprintf(name, sizeof(name), "Nouveau_%s", compactDateTime);

    float lengthFt = t->getLastLapDistance() * 3.28084f; // getLastLapDistance() en metres -- meme conversion que partout ailleurs dans le fichier
    appendAutoCircuitToFile(name, lengthFt);

    newCircuitAutoSaved = true;
    pendingNewCircuitCapture = false;
  }
}

// Traite UN point GPS a travers tout le pipeline de production (geofencing,
// detection/contournement, demarrage auto de l'enregistrement, calcul de
// tour, log CSV) -- extrait de loop() pour etre appelable aussi bien avec
// un vrai fix GPS (timeMs = millis() reel) qu'avec un point rejoue depuis
// un fichier en mode debogage (timeMs = horodatage simule, cf.
// debugReplayTick() plus bas). Lit/ecrit liveData comme le ferait
// pollGps() -- l'appelant doit l'avoir peuplee avant d'appeler cette
// fonction (fixStatus au minimum, pour le garde ci-dessous).
static void processGpsFix(double lat, double lng, float altM, float speedKnots, unsigned long timeMs) {
  if (liveData.fixStatus < 2) return;

  pushGpsHistory(lat, lng, timeMs); // alimente l'historique court terme, utilise par checkAutoCircuitCapture() plus bas

  // Geofencing avant toute chose -- s'il active un circuit (cf.
  // checkCircuitGeofence()), manualOverrideActive devient vrai et le point
  // courant est traite tout de suite par le bon timer juste en dessous,
  // sans attendre le prochain point.
  if (!geofenceCheckDone && !manualOverrideActive) {
    checkCircuitGeofence(lat, lng);
  }

  if (manualOverrideActive) {
    manualTimer.updateCurrentTime(timeMs);
    manualTimer.loop(lat, lng, altM, speedKnots);
  } else {
    static bool detectionAnnounced = false;
    courseManager->updateCurrentTime(timeMs);
    courseManager->loop(lat, lng, altM, speedKnots);
    checkAutoCircuitCapture(timeMs); // cf. section "Capture automatique de nouveau circuit" plus haut

    // Contournement du fallback natif de la lib (cf. commentaire pres
    // de DETECTION_FALLBACK_DISTANCE_M) -- le chrono proximite tourne
    // deja en tache de fond via courseManager->loop() ci-dessus, on
    // se contente de regarder sa distance parcourue OU s'il a deja
    // boucle un tour tout seul pour decider nous-memes de la bascule,
    // sans dependre du compteur de rejets interne a la lib.
    //
    // Le declencheur "a deja boucle un tour" (getLaps() > 0) est
    // apparu necessaire apres un test terrain sur une petite boucle
    // de quartier (~1.8km/tour, cf. historique Git) : le seuil de
    // 4500m est calibre sur la longueur du plus long circuit connu
    // (Le Mans Bugatti), largement trop genereux pour une boucle
    // courte -- la session s'est terminee avant d'atteindre 4500m
    // parcourus, malgre un tour deja compte en interne par le chrono
    // proximite (visible seulement apres coup, jamais affiche/logge
    // puisque lapAnythingEffective() etait reste faux). Un vrai tour
    // complete est une preuve directe qu'on est bien en mode
    // proximite -- pas besoin d'attendre une distance arbitraire en
    // plus dans ce cas.
    if (!forcedLapAnything && !courseManager->isDetectionComplete() &&
        (courseManager->getLapAnythingTimer()->getTotalDistanceTraveled() >= DETECTION_FALLBACK_DISTANCE_M ||
         courseManager->getLapAnythingTimer()->getLaps() > 0)) {
      forcedLapAnything = true;
      Serial.println("Detection auto sans resultat, mode proximite deja boucle ou distance de secours atteinte -- bascule forcee.");
    }

    if (!detectionAnnounced && courseManager->isDetectionComplete()) {
      detectionAnnounced = true;
      if (courseManager->isLapAnythingActive()) {
        Serial.println("Aucun des 8 circuits connus ne correspond -- mode proximite active.");
      } else {
        Serial.printf("Circuit detecte : %s\n", courseManager->getActiveCourseName());
        courseManager->pruneInactiveCourses();
      }
    }
  }

  // Plus de demarrage automatique de l'enregistrement a la detection d'un
  // circuit -- c'etait la source des sessions "polluantes" a chaque
  // allumage pres d'un circuit connu (geofencing), meme sans jamais
  // rouler (cf. discussion). L'enregistrement est desormais TOUJOURS
  // declenche manuellement (PUSH/clic encodeur -> toggleRecording()),
  // aussi bien au demarrage qu'a l'arret -- un allumage de la journee ne
  // cree plus de session tant qu'on n'a pas explicitement appuye. La
  // detection de circuit elle-meme n'est pas affectee : elle continue de
  // tourner en tache de fond (necessaire pour savoir sur quel circuit on
  // chronometre une fois l'enregistrement lance), seul le declenchement
  // AUTOMATIQUE de l'enregistrement a ete retire.

  unsigned long currentLapMs, bestLapMs; bool hasBest; int lapsCount; float totalDistanceM;
  getDisplayState(currentLapMs, bestLapMs, hasBest, lapsCount, totalDistanceM);

  if (lapsCount > lastLapCount) {
    unsigned long finishedLapMs = getLastFinishedLapMs();
    Serial.printf("Tour %d termine : %s  (meilleur : %s)\n",
                  lapsCount, formatLapTime(finishedLapMs).c_str(),
                  formatLapTime(bestLapMs).c_str());
    if (recordingEnabled) {
      logLapToSessionFile(lapsCount, finishedLapMs, bestLapMs);
    }
    lastLapCount = lapsCount;
  }

  if (recordingEnabled && timeMs - lastLogWrite >= LOG_INTERVAL_MS) {
    lastLogWrite = timeMs;
    logRow();
  }
}

// Etat GPS periodique en Serial -- independant de l'enregistrement (pas
// besoin d'etre en REC pour l'avoir), utile pour corréler un faux
// declenchement de detection avec la qualite du fix a un instant donne
// (ex: test statique au bureau, en interieur -- multipath). Ne remplace
// pas un vrai log de session, juste un battement de coeur pour suivre
// l'evolution en direct sans avoir a lancer un REC.
static const unsigned long GPS_STATUS_INTERVAL_MS = 60000; // 1 minute
static unsigned long lastGpsStatusPrint = 0;

static void printGpsStatusHeartbeat() {
  if (millis() - lastGpsStatusPrint < GPS_STATUS_INTERVAL_MS) return;
  lastGpsStatusPrint = millis();

  char timeBuf[12];
  getLocalDateTime(nullptr, 0, timeBuf, sizeof(timeBuf));

  if (!gpsActive) {
    Serial.printf("[%s] [Etat GPS] pas de module GPS detecte.\n", timeBuf);
    return;
  }
  Serial.printf("[%s] [Etat GPS] fix=%u sats=%u lat=%.7f lng=%.7f proximite_forcee=%s geofence_verifie=%s\n",
                timeBuf, liveData.fixStatus, liveData.numSVs,
                liveData.latitude / 1e7, liveData.longitude / 1e7,
                forcedLapAnything ? "oui" : "non",
                geofenceCheckDone ? "oui" : "non");
}

void loop() {
  pollGps(); // lit l'UART GPS, met a jour liveData/newGpsData/gpsActive -- plus de scan/connexion a gerer, le GPS tourne toujours, meme pendant le WiFi.
  debugReplayTick(); // no-op si le mode debogage n'est pas actif (cf. section dediee)
  printGpsStatusHeartbeat(); // no-op sauf toutes les 60s (cf. GPS_STATUS_INTERVAL_MS)

  if (gpsActive && newGpsData) {
    newGpsData = false;

    double lat   = liveData.latitude / 1e7;
    double lng    = liveData.longitude / 1e7;
    float  altM   = liveData.wgsAltitude / 1000.0f;
    float  speedKnots = liveData.speedMmPerS * 0.0019438f;

    processGpsFix(lat, lng, altM, speedKnots, millis());
  }

  // ----- Encodeur + boutons : comportement different selon l'ecran -----
  // BACK : appui court ou long, confirme par pollBackRelease() (anti-rebond
  // robuste, cf. plus haut) -- evite de manquer un appui si la boucle est
  // momentanement ralentie. BACK = "retour" partout sauf depuis
  // l'ecran statut, ou il ouvre directement le menu principal (court ou long,
  // meme effet -- plus de distinction court/long depuis cette refonte).
  unsigned long backHeldMs = 0;
  bool backReleased = pollBackRelease(backHeldMs);
  (void)backHeldMs; // plus utilise depuis la refonte du menu (un seul comportement pour BACK), garde pour reference future

  // CONFIRM/PUSH : appeles ici de la meme facon que backReleased ci-dessus
  // (hors de la chaine if/else par ecran) -- garde leurs statics internes
  // en phase en permanence, meme quand on change d'ecran entre deux appuis.
  unsigned long confirmHeldMs = 0, pushHeldMs = 0;
  bool confirmReleased = pollConfirmRelease(confirmHeldMs);
  bool pushReleased = pollPushRelease(pushHeldMs);
  (void)confirmHeldMs; (void)pushHeldMs; // plus utilises depuis que CONFIRM/PUSH ont des roles distincts sur l'ecran statut (cf. juste en dessous) -- gardes pour reference future

  if (screenState == SCREEN_STATUS) {
    // CONFIRM et PUSH ont maintenant chacun un role dedie sur cet ecran
    // (plus de redondance ici -- ils restent redondants partout ailleurs,
    // dans les menus, ou un seul choix -- "valider" -- est possible).
    // Avant, les deux faisaient la meme chose (demarrer/arreter le REC en
    // appui court, forcer le mode proximite en appui long >=800ms) -- gaspillait
    // un bouton physique dedie pour une fonction deja couverte par le clic
    // de l'encodeur, et le geste d'appui long n'etait pas franchement
    // decouvrable (rien a l'ecran ne l'indiquait).
    if (pushReleased) {
      toggleRecording();
    }
    if (confirmReleased) {
      forceLapAnythingManually(); // cf. section dediee -- arme aussi la capture auto de nouveau circuit
    }
    if (backReleased) {
      cancelForcedLapAnything(); // annule un CONFIRM presse par erreur -- no-op si rien n'a ete force
      menuSelection = 0;
      rotaryEncoder.setBoundaries(0, MAIN_MENU_COUNT - 1, true);
      rotaryEncoder.setEncoderValue(menuSelection);
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_MAIN_MENU) {
    if (rotaryEncoder.encoderChanged()) menuSelection = rotaryEncoder.readEncoder();
    if (pollConfirmPress() || rotaryEncoder.isEncoderButtonClicked()) {
      switch (menuSelection) {
        case 0: { // Circuit
          int circuitSel = manualOverrideActive ? (manualCourseIndex + 1) : 0;
          rotaryEncoder.setBoundaries(0, myTracks.courseCount, true); // Auto + 8 circuits seulement
          rotaryEncoder.setEncoderValue(circuitSel);
          menuSelection = circuitSel;
          screenState = SCREEN_CIRCUIT_MENU;
          break;
        }
        case 1: // Connexion
          screenState = SCREEN_CONNEXION;
          break;
        case 2: // Session
          parseSessionsFile();
          sessionListSelection = 0;
          if (sessionCount() > 0) rotaryEncoder.setBoundaries(0, sessionCount() - 1, false);
          rotaryEncoder.setEncoderValue(0);
          screenState = SCREEN_SESSION_LIST;
          break;
        default: // 3 : Reglages
          menuSelection = 0;
          rotaryEncoder.setBoundaries(0, SETTINGS_MENU_COUNT - 1, true);
          rotaryEncoder.setEncoderValue(menuSelection);
          screenState = SCREEN_SETTINGS;
          break;
      }
    }
    if (backReleased) screenState = SCREEN_STATUS;

  } else if (screenState == SCREEN_CIRCUIT_MENU) {
    if (rotaryEncoder.encoderChanged()) menuSelection = rotaryEncoder.readEncoder();
    if (pollConfirmPress() || rotaryEncoder.isEncoderButtonClicked()) {
      if (menuSelection == 0) activateAutoMode();
      else activateManualCourse(menuSelection - 1);
      screenState = SCREEN_STATUS; // circuit choisi -> retour direct a l'ecran principal
    }
    if (backReleased) {
      menuSelection = 0;
      rotaryEncoder.setBoundaries(0, MAIN_MENU_COUNT - 1, true);
      rotaryEncoder.setEncoderValue(menuSelection);
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_CONNEXION) {
    if (backReleased) screenState = SCREEN_MAIN_MENU;

  } else if (screenState == SCREEN_SESSION_LIST) {
    if (rotaryEncoder.encoderChanged()) sessionListSelection = rotaryEncoder.readEncoder();
    if ((pollConfirmPress() || rotaryEncoder.isEncoderButtonClicked()) && sessionCount() > 0) {
      viewingSessionIndex = sessionListSelection;
      int lapCount = (int)sessionAt(viewingSessionIndex).laps.size();
      sessionLapSelection = lapCount > 0 ? lapCount - 1 : 0; // dernier tour affiche en premier
      if (lapCount > 0) rotaryEncoder.setBoundaries(0, lapCount - 1, false);
      rotaryEncoder.setEncoderValue(sessionLapSelection);
      screenState = SCREEN_SESSION_LAPS;
    }
    if (backReleased) {
      menuSelection = 2;
      rotaryEncoder.setBoundaries(0, MAIN_MENU_COUNT - 1, true);
      rotaryEncoder.setEncoderValue(menuSelection);
      screenState = SCREEN_MAIN_MENU;
    }

  } else if (screenState == SCREEN_SESSION_LAPS) {
    if (rotaryEncoder.encoderChanged()) sessionLapSelection = rotaryEncoder.readEncoder();
    if (backReleased) {
      if (sessionCount() > 0) rotaryEncoder.setBoundaries(0, sessionCount() - 1, false);
      rotaryEncoder.setEncoderValue(sessionListSelection);
      screenState = SCREEN_SESSION_LIST;
    }

  } else if (screenState == SCREEN_SETTINGS) {
    if (rotaryEncoder.encoderChanged()) menuSelection = rotaryEncoder.readEncoder();
    if (pollConfirmPress() || rotaryEncoder.isEncoderButtonClicked()) {
      webServerManager.startDownloadMode();
      screenState = SCREEN_WIFI;
    }
    if (backReleased) {
      menuSelection = 3;
      rotaryEncoder.setBoundaries(0, MAIN_MENU_COUNT - 1, true);
      rotaryEncoder.setEncoderValue(menuSelection);
      screenState = SCREEN_MAIN_MENU;
    }

  } else { // SCREEN_WIFI
    if (backReleased) {
      webServerManager.stopDownloadMode();
      screenState = SCREEN_STATUS;
    }
  }

  handleSerialCommands();

  webServerManager.loop();

  // Rafraichissement OLED ~4Hz -- largement assez pour un ecran qui ne
  // bouge qu'au changement de tour ou de menu, pas un chrono live.
  // Contrairement au firmware TFT, pas besoin de couper le rendu pendant
  // le WiFi : pas de sprites a proteger, le tampon OLED est minuscule.
  static unsigned long lastRender = 0;
  if (millis() - lastRender >= 250) {
    lastRender = millis();
    switch (screenState) {
      case SCREEN_STATUS:        drawStatusScreen(); break;
      case SCREEN_MAIN_MENU:     drawMainMenuScreen(); break;
      case SCREEN_CIRCUIT_MENU:  drawCircuitMenuScreen(); break;
      case SCREEN_CONNEXION:     drawConnexionScreen(); break;
      case SCREEN_SESSION_LIST:  drawSessionListScreen(); break;
      case SCREEN_SESSION_LAPS:  drawSessionLapsScreen(); break;
      case SCREEN_SETTINGS:      drawSettingsScreen(); break;
      default:                   drawWifiScreen(); break;
    }
  }
}
