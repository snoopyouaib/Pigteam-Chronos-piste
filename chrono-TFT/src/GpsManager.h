#pragma once

#include <Arduino.h>

// ===================== Module GPS =====================
//
// Interface volontairement minimale et stable, pensee pour rester
// identique quel que soit le module/protocole GPS branche derriere
// (u-blox NEO-M8N/M9N en UBX aujourd'hui, eventuellement un module
// NMEA type Waveshare LC76G plus tard). Le reste du firmware
// (CourseManager, WebServerManager, affichage, ecrans...) ne lit/ecrit
// jamais que GpsData/liveData -- jamais un octet UBX brut. Un futur
// backend NMEA n'aurait donc qu'a fournir sa propre implementation de
// initGps()/pollGps() qui peuple les memes champs, sans toucher au
// reste du projet.
//
// GpsManager.cpp/h sont mutualises avec la variante OLED (meme
// interface, meme structure GpsData) -- SEULS les pins et le debit
// initial different d'un projet a l'autre (cf. GpsManager.cpp) selon le
// cablage reel de chaque carte. Si tu modifies cette interface, pense a
// reporter le changement dans l'autre projet (cf. README, section
// WebServerManager, meme principe de partage).

struct GpsData {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  uint32_t nanoseconds;    // fraction de seconde, pour une precision sub-seconde au log
  uint8_t  fixStatus;      // convention u-blox : 0=aucun, 1=dead-reckoning, 2=2D, 3=3D...
  uint8_t  numSVs;
  int32_t  longitude;      // 1e-7 deg
  int32_t  latitude;       // 1e-7 deg
  int32_t  wgsAltitude;    // mm
  uint32_t speedMmPerS;    // mm/s
};

// Derniere position/vitesse/horodatage connus -- mise a jour par pollGps().
extern GpsData liveData;

// true a positionner a false par l'appelant une fois la trame consommee
// (cf. loop() dans main.cpp) -- signale qu'une nouvelle trame vient
// d'etre parsee depuis le dernier passage.
extern volatile bool newGpsData;

// true des qu'une trame valide a ete recue recemment (timeout glissant,
// pas sticky) -- sert d'indicateur "source active" (ecran Connexion,
// page Statut web).
extern bool gpsActive;

// Configure et demarre le module GPS -- a appeler une fois dans setup().
void initGps();

// Lit l'UART GPS, met a jour liveData/newGpsData/gpsActive -- a appeler
// a chaque tour de loop().
void pollGps();
