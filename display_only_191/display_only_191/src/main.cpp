/**
 * Chrono GPS moto piste -- sous-projet "display_only_191"
 * ---------------------------------------------------------
 * Meme principe que le display_only du projet TFT : ecran + encodeur
 * EC11 + bouton BACK, AUCUNE dependance GPS/SD/batterie/WiFi reelle --
 * toutes les donnees viennent d'une couche de simulation en RAM.
 *
 * Navigation (encodeur conserve, pas de menu liste separe) :
 *   - Depuis Statut : BACK -> ecran Circuit (1er ecran de l'anneau)
 *   - Anneau Circuit -> Connexion -> Session -> Reglages -> Circuit...
 *     (rotation encodeur fait tourner l'anneau)
 *   - Sur Circuit/Session/Reglages : PUSH entre en mode selection
 *     (l'encodeur change de role, selectionne un element de la liste),
 *     re-PUSH valide et sort du mode selection.
 *   - BACK depuis un ecran de l'anneau (hors mode selection) -> Statut.
 *   - BACK en mode selection -> annule, sort du mode selection (reste
 *     sur le meme ecran, anneau).
 *   - Session -> PUSH valide ouvre les tours de la session (ecran
 *     feuille), BACK y revient a la liste des sessions.
 *   - Reglages -> PUSH valide ouvre WiFi (ecran feuille), BACK y
 *     revient directement a Statut (comme le TFT d'origine).
 */

#include <Arduino.h>
#include <lvgl.h>
#include <AiEsp32RotaryEncoder.h>
#include <limits.h>
#include <math.h>
#include <vector>
#include <algorithm>

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "display_bsp.h"
#include "fonts_teko.h"
#include "splash_pigteam.h"

// display_only_191 -- banc de test ecran : AUCUNE dependance GPS/SD/
// batterie reelle, tout est simule en RAM (cf. bloc "Simulation moteur"
// plus bas). CourseManager/GpsManager/SdLogStorage/adc_bsp restent dans
// lib/ (repris tels quels du firmware reel pour reference future) mais
// ne sont plus inclus ni appeles ici.

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

// ===================== Horloge simulee (pas de fix GPS UTC reel) =====================
//
// Demarre a une heure arbitraire (14:32:00) et avance avec millis() --
// suffisant pour calibrer l'affichage de l'heure sur le banc, sans
// dependre d'un fix GPS.
static void getLocalDateTime(char* dateBuf, size_t dateBufSize, char* timeBuf, size_t timeBufSize) {
  unsigned long totalSec = (millis() / 1000) + (14UL * 3600 + 32UL * 60);
  unsigned long h = (totalSec / 3600) % 24;
  unsigned long m = (totalSec / 60) % 60;
  unsigned long s = totalSec % 60;
  if (dateBuf) snprintf(dateBuf, dateBufSize, "2026-01-01");
  if (timeBuf) snprintf(timeBuf, timeBufSize, "%02lu:%02lu:%02lu", h, m, s);
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

// ===================== Simulation moteur (banc de test, sans GPS/SD/batterie reels) =====================
//
// Meme principe que le display_only du projet TFT d'origine : un seul
// PUSH (depuis Statut) fait avancer un cycle Recherche -> Circuit
// reconnu -> REC -> Circuit reconnu (stop), toutes les donnees
// affichees (position/tours/vitesse/batterie) viennent d'une couche de
// simulation en RAM. But : placer/calibrer rapidement les ecrans sur
// le banc, sans GPS/SD/batterie branches. La vraie logique
// (CourseManager/GpsManager/SdLogStorage/adc_bsp, dans lib/) est
// portee dans firmware_191 une fois la mise en page validee ici.

enum DemoState { DEMO_SEARCHING, DEMO_DETECTED, DEMO_RECORDING };
static DemoState demoState = DEMO_SEARCHING;

// ----- Circuits simules (pas de circuits.csv, pas de LittleFS necessaire) -----
static const char* DEMO_COURSE_NAMES[] = { "PigTeam_track", "Croix-en-Ternois", "Circuit Carole" };
static const int DEMO_COURSE_COUNT = 3;
static int demoSelectedCourse = 0; // 0 = Auto (nom generique), 1..N = circuit force manuellement

struct SimCourseEntry { const char* name; };
struct SimTrackList {
  int courseCount;
  SimCourseEntry courses[DEMO_COURSE_COUNT];
};
static SimTrackList myTracks = {
  DEMO_COURSE_COUNT,
  { { DEMO_COURSE_NAMES[0] }, { DEMO_COURSE_NAMES[1] }, { DEMO_COURSE_NAMES[2] } }
};

static void activateAutoMode() { demoSelectedCourse = 0; }
static void activateManualCourse(int index) {
  if (index < 0 || index >= myTracks.courseCount) return;
  demoSelectedCourse = index + 1;
}
// Ecran "Nouveau circuit" garde a but de calibration d'UI -- pas de
// vraie capture GPS ici, juste de quoi tester l'ecran/le bouton.
static void armNewCircuitCapture() { Serial.println("Capture nouveau circuit (simulee -- juste pour tester l'ecran)."); }
static void cancelNewCircuitCapture() {}

static const char* getActiveCourseNameForDisplay() {
  if (demoState == DEMO_SEARCHING) return "Detection...";
  return DEMO_COURSE_NAMES[demoSelectedCourse == 0 ? 0 : demoSelectedCourse - 1];
}
static bool detectionEffectivelyComplete() {
  return demoState != DEMO_SEARCHING;
}

// ----- Sessions/tours simules (en RAM, pas de sessions.csv) -----
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
static std::vector<SessionSummary> simSessions;
static std::vector<std::vector<LapDetail>> simSessionLaps; // parallele a simSessions, meme index

static std::vector<SessionSummary> loadSessionSummaries() { return simSessions; }
static std::vector<LapDetail> loadLapsForSession(const String& compactKey) {
  for (size_t i = 0; i < simSessions.size(); i++) {
    if (simSessions[i].compactKey == compactKey) return simSessionLaps[i];
  }
  return {};
}
static String formatCompactKeyShort(const String& key) { return key; } // deja court en simulation

// ----- Enregistrement (REC) simule -----
static bool recordingEnabled = false;
static unsigned long simLapStartMs = 0;
static unsigned long simLastLapMs = 0;
static unsigned long simBestLapMs = ULONG_MAX;
static int simLapsCount = 0;
static const unsigned long SIM_LAP_DURATION_MS = 22000; // un "tour" toutes les ~22s en REC, pour voir defiler l'ecran

static void getDisplayState(unsigned long& currentLapMs, unsigned long& bestLapMs, bool& hasBest, int& lapsCount) {
  currentLapMs = recordingEnabled ? (millis() - simLapStartMs) : 0;
  hasBest = simBestLapMs != ULONG_MAX;
  bestLapMs = hasBest ? simBestLapMs : 0;
  lapsCount = simLapsCount;
}
static unsigned long getLastFinishedLapMs() { return simLastLapMs; }

static void startRecording() {
  if (recordingEnabled) return;
  recordingEnabled = true;
  simLapStartMs = millis();
  simLastLapMs = 0;
  simBestLapMs = ULONG_MAX;
  simLapsCount = 0;

  SessionSummary s;
  s.compactKey = String("SIM_") + String(millis() / 1000);
  s.lapCount = 0;
  s.bestLapMs = ULONG_MAX;
  simSessions.push_back(s);
  simSessionLaps.push_back(std::vector<LapDetail>());
  if (simSessions.size() > 8) { // meme limite d'affichage que le projet principal
    simSessions.erase(simSessions.begin());
    simSessionLaps.erase(simSessionLaps.begin());
  }
  Serial.println("REC ON (simule)");
}

static void stopRecording() {
  if (!recordingEnabled) return;
  recordingEnabled = false;
  Serial.println("REC OFF (simule)");
}

// Simule la fin d'un tour toutes les SIM_LAP_DURATION_MS pendant le REC.
static void checkLapCompletion() {
  if (!recordingEnabled) return;
  unsigned long elapsed = millis() - simLapStartMs;
  if (elapsed < SIM_LAP_DURATION_MS) return;

  simLapStartMs = millis();
  simLastLapMs = elapsed;
  if (simLastLapMs < simBestLapMs) simBestLapMs = simLastLapMs;
  simLapsCount++;

  if (!simSessions.empty()) {
    SessionSummary& s = simSessions.back();
    s.lapCount = simLapsCount;
    if (simLastLapMs < s.bestLapMs) s.bestLapMs = simLastLapMs;

    LapDetail lap;
    lap.lapNumber = simLapsCount;
    lap.lapMs = simLastLapMs;
    lap.circuit = getActiveCourseNameForDisplay();
    simSessionLaps.back().push_back(lap);
  }
  Serial.printf("Tour %d simule : %lu ms\n", simLapsCount, simLastLapMs);
}

// ----- Cycle demo pilote au PUSH (ou tap REC) depuis l'ecran Statut -----
static void simAdvanceDemoState() {
  if (demoState == DEMO_SEARCHING) {
    demoState = DEMO_DETECTED;
  } else if (demoState == DEMO_DETECTED) {
    demoState = DEMO_RECORDING;
    startRecording();
  } else { // DEMO_RECORDING
    demoState = DEMO_DETECTED;
    stopRecording();
  }
}

// ----- Batterie simulee (cycle lent aller-retour, pas de lecture ADC) -----
static int readBatteryPercent() {
  const unsigned long period = 60000; // cycle complet ~60s : 100% -> 20% -> 100%
  unsigned long phase = millis() % period;
  float ratio = (phase < period / 2)
    ? (1.0f - (float)phase / (period / 2))
    : ((float)(phase - period / 2) / (period / 2));
  return 20 + (int)(ratio * 80.0f);
}

// ----- GPS simule (juste de quoi remplir Fix/Sat/vitesse a l'ecran) -----
static bool gpsActive = true;
static uint8_t gpsFixStatus = 3;
static uint8_t gpsNumSVs = 9;
static float gpsSpeedKmh = 0.0f;

static void simGpsTick() {
  gpsNumSVs = 8 + (uint8_t)((millis() / 3000) % 5);
  gpsSpeedKmh = recordingEnabled ? (60.0f + 40.0f * sinf(millis() / 4000.0f)) : 0.0f;
}

// ===================== Navigation =====================

enum AppScreen { SCR_STATUS, SCR_CIRCUIT, SCR_CONNEXION, SCR_SESSION_LIST, SCR_SESSION_LAPS, SCR_SETTINGS, SCR_WIFI, SCR_NEW_CIRCUIT };
static AppScreen currentScreen = SCR_STATUS;
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

static bool screenDirty = true; // force un redessin complet au prochain refresh

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
  if (demoState == DEMO_DETECTED) {
    simAdvanceDemoState(); // -> DEMO_RECORDING
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

static void buildSessionLapsScreen() {
  scrSessionLaps = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSessionLaps, lv_color_black(), 0);
  lv_obj_clear_flag(scrSessionLaps, LV_OBJ_FLAG_SCROLLABLE); // le scroll se fait dans le conteneur interne, pas sur l'ecran
  createTitle(scrSessionLaps, "Tours");

  lapListCont = createScrollList(scrSessionLaps, 44);
  lblLapEmpty = createListRow(scrSessionLaps, 44);
  lblLapHint = createHint(scrSessionLaps);
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

static void buildWifiScreen() {
  scrWifi = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrWifi, lv_color_black(), 0);
  createTitle(scrWifi, "WiFi (simule)");

  lv_obj_t* l1 = createListRow(scrWifi, 46);
  lv_label_set_text(l1, "SSID: ChronoMoto191-SIM");
  lv_obj_t* l2 = createListRow(scrWifi, 86);
  lv_label_set_text(l2, "IP:   192.168.4.1");
  lv_obj_t* l3 = createListRowSmall(scrWifi, 130);
  lv_label_set_text(l3, "(page factice -- pas de vrai serveur");
  lv_obj_t* l4 = createListRowSmall(scrWifi, 156);
  lv_label_set_text(l4, " WiFi dans ce sous-projet display_only)");

  lv_obj_t* hint = createHint(scrWifi);
  lv_label_set_text(hint, "BACK: statut"); // raccourci -- la version longue debordait sur le titre
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
    case SCR_WIFI:         break; // statique, rien a rafraichir
    case SCR_NEW_CIRCUIT:  break; // statique, rien a rafraichir
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
      simAdvanceDemoState(); // cycle recherche -> circuit reconnu -> REC -> circuit reconnu (stop)
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
      cancelNewCircuitCapture(); // no-op ici, laisse pour coherence avec les autres ecrans
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
      rotaryEncoder.setBoundaries(0, (int)sessionListCache.size() - 1, false);
      rotaryEncoder.setEncoderValue(sessionListSelection);
      goToScreen(SCR_SESSION_LIST); // remet selectingMode a false en interne
      selectingMode = true;         // on veut rester en mode selection sur la liste des sessions
      screenDirty = true;
      break;

    case SCR_WIFI:
      goToScreen(SCR_STATUS); // coupe le "WiFi" et revient direct au statut, comme le TFT
      break;
  }
}

// ===================== Setup / Loop =====================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== display_only_191 -- navigation (Circuit/Nouveau circuit/Connexion/Session/Reglages) ===");

  I2C_master_Init();
  Serial.println("[1/5] I2C partage ok");
  Touch_Init();
  Serial.println("[2/5] Tactile ok");
  displayInit();
  Serial.println("[3/5] Ecran + LVGL ok");

  Serial.println("[3.5/5] GPS/SD/batterie simules -- aucune init hardware necessaire ici.");

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
    lvglUnlock();
  }
  Serial.println("[5/5] Ecrans construits, splash affiche");

  delay(3500); // laisse le splash visible plus longtemps avant de basculer sur le Statut (etait 2000ms)

  if (lvglLock(-1)) {
    lv_scr_load(scrStatus);
    lvglUnlock();
  }

  Serial.println("Pret. Statut: PUSH fait tourner recherche->detecte->REC->detecte (stop). BACK va sur Circuit. Anneau: tourne = change d'ecran, PUSH = selection, BACK = retour.");
}

void loop() {
  unsigned long nowMs = millis();
  simGpsTick();
  checkLapCompletion(); // simule la fin d'un tour toutes les SIM_LAP_DURATION_MS pendant le REC -- no-op si REC off

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
