#include "GpsManager.h"
#include <stdlib.h>
#include <string.h>

// ===================== GPS Quectel LC76G (UART direct, NMEA 0183) =====================
//
// Portage du GpsManager.cpp du projet chrono GPS TFT -- meme interface
// GpsManager.h, meme logique de parsing NMEA (RMC/GGA/GSA). Seul le
// brochage change : broches TXD/RXD dediees de la carte AMOLED 1.91
// (GPIO12/13, UART1 -- deplace de GPIO43/44 le 04/08, cf. README
// section "Diagnostic GPS bloque a 1Hz") au lieu des GPIO5/6 du
// montage Tiny N8R8.

#define GPS_RX_PIN 12  // <- TX du module GPS -- deplace de 44 (test bout de fil TX/RX mort sur 43/44, cf. diagnostic 04/08)
#define GPS_TX_PIN 13  // -> RX du module GPS -- deplace de 43

HardwareSerial GPSSerial(1); // UART1 de l'ESP32 (l'UART0 reste pour le Serial USB de debug)

// Confirme au banc sur ce montage (cf. test brut precedent) : le module
// repond bien a 115200 des le premier essai, fix 3D/10 satellites
// obtenu directement.
#define GPS_BAUD 115200

GpsData liveData;
volatile bool newGpsData = false;

// true des qu'une trame de position valide (RMC ou GGA) a ete recue
// recemment -- timeout plutot que sticky, pour detecter un fil
// debranche ou un module en panne.
bool gpsActive = false;
static unsigned long lastGpsFrameAt = 0;
static const unsigned long GPS_TIMEOUT_MS = 3000;

// Resultats du diagnostic 10Hz au boot, exposes pour l'ecran Connexion
// (cf. GpsManager.h) -- pas d'acces console possible sur la moto, donc
// ces infos doivent etre lisibles a l'ecran.
float gpsMeasuredRmcHz = 0.0f;
bool gpsFixRateAckOk = false;

// ===================== Envoi de commandes PAIR (config module) =====================
//
// Protocole PROPRIETAIRE Quectel "$PAIR" -- PAS le protocole legacy
// MediaTek "$PMTK". Meme format d'encodage/checksum que PMTK (XOR entre
// '$' et '*').
static void sendPAIR(const char* sentenceBody) {
  uint8_t checksum = 0;
  for (const char* p = sentenceBody; *p; p++) checksum ^= (uint8_t)*p;

  GPSSerial.print('$');
  GPSSerial.print(sentenceBody);
  GPSSerial.print('*');
  if (checksum < 0x10) GPSSerial.print('0');
  GPSSerial.println(checksum, HEX);
}

// PAIR050 (PAIR_COMMON_SET_FIX_RATE) : intervalle de calcul du fix en ms
// (plage 100-1000, defaut 1000) -- 100 = 10Hz.
static void configureFixRate10Hz() {
  sendPAIR("PAIR050,100");
}

// PAIR062 (PAIR_COMMON_SET_NMEA_OUTPUT_RATE) : active/coupe chaque type
// de trame NMEA individuellement. On garde GGA/GSA/RMC (nos 3 parseurs),
// on coupe le reste (GLL/GSV/VTG).
static void configureOutputSentences() {
  sendPAIR("PAIR062,0,1"); delay(50); // GGA -- garde, 1x par fix
  sendPAIR("PAIR062,1,0"); delay(50); // GLL -- coupe, non parse
  sendPAIR("PAIR062,2,1"); delay(50); // GSA -- garde, 1x par fix
  sendPAIR("PAIR062,3,0"); delay(50); // GSV -- coupe, non parse
  sendPAIR("PAIR062,4,1"); delay(50); // RMC -- garde, 1x par fix
  sendPAIR("PAIR062,5,0"); delay(50); // VTG -- coupe, non parse
}

// ===================== Verification post-config (ACK + debit reel) =====================
//
// Ajoute suite au diagnostic du 04/08 : configureFixRate10Hz() envoyait
// PAIR050,100 sans jamais verifier si le module l'acceptait. Les logs de
// la session Carole du 01/08 montrent un delta constant de ~1000ms entre
// fixes traites (donc 1Hz reel) malgre cette commande -- ce qui explique
// probablement les franchissements de ligne rates a haute vitesse (~53m
// entre deux mesures a 190km/h, bien plus large que la zone de detection
// de la ligne). Les deux fonctions ci-dessous rendent ca visible au boot.

// Lit une ligne NMEA/PAIR brute directement sur GPSSerial (hors pollGps(),
// utilise uniquement pendant l'init). Retourne false si aucune ligne
// complete n'est recue avant timeoutMs (ne renvoie jamais de ligne
// partielle/tronquee).
static bool readGpsLineBlocking(char* buf, size_t bufSize, unsigned long timeoutMs) {
  unsigned long deadline = millis() + timeoutMs;
  size_t idx = 0;
  while ((long)(deadline - millis()) > 0) {
    while (GPSSerial.available()) {
      char c = (char)GPSSerial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        buf[idx] = '\0';
        return idx > 0;
      }
      if (idx < bufSize - 1) buf[idx++] = c;
    }
  }
  return false;
}

// Cherche l'ACK $PAIR001,050,<result>*hh (reponse a PAIR050) dans le flux,
// pendant maxWaitMs. result='0' = commande acceptee par le module.
static void confirmFixRateAck(unsigned long maxWaitMs) {
  char line[96];
  unsigned long deadline = millis() + maxWaitMs;
  bool ackSeen = false;
  int otherLines = 0;

  while ((long)(deadline - millis()) > 0) {
    if (!readGpsLineBlocking(line, sizeof(line), deadline - millis())) break;
    if (strncmp(line, "$PAIR001,050,", 13) == 0) {
      char result = line[13];
      gpsFixRateAckOk = (result == '0');
      Serial.printf("[GPS] ACK PAIR050 recu : \"%s\" -- %s\n",
                    line, result == '0' ? "OK, commande acceptee par le module"
                                         : "ECHEC, le module a refuse la commande");
      ackSeen = true;
      break;
    }
    // Toute autre ligne recue pendant la fenetre -- affichee en brut pour
    // voir si le module repond autre chose que ce qu'on attend (au lieu de
    // la jeter silencieusement comme avant).
    Serial.printf("[GPS] (brut, pendant l'attente ACK) : \"%s\"\n", line);
    otherLines++;
  }

  if (!ackSeen) {
    Serial.printf("[GPS] Aucun ACK PAIR001,050 recu dans le delai (%d autre(s) ligne(s) vue(s)) -- "
                  "soit le module ne repond pas aux PAIR050 sur ce firmware, "
                  "soit le module connecte n'est pas un Quectel (protocole PAIR non reconnu), "
                  "soit la commande est arrivee trop tot apres begin().\n", otherLines);
  }
}

// Mesure le nombre reel de trames RMC recues sur measureMs -- verification
// independante de l'ACK, car un module peut repondre "OK" sans reellement
// changer de cadence. En dessous de 8Hz on considere que le 10Hz n'a pas
// pris effet.
static void measureActualRmcRate(unsigned long measureMs) {
  char line[96];
  unsigned long deadline = millis() + measureMs;
  int rmcCount = 0;

  while ((long)(deadline - millis()) > 0) {
    if (!readGpsLineBlocking(line, sizeof(line), deadline - millis())) continue;
    if (strlen(line) >= 6 && strncmp(line + 3, "RMC", 3) == 0) rmcCount++;
  }

  float hz = rmcCount * 1000.0f / (float)measureMs;
  gpsMeasuredRmcHz = hz;
  Serial.printf("[GPS] Debit RMC mesure : %d trames en %lums (~%.1fHz) -- %s\n",
                rmcCount, measureMs, hz,
                hz >= 8.0f ? "10Hz actif, config OK"
                           : "toujours proche de 1Hz -- la commande PAIR050 n'a PAS ete appliquee");
}

// ===================== DEBUG : balayage de baudrate GPS =====================
// Desactive ici -- 115200 deja confirme sur ce montage au test brut
// precedent. Conserve au cas ou tu changes de module GPS plus tard.
static void debugEchoRawGps(unsigned long durationMs) {
  unsigned long start = millis();
  bool anyByteSeen = false;
  while (millis() - start < durationMs) {
    while (GPSSerial.available()) {
      int b = GPSSerial.read();
      anyByteSeen = true;
      if (b >= 0x20 && b < 0x7F) {
        Serial.write((uint8_t)b);
      } else {
        Serial.printf("[%02X]", b);
      }
    }
  }
  Serial.println();
  if (!anyByteSeen) Serial.println("  (rien recu a ce debit)");
}

static void debugBaudScan() {
  const uint32_t candidates[] = { 115200, 9600, 38400, 57600, 19200, 4800 };
  for (uint32_t baud : candidates) {
    Serial.printf("\n[DEBUG] --- Test a %u bauds (3s) ---\n", baud);
    GPSSerial.end();
    delay(50);
    GPSSerial.begin(baud, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    delay(100);
    debugEchoRawGps(3000);
  }
  Serial.println("[DEBUG] Fin du balayage -- cherche le debit ou tu vois du texte NMEA lisible ($GNRMC, $GNGGA...).");
  GPSSerial.end();
  delay(50);
}

// ===================== DEBUG : dump liveData a chaque trame RMC =====================
// Active par defaut sur ce projet de bring-up (contrairement au projet
// TFT ou c'est remis a 0 une fois valide) -- utile pour confirmer que
// le parsing NMEA fonctionne bien sur ce nouveau montage. A repasser a
// 0 une fois le portage confirme si tu recuperes ce module tel quel
// dans un firmware final.
#define GPS_DEBUG_LOG 0

#if GPS_DEBUG_LOG
static void debugPrintLiveData() {
  Serial.printf("[GPS] %04u-%02u-%02u %02u:%02u:%02u  fix=%u  sats=%u  lat=%.6f  lon=%.6f  alt=%.1fm  speed=%.1fkm/h\n",
                liveData.year, liveData.month, liveData.day,
                liveData.hour, liveData.minute, liveData.second,
                liveData.fixStatus, liveData.numSVs,
                liveData.latitude / 1e7, liveData.longitude / 1e7,
                liveData.wgsAltitude / 1000.0,
                liveData.speedMmPerS * 3.6 / 1000.0); // mm/s -> km/h
}
#endif

// ===================== Parsing NMEA =====================

// Retourne le prochain champ (entre virgules), en avancant *p au
// caractere suivant la virgule. Remplace la virgule par '\0' -- gere
// nativement les champs vides (deux virgules collees).
static char* nextField(char** p) {
  if (**p == '\0') return *p;

  char* fieldStart = *p;
  char* comma = strchr(*p, ',');
  if (comma) {
    *comma = '\0';
    *p = comma + 1;
  } else {
    *p = fieldStart + strlen(fieldStart);
  }
  return fieldStart;
}

// Convertit un champ latitude/longitude NMEA (format ddmm.mmmm ou
// dddmm.mmmm) + son indicateur de direction (N/S/E/W) en degres
// decimaux * 1e7 (meme convention que liveData.longitude/latitude).
static int32_t parseNmeaCoord(const char* coordField, const char* dirField) {
  if (coordField[0] == '\0') return 0;

  double raw = atof(coordField);
  int deg = (int)(raw / 100.0);
  double minutes = raw - (deg * 100.0);
  double decDeg = deg + (minutes / 60.0);

  if (dirField[0] == 'S' || dirField[0] == 'W') decDeg = -decDeg;

  return (int32_t)(decDeg * 1e7);
}

// $xxRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a,mode*hh
// Compteur de trames RMC recues, incremente ici quelle que soit la
// validite du fix -- meme logique que measureActualRmcRate() (qui
// comptait toute trame RMC recue, cf. plus haut), pour rester
// comparable. Expose (cf. GpsManager.h) pour un calcul de debit
// periodique cote main.cpp SANS bloquer le parsing GPS reel -- contrairement
// a measureActualRmcRate() qui bloque 2s en lecture directe du port,
// acceptable une seule fois au boot (avant que loop()/CourseManager ne
// tournent) mais PAS pendant une session en cours (ca volerait des
// trames reelles au parsing, risque concret de rater un franchissement
// de ligne a haute vitesse). uint32_t suffit largement : a 10Hz continu,
// le débordement (4.3 milliards) prendrait ~13 ans.
uint32_t gpsRmcSentenceCount = 0;

static void parseRMC(char* fields) {
  gpsRmcSentenceCount++;

  char* time_  = nextField(&fields);
  char* status = nextField(&fields);
  char* lat    = nextField(&fields);
  char* latDir = nextField(&fields);
  char* lon    = nextField(&fields);
  char* lonDir = nextField(&fields);
  char* speed  = nextField(&fields);
  nextField(&fields); // course (cap), non utilise
  char* date   = nextField(&fields);

  if (status[0] != 'A') {
#if GPS_DEBUG_LOG
    Serial.printf("[GPS] RMC recu -- pas de fix (statut='%c') -- module actif, en attente de satellites.\n", status[0] ? status[0] : '?');
#endif
    return;
  }

  if (strlen(time_) >= 6) {
    char buf[3] = {0};
    buf[0] = time_[0]; buf[1] = time_[1]; liveData.hour   = atoi(buf);
    buf[0] = time_[2]; buf[1] = time_[3]; liveData.minute = atoi(buf);
    buf[0] = time_[4]; buf[1] = time_[5]; liveData.second = atoi(buf);

    const char* dot = strchr(time_, '.');
    liveData.nanoseconds = dot ? (uint32_t)(atof(dot) * 1e9) : 0;
  }

  if (strlen(date) >= 6) {
    char buf[3] = {0};
    buf[0] = date[0]; buf[1] = date[1]; liveData.day   = atoi(buf);
    buf[0] = date[2]; buf[1] = date[3]; liveData.month = atoi(buf);
    buf[0] = date[4]; buf[1] = date[5]; liveData.year  = 2000 + atoi(buf);
  }

  liveData.latitude  = parseNmeaCoord(lat, latDir);
  liveData.longitude = parseNmeaCoord(lon, lonDir);

  if (speed[0] != '\0') {
    liveData.speedMmPerS = (uint32_t)(atof(speed) * 514.444); // noeuds -> mm/s
  }

  newGpsData = true;
  gpsActive = true;
  lastGpsFrameAt = millis();

#if GPS_DEBUG_LOG
  debugPrintLiveData();
#endif
}

// $xxGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,q,ss,h.h,a.a,M,g.g,M,a.a,xxxx*hh
static void parseGGA(char* fields) {
  nextField(&fields); // heure -- deja couverte par RMC
  nextField(&fields); // lat -- deja couverte par RMC
  nextField(&fields); // N/S
  nextField(&fields); // lon -- deja couverte par RMC
  nextField(&fields); // E/W
  char* fixQuality = nextField(&fields);
  char* numSV      = nextField(&fields);
  nextField(&fields); // HDOP
  char* altitude   = nextField(&fields);

  if (fixQuality[0] == '\0' || atoi(fixQuality) == 0) return;

  liveData.numSVs = (uint8_t)atoi(numSV);
  if (altitude[0] != '\0') {
    liveData.wgsAltitude = (int32_t)(atof(altitude) * 1000.0);
  }

  gpsActive = true;
  lastGpsFrameAt = millis();
}

// $xxGSA,A,m,sv1,...,sv12,p.p,h.h,v.v,systemId*hh
static void parseGSA(char* fields) {
  nextField(&fields); // mode selection (A=auto, M=manuel), non utilise
  char* fixMode = nextField(&fields);

  if (fixMode[0] == '\0') return;
  int mode = atoi(fixMode);
  liveData.fixStatus = (mode == 2 || mode == 3) ? (uint8_t)mode : 0;
}

static char nmeaLine[100];
static uint16_t nmeaIdx = 0;

static void parseNmeaSentence(char* line) {
  if (line[0] != '$') {
#if GPS_DEBUG_LOG
    Serial.printf("[GPS] octets recus mais ligne non-NMEA (pas de '$' en tete) -- baudrate probablement errone: \"%s\"\n", line);
#endif
    return;
  }

  char* star = strchr(line, '*');
  if (!star || strlen(star) < 3) {
#if GPS_DEBUG_LOG
    Serial.println("[GPS] trame NMEA incomplete (pas de checksum '*hh') -- ignoree.");
#endif
    return;
  }

  uint8_t expectedChecksum = (uint8_t)strtol(star + 1, nullptr, 16);
  uint8_t actualChecksum = 0;
  for (char* p = line + 1; p < star; p++) actualChecksum ^= (uint8_t)*p;
  if (actualChecksum != expectedChecksum) {
#if GPS_DEBUG_LOG
    Serial.printf("[GPS] checksum NMEA invalide (attendu %02X, calcule %02X) -- trame ignoree: \"%s\"\n", expectedChecksum, actualChecksum, line);
#endif
    return;
  }

  *star = '\0';

  if (strlen(line) < 7) return;
  const char* sentenceType = line + 3;
  char* fields = line + 7;

  if (strncmp(sentenceType, "RMC", 3) == 0)      parseRMC(fields);
  else if (strncmp(sentenceType, "GGA", 3) == 0) parseGGA(fields);
  else if (strncmp(sentenceType, "GSA", 3) == 0) parseGSA(fields);
#if GPS_DEBUG_LOG
  else if (strncmp(line + 1, "PAIR", 4) == 0) {
    Serial.printf("[GPS] reponse module: \"%s\"\n", line);
  }
#endif
}

void pollGps() {
  while (GPSSerial.available()) {
    char c = (char)GPSSerial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      nmeaLine[nmeaIdx] = '\0';
      if (nmeaIdx > 0) parseNmeaSentence(nmeaLine);
      nmeaIdx = 0;
      continue;
    }

    if (nmeaIdx < sizeof(nmeaLine) - 1) {
      nmeaLine[nmeaIdx++] = c;
    } else {
      nmeaIdx = 0;
    }
  }

  if (gpsActive && millis() - lastGpsFrameAt > GPS_TIMEOUT_MS) {
    gpsActive = false;
  }
}

void initGps() {
  // debugBaudScan(); // DEBUG -- inutile ici, 115200 deja confirme

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  configureOutputSentences();
  delay(100);
  configureFixRate10Hz();

  confirmFixRateAck(500);       // ACK explicite du module a PAIR050
  measureActualRmcRate(2000);   // debit RMC reellement observe sur 2s

  Serial.println("GPS LC76G configure (115200 bauds, PAIR050/PAIR062, cible 10Hz sur GGA/GSA/RMC).");
}
