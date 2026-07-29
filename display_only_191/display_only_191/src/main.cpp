/**
 * Chrono GPS moto piste -- display_only_191 (banc de test d'affichage)
 * ----------------------------------------------------------------------
 * Repris a l'identique du vrai firmware (firmware_191, meme repo) cote
 * ecrans/navigation/tactile -- AUCUNE dependance GPS/SD/batterie/WiFi
 * reelle ici, tout est simule en RAM (cf. bloc "Simulation moteur" plus
 * bas). But : calibrer/valider l'affichage sur le banc avant de reporter
 * les changements dans firmware_191.
 *
 * Ecran + bouton PUSH simple (rotatif EC11 retire du vrai firmware le
 * 27/07, jamais cable ici) + bouton BACK.
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

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "display_bsp.h"
#include "fonts_teko.h"
#include "splash_pigteam.h"

// display_only_191 -- banc de test ecran : AUCUNE dependance GPS/SD/
// batterie/WiFi reelle, tout est simule en RAM (cf. bloc "Simulation
// moteur" plus bas). CourseManager/GpsManager/SdLogStorage/adc_bsp/
// WebServerManager restent dans lib/ (repris tels quels du firmware
// reel pour reference future) mais ne sont plus inclus ni appeles ici.

// ===================== Pins (valides au bring-up 1.91) =====================
// Rotatif physiquement retire (27/07) -- remplace par un bouton poussoir
// simple sur la meme broche (ENCODER_CLK/DT ne sont plus cables). La
// navigation anneau/listes, auparavant pilotee par la rotation, repose
// desormais entierement sur le tactile (swipe pour l'anneau, tap direct
// pour les listes -- deja fonctionnel en parallele avant ce changement,
// cf. ringScreenGestureCb() et les commentaires "Tap: choisir").
#define PUSH_BUTTON   10
#define BACK_BUTTON   14

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
  unsigned long minutes = ms / 60000;
  unsigned long seconds = (ms / 1000) % 60;
  unsigned long millisPart = ms % 1000;
  snprintf(buf, bufSize, "%lu:%02lu.%03lu", minutes, seconds, millisPart);
}

// ===================== Simulation moteur (banc de test, sans GPS/SD/batterie/WiFi reels) =====================
//
// Repris a l'identique de firmware_191 cote signatures de fonctions
// (detectionEffectivelyComplete, getDisplayState, recordingEnabled,
// startRecording/stopRecording, myTracks, activateAutoMode/
// activateManualCourse, armNewCircuitCapture/cancelNewCircuitCapture,
// readBatteryPercent, loadSessionSummaries/loadLapsForSession,
// gpsFixStatus/gpsNumSVs/gpsSpeedKmh/gpsActive) -- seul le contenu
// change, tout vient d'une simulation RAM plutot que du GPS/SD/ADC
// reels. Ecrans/navigation/tactile (plus bas) sont identiques au vrai
// firmware et n'ont pas besoin de savoir que c'est simule.

// mm:ss.mmm -- deja definie plus haut (juste apres les ISR PUSH/BACK),
// reutilisee ici sans redefinition.

// ----- Horloge simulee (pas de fix GPS UTC reel) -----
// Demarre a une heure arbitraire (14:32:00) et avance avec millis().
static void getLocalDateTime(char* dateBuf, size_t dateBufSize, char* timeBuf, size_t timeBufSize) {
  unsigned long totalSec = (millis() / 1000) + (14UL * 3600 + 32UL * 60);
  unsigned long h = (totalSec / 3600) % 24;
  unsigned long m = (totalSec / 60) % 60;
  unsigned long s = totalSec % 60;
  if (dateBuf) snprintf(dateBuf, dateBufSize, "2026-01-01");
  if (timeBuf) snprintf(timeBuf, timeBufSize, "%02lu:%02lu:%02lu", h, m, s);
}

// ----- GPS simule (juste de quoi remplir Fix/Sat/vitesse a l'ecran) -----
static bool gpsActive = true;
static int gpsFixStatus = 3;
static int gpsNumSVs = 9;
static float gpsSpeedKmh = 0.0f;

// ----- Circuits simules (pas de circuits.csv, pas de LittleFS necessaire) -----
static const char* DEMO_COURSE_NAMES[] = { "PigTeam_track", "Croix-en-Ternois", "Circuit Carole" };
static const int DEMO_COURSE_COUNT = 3;
static int demoSelectedCourse = 0; // 0 = Auto, 1..N = circuit force manuellement

struct SimCourseEntry { const char* name; };
struct SimTrackList {
  int courseCount;
  SimCourseEntry courses[DEMO_COURSE_COUNT];
};
static SimTrackList myTracks = {
  DEMO_COURSE_COUNT,
  { { DEMO_COURSE_NAMES[0] }, { DEMO_COURSE_NAMES[1] }, { DEMO_COURSE_NAMES[2] } }
};

// true une fois qu'un circuit est "detecte" (choisi sur l'ecran Circuit,
// Auto ou manuel, OU 1er PUSH sur Statut -- cf. handlePush()) --
// equivalent banc de detectionEffectivelyComplete() reel (geofencing+
// GPS). Redevient false apres un arret definitif confirme
// (SCR_CONFIRM_STOP -> BACK -> activateAutoMode(), cf. plus bas,
// exactement comme le reset courseManager reel) ou pendant une capture
// de nouveau circuit armee. Demarre a false -- la simulation reproduit
// le cycle complet recherche->detecte->REC comme au reel.
static bool simDetected = false;

static bool newCircuitCaptureArmed = false;
static bool newCircuitAutoSaved = false;

static void activateAutoMode() {
  demoSelectedCourse = 0;
  simDetected = false; // comme le vrai courseManager->reset() -- redemarre la detection
  newCircuitCaptureArmed = false;
  Serial.println("Retour en mode detection automatique (simule).");
}
static void activateManualCourse(int index) {
  if (index < 0 || index >= myTracks.courseCount) return;
  demoSelectedCourse = index + 1;
  simDetected = true; // choisir un circuit precis = detection immediate, comme au reel
  newCircuitCaptureArmed = false;
  Serial.printf("Circuit force (simule) : %s\n", myTracks.courses[index].name);
}
static void armNewCircuitCapture() {
  simDetected = false; // comme courseManager->reset() au reel -- suspend la detection pendant la capture
  newCircuitCaptureArmed = true;
  newCircuitAutoSaved = false;
  Serial.println("Capture nouveau circuit armee (simulee -- juste pour tester l'ecran).");
}
static void cancelNewCircuitCapture() {
  newCircuitCaptureArmed = false;
}

static bool detectionEffectivelyComplete() { return simDetected; }
static const char* getActiveCourseNameForDisplay() {
  if (newCircuitCaptureArmed) return newCircuitAutoSaved ? "Circuit capture !" : "CAPTURE NEW TRACK";
  if (!detectionEffectivelyComplete()) return "Detection...";
  return DEMO_COURSE_NAMES[demoSelectedCourse == 0 ? 0 : demoSelectedCourse - 1];
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
// Duree du tour en cours, tiree au hasard a chaque tour (8-29s) --
// permet de voir varier dernier/meilleur tour sans attendre un temps
// fixe, utile pour verifier que les deux se mettent a jour correctement.
static unsigned long simCurrentLapTargetMs = 20000;
static unsigned long randomLapDurationMs() { return (unsigned long)random(8000, 30000); } // [8s, 30s[

// Simule quelques secondes de roulage dans le paddock avant le vrai
// depart (passage de ligne) -- au reel, currentLapMs reste a 0 tant que
// getRaceStarted() n'est pas true cote CourseManager/DovesLapTimer
// (ligne pas franchie), et l'ecran affiche la vitesse a la place du
// chrono pendant ce temps (cf. updateStatusScreen()). Ici, on decale le
// "vrai" depart de simLapStartMs dans le futur au lieu de le poser
// immediatement a millis() : getDisplayState()/checkLapCompletion()
// restent inchanges, ils voient juste un compteur qui n'a pas encore
// commence.
static const unsigned long SIM_PADDOCK_DELAY_MS = 6000UL;

// Fige l'affichage du chrono sur le temps du tour qui vient de se
// terminer pendant lapFreezeS secondes, comme au reel -- reglable
// depuis Reglages (tap sur la ligne, cycle parmi FREEZE_PRESETS_S).
// Contrairement au reel, pas de persistance LittleFS ici (banc
// entierement en RAM) -- revient a 10s par defaut a chaque reboot.
static unsigned long lapFreezeUntilMs = 0;
static int lapFreezeS = 10;
static const int FREEZE_PRESETS_S[] = { 0, 5, 10, 15, 20, 30 };
static const int FREEZE_PRESETS_COUNT = 6;

static void getDisplayState(unsigned long& currentLapMs, unsigned long& bestLapMs, bool& hasBest, int& lapsCount) {
  currentLapMs = (recordingEnabled && millis() >= simLapStartMs) ? (millis() - simLapStartMs) : 0;
  hasBest = simBestLapMs != ULONG_MAX;
  bestLapMs = hasBest ? simBestLapMs : 0;
  lapsCount = simLapsCount;
}
static unsigned long getLastFinishedLapMs() { return simLastLapMs; }

static void startRecording() {
  if (recordingEnabled) return;
  recordingEnabled = true;
  simLapStartMs = millis() + SIM_PADDOCK_DELAY_MS; // "vrai" depart differe -- simule le roulage paddock
  simCurrentLapTargetMs = randomLapDurationMs();
  simLastLapMs = 0;
  simBestLapMs = ULONG_MAX;
  simLapsCount = 0;
  lapFreezeUntilMs = 0;

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
  Serial.println("REC OFF (simule -- pause, le circuit reste detecte)");
}

// Simule la fin d'un tour a une duree aleatoire (8-29s) pendant le REC.
static void checkLapCompletion() {
  if (!recordingEnabled) return;
  if (millis() < simLapStartMs) return; // encore dans le paddock simule, pas encore parti
  unsigned long elapsed = millis() - simLapStartMs;
  if (elapsed < simCurrentLapTargetMs) return;

  simLapStartMs = millis();
  simCurrentLapTargetMs = randomLapDurationMs(); // tire la duree du prochain tour
  simLastLapMs = elapsed;
  if (simLastLapMs < simBestLapMs) simBestLapMs = simLastLapMs;
  simLapsCount++;
  lapFreezeUntilMs = millis() + (unsigned long)lapFreezeS * 1000UL;

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

// ----- Batterie simulee (cycle lent aller-retour, pas de lecture ADC) -----
static int readBatteryPercent() {
  const unsigned long period = 60000; // cycle complet ~60s : 100% -> 0% -> 100%
  unsigned long phase = millis() % period;
  float ratio = (phase < period / 2)
    ? (1.0f - (float)phase / (period / 2))
    : ((float)(phase - period / 2) / (period / 2));
  return (int)(ratio * 100.0f); // descend jusqu'a 0% -- permet de voir "NO BAT" se declencher sur le banc
}

// ----- Tick GPS simule, appele une fois par tour de loop() -----
static void simGpsTick() {
  gpsNumSVs = 8 + (int)((millis() / 3000) % 5);
  gpsSpeedKmh = recordingEnabled ? (60.0f + 40.0f * sinf(millis() / 4000.0f)) : 0.0f;
}

// ----- WiFi simule (pas de WebServerManager reel) -----
static const char* SIM_WIFI_SSID = "PigTeam-Chrono (simule)";
static const char* SIM_WIFI_IP = "192.168.4.1";

// ===================== Navigation =====================

enum AppScreen { SCR_STATUS, SCR_CIRCUIT, SCR_CONNEXION, SCR_SESSION_LIST, SCR_SESSION_LAPS, SCR_SETTINGS, SCR_WIFI, SCR_NEW_CIRCUIT, SCR_CONFIRM_STOP };
static AppScreen currentScreen = SCR_STATUS;
// Duree max sur SCR_CONFIRM_STOP avant arret definitif automatique --
// filet de securite si on oublie de valider (BACK) ou de reprendre
// (tactile/PUSH) en quittant la piste. Sans ca, l'ecran resterait arme
// indefiniment et un faux contact tactile pourrait relancer
// l'enregistrement (cf. bug du 27/07 -- circuit jamais desarme apres stop).
static const unsigned long CONFIRM_STOP_TIMEOUT_MS = 300000UL; // 5 minutes
static unsigned long confirmStopEnteredMs = 0;
// Faux contact electrique (bruit sur l'alim, pas forcement un vrai doigt)
// = evenement quasi instantane. Un vrai choix humain de REPRENDRE prend
// toujours au moins quelques centaines de ms de reaction -- on ignore donc
// REPRENDRE (tactile/PUSH) pendant cette fenetre courte apres l'ouverture
// de l'ecran. BACK (arret definitif) n'est PAS concerne : c'est toujours
// la direction "sure", pas besoin de la retarder.
static const unsigned long CONFIRM_STOP_INPUT_GRACE_MS = 600UL;
// Idem, mais cote ecran Statut : une fois l'arret DEFINITIF confirme, le
// geofencing (rayon 15km, cf. GEOFENCE_MAX_DISTANCE_M) peut redetecter le
// circuit et rearmer le bouton REC en ~1s si on est pres de la piste --
// constate au banc (cf. log Serial du 27/07 : "Retour en mode detection
// automatique" suivi de "REC ON" en une poignee de lignes). Cette fenetre
// protege specifiquement le REC de l'ecran Statut juste apres un arret
// definitif, en plus de la protection REPRENDRE ci-dessus.
static unsigned long lastDefinitiveStopMs = 0;
static const unsigned long STATUS_REC_GRACE_AFTER_STOP_MS = 1500UL;
static bool selectingMode = false; // true = mode "selection dans la liste" (Circuit/Session/Reglages), arme par PUSH

// Batterie trop faible pour demarrer un enregistrement -- meme seuil
// que le firmware reel. Batterie simulee ici, juste pour verifier
// l'affichage "NO BAT" et le blocage du PUSH.
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
    case SCR_CONFIRM_STOP: lv_scr_load(scrConfirmStop); break;
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
  lv_obj_set_style_text_font(lblBig, &lv_font_teko_bold_84, 0);
  lv_obj_set_style_text_color(lblBig, lv_color_white(), 0);

  lblClock = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblClock, &lv_font_teko_bold_56, 0); // taille intermediaire (etait bold_84, trop gros)
  lv_obj_set_style_text_color(lblClock, lv_color_white(), 0);

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
  lv_obj_set_style_text_font(lblDernier, &lv_font_teko_bold_38, 0); // etait medium_34, un poil trop petit
  lv_obj_set_style_text_color(lblDernier, lv_color_white(), 0);
  lv_obj_align(lblDernier, LV_ALIGN_TOP_LEFT, 4, 162);

  lblBest = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblBest, &lv_font_teko_bold_38, 0); // etait medium_34, un poil trop petit
  lv_obj_set_style_text_color(lblBest, lv_color_white(), 0);
  lv_obj_align(lblBest, LV_ALIGN_TOP_LEFT, 4, 202);

  lblTours = lv_label_create(scrStatus);
  lv_obj_set_style_text_font(lblTours, &lv_font_teko_bold_38, 0); // etait medium_34, aligne sur Dernier/Best
  lv_obj_set_style_text_color(lblTours, lv_color_white(), 0);
  lv_obj_align(lblTours, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
}

// ===================== Confirmation d'arret (a la RaceChrono) =====================
// BACK depuis SCR_STATUS pendant l'enregistrement ne stoppe plus
// directement -- il ouvre cet ecran. Le PUSH relance aussitot, le
// circuit etant reste arme (courseManager pas reset par stopRecording()).
// BACK confirme l'arret definitif (desarme le circuit, cf. handleBack()).
// Un timeout de securite (CONFIRM_STOP_TIMEOUT_MS) confirme automatiquement
// l'arret si on oublie de choisir avant de prendre la route. Aucun bouton
// tactile REPRENDRE (28/07) : preuve au Serial d'un faux contact capacitif
// declenchant l'enregistrement seul -- cf. commentaire pres de lblRecHint
// dans buildStatusScreen().
static lv_obj_t* lblConfirmCountdown;

static void buildConfirmStopScreen() {
  scrConfirmStop = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrConfirmStop, lv_color_black(), 0);
  lv_obj_clear_flag(scrConfirmStop, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lblTitle = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblTitle, &lv_font_teko_medium_34, 0);
  lv_obj_set_style_text_color(lblTitle, lv_palette_main(LV_PALETTE_ORANGE), 0);
  lv_label_set_text(lblTitle, "Enregistrement en pause");
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 20);

  // Pas de widget bouton ici (28/07) -- meme raison que lblRecHint, cf.
  // commentaire pres de sa creation dans buildStatusScreen(). Simple
  // texte d'etat, seul le PUSH declenche la reprise -- cf.
  // handlePush()/SCR_CONFIRM_STOP.
  lv_obj_t* lblReprendre = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblReprendre, &lv_font_teko_bold_38, 0);
  lv_obj_set_style_text_color(lblReprendre, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_label_set_text(lblReprendre, "PUSH pour reprendre");
  lv_obj_align(lblReprendre, LV_ALIGN_CENTER, 0, -10);

  lv_obj_t* lblBack = lv_label_create(scrConfirmStop);
  lv_obj_set_style_text_font(lblBack, &lv_font_teko_bold_38, 0); // etait medium_26, aligne sur "PUSH pour reprendre"
  lv_obj_set_style_text_color(lblBack, lv_color_white(), 0);
  lv_label_set_text(lblBack, "BACK pour arreter");
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

  } else {
    if (millis() < lapFreezeUntilMs) {
      // Tour vient de se terminer -- affiche encore son temps fige,
      // plutot que de reafficher direct le chrono du tour suivant.
      formatLapTime(getLastFinishedLapMs(), buf, sizeof(buf));
    } else if (currentLapMs > 0) {
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

static void buildCircuitScreen() {
  scrCircuit = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrCircuit, lv_color_black(), 0);
  enableRingSwipe(scrCircuit);
  createTitle(scrCircuit, "Circuit");

  circuitListCont = createScrollList(scrCircuit, 44); // pleine hauteur -- plus de bouton en bas, "Nouveau circuit" a son propre ecran dans l'anneau
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
  char buf[48];
  snprintf(buf, sizeof(buf), "%s  Fix:%d  Sat:%d", gpsActive ? "GPS OK" : "GPS --", gpsFixStatus, gpsNumSVs);
  lv_label_set_text(lblConnGps, buf);
  lv_label_set_text(lblConnCircuit, getActiveCourseNameForDisplay());

  // SD/batterie simulees -- pas de vraie carte/ADC sur ce banc, juste de
  // quoi verifier que les lignes s'affichent et se colorent comme au reel.
  snprintf(buf, sizeof(buf), "SD OK  4.2/29.7 GB (simule)");
  lv_obj_set_style_text_color(lblConnSd, lv_color_white(), 0);
  lv_label_set_text(lblConnSd, buf);

  int battPct = readBatteryPercent();
  snprintf(buf, sizeof(buf), "Batt %d%%  (simule)", battPct);
  lv_obj_set_style_text_color(lblConnBatt, battPct <= 15 ? lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
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
    char buf[48];
    snprintf(buf, sizeof(buf), "%sTour %d : %s%s", sel ? "> " : "  ", lap.lapNumber, lapBuf, isBest ? "  (best)" : "");
    lv_label_set_text(row, buf);
  }
}


// ===================== Ecran Reglages =====================

static lv_obj_t* lblSettingsRow;
static lv_obj_t* lblFreezeRow;

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
}

static void refreshSettingsScreen() {
  bool sel = selectingMode;
  lv_obj_set_style_text_color(lblSettingsRow, sel ? lv_palette_main(LV_PALETTE_YELLOW) : lv_color_white(), 0);
  lv_label_set_text(lblSettingsRow, sel ? "> WiFi telechargement" : "  WiFi telechargement");

  char buf[32];
  if (lapFreezeS == 0) snprintf(buf, sizeof(buf), "  Pause chrono: desactive");
  else snprintf(buf, sizeof(buf), "  Pause chrono: %ds", lapFreezeS);
  lv_label_set_text(lblFreezeRow, buf);
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
  snprintf(buf, sizeof(buf), "SSID: %s", SIM_WIFI_SSID);
  lv_label_set_text(lblWifiSsid, buf);
  snprintf(buf, sizeof(buf), "IP:   %s", SIM_WIFI_IP);
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
    case SCR_CONFIRM_STOP: refreshConfirmStopScreen(); break;
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
      if (recordingEnabled) {
        // Option 1 (comme sur TFT) : PUSH met en pause pendant
        // l'enregistrement -- pas BACK. stopRecording() ici agit comme une
        // pause : le fichier de log est ferme mais le circuit reste arme
        // (courseManager pas reset), pour permettre une reprise rapide.
        stopRecording();
        confirmStopEnteredMs = millis();
        goToScreen(SCR_CONFIRM_STOP);
        break;
      }
      if (millis() - lastDefinitiveStopMs < STATUS_REC_GRACE_AFTER_STOP_MS) break; // cf. commentaire pres de la constante
      if (!detectionEffectivelyComplete()) {
        // 1er PUSH depuis "recherche de circuit" -- simule la detection
        // (equivalent banc du geofencing GPS reel, qui serait automatique).
        simDetected = true;
        break;
      }
      if (readBatteryPercent() <= LOW_BATT_NO_REC_PERCENT) break; // "NO BAT" affiche a la place -- pas de nouvel enregistrement sous ce seuil
      startRecording();
      break;

    case SCR_CONFIRM_STOP:
      // Seule voie de reprise desormais (tactile retire, cf. buildConfirmStopScreen()).
      if (millis() - confirmStopEnteredMs < CONFIRM_STOP_INPUT_GRACE_MS) break; // cf. commentaire pres de la constante
      startRecording();
      goToScreen(SCR_STATUS);
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

    case SCR_CONFIRM_STOP:
      // Arret definitif : desarme le circuit (auto ou force manuellement)
      // pour qu'aucun faux contact ne puisse relancer l'enregistrement une
      // fois qu'on a quitte la piste. Le timeout dans loop() couvre le cas
      // ou on oublie de confirmer avant de prendre la route.
      activateAutoMode(); // reset complet du courseManager + des flags d'armement
      lastDefinitiveStopMs = millis(); // cf. STATUS_REC_GRACE_AFTER_STOP_MS -- le geofencing peut rearmer le circuit en ~1s
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
  randomSeed(esp_random()); // RNG materiel ESP32 -- sequence de tours differente a chaque boot
  delay(1000);
  Serial.println("=== display_only_191 -- banc de test ecran (Circuit/Nouveau circuit/Connexion/Session/Reglages) ===");

  I2C_master_Init();
  Serial.println("[1/5] I2C partage ok");
  Touch_Init();
  Serial.println("[2/5] Tactile ok");
  displayInit();
  Serial.println("[3/5] Ecran + LVGL ok");

  Serial.println("[3.5/5] GPS/SD/batterie/WiFi simules -- aucune init hardware necessaire ici.");

  pinMode(BACK_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BACK_BUTTON), backButtonISR, FALLING);
  pinMode(PUSH_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PUSH_BUTTON), pushButtonISR, FALLING);
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
    buildConfirmStopScreen();
    lvglUnlock();
  }
  Serial.println("[5/5] Ecrans construits, splash affiche");

  delay(3500); // laisse le splash visible plus longtemps avant de basculer sur le Statut (etait 2000ms)

  if (lvglLock(-1)) {
    lv_scr_load(scrStatus);
    lvglUnlock();
  }

  Serial.println("Pret. Statut: PUSH fait tourner recherche->detecte->REC (avec quelques secondes de paddock simule)->pause. BACK sur Statut va sur Circuit (ou choisis un circuit la-bas pour forcer la detection). Anneau: swipe = change d'ecran, tap = selection, BACK = retour.");
}

void loop() {
  unsigned long nowMs = millis();
  simGpsTick();
  checkLapCompletion(); // simule la fin d'un tour a une duree aleatoire (8-29s) pendant le REC -- no-op si REC off

  if (lvglLock(10)) {
    if (pushButtonPressed) {
      pushButtonPressed = false;
      handlePush();
    }
    if (backButtonPressed) {
      backButtonPressed = false;
      handleBack();
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
  // Pas de vraie radio WiFi ici (banc) -- juste consommer les flags pour
  // rester coherent avec settingsRowTappedCb()/handleBack(SCR_WIFI), qui
  // les positionnent comme au reel.
  if (wifiStartRequested) {
    wifiStartRequested = false;
    Serial.println("WiFi (simule) : demarrage demande.");
  }
  if (wifiStopRequested) {
    wifiStopRequested = false;
    Serial.println("WiFi (simule) : arret demande.");
  }

  if (currentScreen == SCR_CONFIRM_STOP) {
    // BUG CORRIGE (28/07) : nowMs est capture UNE FOIS en haut de loop(),
    // avant tout traitement de bouton. Si on entre sur SCR_CONFIRM_STOP
    // dans CETTE MEME iteration (confirmStopEnteredMs = millis() appele
    // plus tard, dans handlePush()), confirmStopEnteredMs peut alors etre
    // *posterieur* a nowMs. Comme ce sont des unsigned long, nowMs -
    // confirmStopEnteredMs ne devient pas negatif mais boucle par en
    // dessous (~4 milliards), ce qui depasse instantanement
    // CONFIRM_STOP_TIMEOUT_MS et confirme l'arret en quelques ms au lieu
    // de 2 minutes -- cause reelle de tous les "ca va trop vite pour etre
    // vu" observes, quel que soit le bouton implique. Fix : capturer un
    // millis() frais ici, strictement posterieur ou egal a
    // confirmStopEnteredMs puisque le temps ne remonte jamais.
    unsigned long nowCheck = millis();
    if (nowCheck - confirmStopEnteredMs >= CONFIRM_STOP_TIMEOUT_MS) {
      // Timeout de securite : personne n'a choisi (BACK ou REPRENDRE) --
      // on confirme l'arret tout seul plutot que de laisser cet ecran arme
      // indefiniment (cf. commentaire pres de CONFIRM_STOP_TIMEOUT_MS).
      activateAutoMode();
      lastDefinitiveStopMs = nowCheck; // cf. STATUS_REC_GRACE_AFTER_STOP_MS -- le geofencing peut rearmer le circuit en ~1s
      goToScreen(SCR_STATUS);
    }
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
