#include "GpsManager.h"
#include <string.h>

// ===================== GPS NEO-M8N/M9N (UART direct, UBX/NAV-PVT) =====================
//
// Remplace le RaceBox/BLE. Meme structure de champs que l'ancien
// RaceBoxData pour ne rien casser en aval (getLocalDateTime, logRow,
// CourseManager...) -- fixStatus suit deja la convention u-blox
// (0=aucun,1=dead-reckoning,2=2D,3=3D...), identique a ce que renvoyait
// le RaceBox (lui-meme base sur une puce u-blox), donc le test
// `fixStatus >= 2` en aval reste valide sans modification.

// Pins GPS (ESP32-S3-Tiny N8R8) -- GPIO liberees par le retrait de la
// carte SD (cf. README, section migration/cablage). A adapter ici si un
// futur montage cable le module GPS ailleurs -- rien d'autre dans le
// firmware ne depend de ces deux constantes.
#define GPS_RX_PIN 4  // <- TX du module GPS
#define GPS_TX_PIN 5  // -> RX du module GPS

HardwareSerial GPSSerial(1); // UART1 de l'ESP32 (l'UART0 reste pour le Serial USB de debug)

// Module GPS NEO-M8N en UART direct -- branche sur les pins liberees par
// le retrait de la carte SD. Ce module demarre en 9600 bauds nativement
// (contrairement a certains M9N qui sortent deja en 38400) -- une
// bascule explicite vers GPS_TARGET_BAUD est donc necessaire, geree par
// la sequence de config (configurePortBaud/updateBaudRate/
// configureNavRate10Hz/enableNavPvt) juste apres l'ouverture du port.
// ATTENTION si un jour ce montage passe sur un module qui sort deja en
// 38400 en natif (ex. certains M9N) : GPS_INITIAL_BAUD doit alors valoir
// 38400 directement, sinon la toute premiere commande de bascule de
// debit ne sera jamais recue par le module (deja sur le mauvais debit
// avant meme d'avoir tente la conversation initiale a 9600).
#define GPS_INITIAL_BAUD 9600
#define GPS_TARGET_BAUD  38400

GpsData liveData;
volatile bool newGpsData = false;

// true des qu'une trame NAV-PVT valide a ete recue recemment -- sert au
// meme role d'indicateur "source active" que bleConnected avant (ecran
// Connexion, page Statut web). Base sur un timeout plutot que sticky :
// une liaison cablee ne se "deconnecte" pas comme du BLE, mais ce
// timeout detecte quand meme un fil debranche ou un module en panne.
bool gpsActive = false;
static unsigned long lastGpsFrameAt = 0;
static const unsigned long GPS_TIMEOUT_MS = 3000;

static void ubxChecksum(const uint8_t* data, uint16_t len, uint8_t& ckA, uint8_t& ckB) {
  ckA = 0; ckB = 0;
  for (uint16_t i = 0; i < len; i++) {
    ckA += data[i];
    ckB += ckA;
  }
}

// Construit et envoie une trame UBX complete (classe/ID/payload), calcule le checksum lui-meme.
static void sendUBX(uint8_t msgClass, uint8_t msgID, const uint8_t* payload, uint16_t payloadLen) {
  uint8_t header[4] = { msgClass, msgID, (uint8_t)(payloadLen & 0xFF), (uint8_t)(payloadLen >> 8) };

  uint8_t ckBuf[4 + 256];
  memcpy(ckBuf, header, 4);
  memcpy(ckBuf + 4, payload, payloadLen);
  uint8_t ckA, ckB;
  ubxChecksum(ckBuf, 4 + payloadLen, ckA, ckB);

  GPSSerial.write(0xB5);
  GPSSerial.write(0x62);
  GPSSerial.write(header, 4);
  GPSSerial.write(payload, payloadLen);
  GPSSerial.write(ckA);
  GPSSerial.write(ckB);
}

// UBX-CFG-PRT : configure le port UART1 du module (baudrate + protocoles in/out).
// On coupe le NMEA en sortie -- on ne veut que de l'UBX binaire.
static void configurePortBaud(uint32_t baud) {
  uint8_t payload[20] = {0};
  payload[0]  = 0x01;              // portID = 1 (UART1)
  payload[4]  = 0xD0; payload[5] = 0x08; // mode : 8N1
  payload[8]  = (uint8_t)(baud & 0xFF);
  payload[9]  = (uint8_t)((baud >> 8) & 0xFF);
  payload[10] = (uint8_t)((baud >> 16) & 0xFF);
  payload[11] = (uint8_t)((baud >> 24) & 0xFF);
  payload[12] = 0x01; payload[13] = 0x00; // inProtoMask : UBX only
  payload[14] = 0x01; payload[15] = 0x00; // outProtoMask : UBX only (pas de NMEA)
  sendUBX(0x06, 0x00, payload, sizeof(payload));
}

// UBX-CFG-RATE : 100ms = 10Hz, reference GPS time.
static void configureNavRate10Hz() {
  uint8_t payload[6] = { 0x64, 0x00, 0x01, 0x00, 0x01, 0x00 };
  sendUBX(0x06, 0x08, payload, sizeof(payload));
}

// UBX-CFG-MSG : active NAV-PVT (classe 0x01, ID 0x07) en sortie sur UART1, rate = 1.
static void enableNavPvt() {
  uint8_t payload[8] = { 0x01, 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
  sendUBX(0x06, 0x01, payload, sizeof(payload));
}

static void parseNavPvtPayload(const uint8_t* p) {
  // Bug constate sur le terrain (Croix-en-Ternois) : un log nomme avec
  // la date "2020-08-04" alors que le fix 3D etait deja bon (fixStatus=3,
  // bon nombre de satellites) des le debut de la trame. Cause : NAV-PVT
  // expose un octet "valid" (offset 11, bits validDate/validTime/
  // fullyResolved/validMag) qui n'etait pas verifie -- le fix de POSITION
  // peut etre bon avant que la date/heure UTC ne soit reellement
  // verrouillee (l'horodatage necessite en plus le numero de semaine GPS
  // et les secondes intercalaires, qui prennent un peu plus de temps a
  // se resoudre qu'une simple position). Sans ce controle, les toutes
  // premieres trames apres un demarrage a froid peuvent contenir une
  // date/heure provisoire et fausse, gravee ensuite dans le nom du
  // fichier de log. On ne met donc a jour la date/heure QUE si les deux
  // bits validDate ET validTime sont positionnes -- la position/vitesse
  // continuent d'etre mises a jour dans tous les cas (jamais concernees
  // par ce probleme, cf. fix=3 stable malgre la date fausse).
  bool dateTimeOk = (p[11] & 0x03) == 0x03; // bit0=validDate, bit1=validTime
  if (dateTimeOk) {
    liveData.year      = p[4] | (p[5] << 8);
    liveData.month     = p[6];
    liveData.day       = p[7];
    liveData.hour      = p[8];
    liveData.minute    = p[9];
    liveData.second    = p[10];
  }
  memcpy(&liveData.nanoseconds, p + 16, 4); // nano (int32 signe a l'origine, mais utilise seulement pour affichage sub-seconde)
  liveData.fixStatus = p[20];
  liveData.numSVs    = p[23];
  memcpy(&liveData.longitude,   p + 24, 4);
  memcpy(&liveData.latitude,    p + 28, 4);
  memcpy(&liveData.wgsAltitude, p + 32, 4); // height (ellipsoide) -- offset 32, pas hMSL (36), coherent avec wgsAltitude du RaceBox
  int32_t gSpeed;
  memcpy(&gSpeed, p + 60, 4);
  liveData.speedMmPerS = (uint32_t)(gSpeed < 0 ? 0 : gSpeed); // gSpeed est signe en theorie, toujours positif en pratique
  newGpsData = true;
  gpsActive = true;
  lastGpsFrameAt = millis();
}

// Machine a etats : cherche 0xB5 0x62, lit classe/ID/longueur, accumule
// le payload, verifie le checksum, traite si c'est NAV-PVT (0x01 0x07,
// longueur 92). A appeler a chaque tour de loop().
static uint8_t gpsRxBuf[128];
static uint16_t gpsRxIdx = 0;

void pollGps() {
  while (GPSSerial.available()) {
    uint8_t b = GPSSerial.read();

    if (gpsRxIdx == 0 && b != 0xB5) continue;
    if (gpsRxIdx == 1 && b != 0x62) { gpsRxIdx = 0; continue; }

    gpsRxBuf[gpsRxIdx++] = b;

    if (gpsRxIdx >= 6) {
      uint16_t payloadLen = gpsRxBuf[4] | (gpsRxBuf[5] << 8);
      uint16_t totalLen = 6 + payloadLen + 2;

      if (totalLen > sizeof(gpsRxBuf)) { gpsRxIdx = 0; continue; }

      if (gpsRxIdx == totalLen) {
        uint8_t ckA, ckB;
        ubxChecksum(gpsRxBuf + 2, 4 + payloadLen, ckA, ckB);
        if (gpsRxBuf[totalLen - 2] == ckA && gpsRxBuf[totalLen - 1] == ckB) {
          if (gpsRxBuf[2] == 0x01 && gpsRxBuf[3] == 0x07 && payloadLen == 92) {
            parseNavPvtPayload(gpsRxBuf + 6);
          }
        }
        gpsRxIdx = 0;
      }
    }
  }

  if (gpsActive && millis() - lastGpsFrameAt > GPS_TIMEOUT_MS) {
    gpsActive = false; // plus de trames depuis GPS_TIMEOUT_MS -- fil debranche ou module en panne
  }
}

// Envoie la config complete au module -- appelee au demarrage. Pas de
// UBX-CFG-CFG (sauvegarde en flash) : la plupart des breakouts NEO-M8N
// n'ont pas de flash externe, la pile de sauvegarde eventuelle ne
// persiste que la RAM de sauvegarde (position/almanach pour le hot
// start), pas la config UBX -- on la renvoie donc a chaque demarrage,
// ce qui ne coute que quelques ms.
void initGps() {
  GPSSerial.begin(GPS_INITIAL_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);
  configurePortBaud(GPS_TARGET_BAUD);
  delay(200);
  GPSSerial.updateBaudRate(GPS_TARGET_BAUD);
  delay(200);
  configureNavRate10Hz();
  delay(100);
  enableNavPvt();
  delay(100);
  Serial.println("GPS NEO-M8N configure (38400 bauds, 10Hz, NAV-PVT).");
}
