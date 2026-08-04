#pragma once

#include <Arduino.h>

// ===================== Module GPS =====================
//
// Portage direct depuis le projet chrono GPS TFT (meme interface, meme
// structure GpsData) -- seuls les pins changent (cf. GpsManager.cpp),
// adaptes au brochage TXD/RXD dedie de la carte AMOLED 1.64
// (GPIO43/44, UART0, libre car le monitoring serie passe par l'USB
// natif de ce board).

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
// pas sticky) -- sert d'indicateur "source active".
extern bool gpsActive;

// Debit RMC reellement mesure au boot (cf. measureActualRmcRate() dans
// GpsManager.cpp) -- ~10.0 si PAIR050 a bien pris, ~1.0 sinon (config
// non appliquee, ex. souci cablage TX vecu le 04/08). Fige a la valeur
// mesuree une seule fois au demarrage, ne se remet pas a jour ensuite.
extern float gpsMeasuredRmcHz;

// true si le module a explicitement acquitte PAIR050 (independant de
// gpsMeasuredRmcHz -- un module peut acquitter sans reellement changer
// de cadence, cf. diagnostic 04/08, d'ou les deux indicateurs distincts).
extern bool gpsFixRateAckOk;

// Configure et demarre le module GPS -- a appeler une fois dans setup().
void initGps();

// Lit l'UART GPS, met a jour liveData/newGpsData/gpsActive -- a appeler
// a chaque tour de loop().
void pollGps();
