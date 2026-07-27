#include "GpsManager.h"
#include <stdlib.h>
#include <string.h>

// ===================== GPS Quectel LC76G (UART direct, NMEA 0183) =====================
//
// Remplace la variante UBX/NEO-M9N -- meme interface GpsManager.h
// (GpsData/liveData/newGpsData/gpsActive/initGps()/pollGps()), le reste
// du firmware (CourseManager, main.cpp, WebServerManager, ecran) ne
// change pas. Seule cette implementation differe : on parse des trames
// texte NMEA (RMC/GGA/GSA) au lieu de trames binaires UBX NAV-PVT.

// Pins GPS -- inchanges par rapport au NEO-M9N (GPIO5/6 sur ce montage
// Tiny N8R8, cf. note historique dans la variante precedente). Le LC76G
// se cable de la meme facon (TX module -> GPIO6, RX module -> GPIO5).
#define GPS_RX_PIN 6  // <- TX du module GPS
#define GPS_TX_PIN 5  // -> RX du module GPS

HardwareSerial GPSSerial(1); // UART1 de l'ESP32 (l'UART0 reste pour le Serial USB de debug)

// Le LC76G annonce 115200 par defaut (contrairement au 9600 constate sur
// le NEO-M9N de ce montage) -- a confirmer/rebalayer avec debugBaudScan()
// si "GPS -- (recherche)" reste bloque au premier essai avec le module
// reellement recu.
#define GPS_BAUD 115200

GpsData liveData;
volatile bool newGpsData = false;

// true des qu'une trame de position valide (RMC ou GGA) a ete recue
// recemment -- timeout plutot que sticky, pour detecter un fil
// debranche ou un module en panne.
bool gpsActive = false;
static unsigned long lastGpsFrameAt = 0;
static const unsigned long GPS_TIMEOUT_MS = 3000;

// ===================== Envoi de commandes PAIR (config module) =====================
//
// Protocole PROPRIETAIRE Quectel "$PAIR" -- PAS le protocole legacy
// MediaTek "$PMTK" utilise par les anciens modules GlobalTop/L76 basiques.
// Piege identifie au banc (confirme par un fil du forum Quectel avec
// exactement le meme symptome : PMTK314 renvoie "$PMTKxxx,ERROR,3" sur
// LC76G) -- le module comprend la syntaxe generique "$xxx,champs*cs"
// (d'ou un checksum valide accepte), mais ne reconnait pas les
// identifiants de commande PMTK. Meme format d'encodage/checksum que
// PMTK (XOR entre '$' et '*'), donc sendPAIR() reutilise la meme logique
// que l'ancien sendPMTK().
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
// (plage 100-1000, defaut 1000) -- 100 = 10Hz. A NOTER : certaines
// variantes LC76G (PA)/(PB) ne supportent PAS cette commande d'apres la
// doc Quectel ("the position fix rate remains at [1Hz]") -- si le
// PAIR001 d'accuse de reception renvoie une erreur malgre la bonne
// syntaxe, c'est probablement le cas de ce module precis, pas un bug de
// notre cote.
static void configureFixRate10Hz() {
  sendPAIR("PAIR050,100");
}

// PAIR062 (PAIR_COMMON_SET_NMEA_OUTPUT_RATE) : active/coupe chaque type
// de trame NMEA individuellement (contrairement a PMTK314 qui prenait
// tout en un seul message). Type de trame en 1er argument (0=GGA,
// 1=GLL, 2=GSA, 3=GSV, 4=RMC, 5=VTG, 6=ZDA -- ordre documente Quectel),
// taux en 2e argument (0=coupe, 1=chaque fix, N=1 fois toutes les N
// trames). On garde GGA/GSA/RMC (nos 3 parseurs), on coupe le reste.
static void configureOutputSentences() {
  sendPAIR("PAIR062,0,1"); delay(50); // GGA -- garde, 1x par fix
  sendPAIR("PAIR062,1,0"); delay(50); // GLL -- coupe, non parse
  sendPAIR("PAIR062,2,1"); delay(50); // GSA -- garde, 1x par fix
  sendPAIR("PAIR062,3,0"); delay(50); // GSV -- coupe, non parse
  sendPAIR("PAIR062,4,1"); delay(50); // RMC -- garde, 1x par fix
  sendPAIR("PAIR062,5,0"); delay(50); // VTG -- coupe, non parse
}

// ===================== DEBUG : balayage de baudrate GPS =====================
// Desactive par defaut (cf. initGps()) -- utile si le module recu ne
// demarre pas a 115200 comme annonce. 115200 place en tete de liste
// (defaut documente LC76G), le reste des debits usuels ensuite.
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
// Parsing NMEA + config PAIR050/PAIR062 valides au banc (10Hz confirme,
// 19/20 satellites en interieur, cf. tests du 23/07) -- repasse a 0 par
// defaut pour retrouver un fonctionnement aussi silencieux que le
// NEO-M9N. Remettre a 1 en cas de nouveau souci a diagnostiquer.
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
// nativement les champs vides (deux virgules collees). Retourne "" et
// laisse *p sur le '\0' final si on a atteint la fin de la trame.
static char* nextField(char** p) {
  if (**p == '\0') return *p; // deja en fin de trame -- champ vide

  char* fieldStart = *p;
  char* comma = strchr(*p, ',');
  if (comma) {
    *comma = '\0';
    *p = comma + 1;
  } else {
    *p = fieldStart + strlen(fieldStart); // fin de trame -- plus de virgule
  }
  return fieldStart;
}

// Convertit un champ latitude/longitude NMEA (format ddmm.mmmm ou
// dddmm.mmmm) + son indicateur de direction (N/S/E/W) en degres
// decimaux * 1e7 (meme convention que liveData.longitude/latitude,
// heritee du format UBX NAV-PVT).
static int32_t parseNmeaCoord(const char* coordField, const char* dirField) {
  if (coordField[0] == '\0') return 0;

  double raw = atof(coordField);         // ddmm.mmmm ou dddmm.mmmm
  int deg = (int)(raw / 100.0);
  double minutes = raw - (deg * 100.0);
  double decDeg = deg + (minutes / 60.0);

  if (dirField[0] == 'S' || dirField[0] == 'W') decDeg = -decDeg;

  return (int32_t)(decDeg * 1e7);
}

// $xxRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a,mode*hh
// Fournit heure/date UTC + position + vitesse -- equivalent du gros de
// NAV-PVT cote UBX. Statut 'A' = valide, 'V' = invalide (pas de fix) :
// on ignore la trame dans ce cas (meme logique que le bit validDate/
// validTime cote UBX -- on ne met a jour que si les donnees sont sures).
static void parseRMC(char* fields) {
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
    return; // pas de fix -- trame ignoree, comme cote UBX
  }

  if (strlen(time_) >= 6) {
    char buf[3] = {0};
    buf[0] = time_[0]; buf[1] = time_[1]; liveData.hour   = atoi(buf);
    buf[0] = time_[2]; buf[1] = time_[3]; liveData.minute = atoi(buf);
    buf[0] = time_[4]; buf[1] = time_[5]; liveData.second = atoi(buf);

    // Partie fractionnaire (hhmmss.ss) -- typiquement 2 decimales
    // (~10ms de resolution), convertie en nanosecondes pour rester
    // compatible avec le champ liveData.nanoseconds (rempli en
    // sub-microseconde cote UBX, mais le champ n'a jamais besoin de
    // plus que quelques ms de precision dans ce projet).
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

  // Vitesse en noeuds -> mm/s (1 noeud = 1852/3600 m/s = 514.444 mm/s).
  if (speed[0] != '\0') {
    liveData.speedMmPerS = (uint32_t)(atof(speed) * 514.444);
  }

  newGpsData = true;
  gpsActive = true;
  lastGpsFrameAt = millis();

#if GPS_DEBUG_LOG
  debugPrintLiveData(); // altitude/numSVs peuvent encore etre a 0 tant que la 1ere trame GGA du cycle n'est pas encore passee -- normal les tout premiers appels
#endif
}

// $xxGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,q,ss,h.h,a.a,M,g.g,M,a.a,xxxx*hh
// Fournit l'altitude et le nombre de satellites -- absents de RMC.
// Champ qualite de fix (q) : 0 = invalide, 1 = fix GPS, 2 = DGPS, etc.
// -- on ne met a jour que si q > 0 (equivalent d'un fix "au moins 2D").
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

  if (fixQuality[0] == '\0' || atoi(fixQuality) == 0) return; // pas de fix

  liveData.numSVs = (uint8_t)atoi(numSV);
  if (altitude[0] != '\0') {
    liveData.wgsAltitude = (int32_t)(atof(altitude) * 1000.0); // m -> mm
  }

  gpsActive = true;
  lastGpsFrameAt = millis();
}

// $xxGSA,A,m,sv1,...,sv12,p.p,h.h,v.v,systemId*hh
// Champ m (mode fix) : 1 = pas de fix, 2 = fix 2D, 3 = fix 3D --
// correspond directement a la convention u-blox deja utilisee dans
// liveData.fixStatus (0=aucun/1=DR/2=2D/3=3D) ; ici DR (1) n'existe pas
// en NMEA standard donc on ne remplit que 0/2/3.
static void parseGSA(char* fields) {
  nextField(&fields); // mode selection (A=auto, M=manuel), non utilise
  char* fixMode = nextField(&fields);

  if (fixMode[0] == '\0') return;
  int mode = atoi(fixMode);
  liveData.fixStatus = (mode == 2 || mode == 3) ? (uint8_t)mode : 0;
}

// Buffer de ligne NMEA -- une trame fait au plus 82 caracteres par la
// norme, 100 laisse une marge confortable.
static char nmeaLine[100];
static uint16_t nmeaIdx = 0;

// Verifie le checksum XOR (entre '$' et '*') et dispatche vers le bon
// parseur selon les 3 derniers caracteres du talker+type de trame
// (ex: "RMC" dans "$GNRMC" ou "$GPRMC" -- le prefixe GN/GP/GL/GA varie
// selon la/les constellations utilisees pour le fix, sans impact sur le
// contenu des champs).
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
    return; // pas de checksum -- trame incomplete/corrompue
  }

  uint8_t expectedChecksum = (uint8_t)strtol(star + 1, nullptr, 16);
  uint8_t actualChecksum = 0;
  for (char* p = line + 1; p < star; p++) actualChecksum ^= (uint8_t)*p;
  if (actualChecksum != expectedChecksum) {
#if GPS_DEBUG_LOG
    Serial.printf("[GPS] checksum NMEA invalide (attendu %02X, calcule %02X) -- trame ignoree: \"%s\"\n", expectedChecksum, actualChecksum, line);
#endif
    return; // checksum invalide -- trame ignoree
  }

  *star = '\0'; // coupe avant "*hh", ne garde que "$xxxxx,champ,champ,..."

  if (strlen(line) < 7) return;
  const char* sentenceType = line + 3; // saute "$" + 2 lettres de talker (GN/GP/GL/GA/GB)
  char* fields = line + 7;             // saute "$xxxxx," (7 caracteres : $ + 2 lettres talker + 3 lettres type + virgule)

  if (strncmp(sentenceType, "RMC", 3) == 0)      parseRMC(fields);
  else if (strncmp(sentenceType, "GGA", 3) == 0) parseGGA(fields);
  else if (strncmp(sentenceType, "GSA", 3) == 0) parseGSA(fields);
#if GPS_DEBUG_LOG
  else if (strncmp(line + 1, "PAIR", 4) == 0) {
    // Accuse de reception PAIR001 -- format $PAIR001,<idCommande_hex>,<resultat>*hh
    // (protocole natif du LC76G, remplace le PMTK001 legacy).
    Serial.printf("[GPS] reponse module: \"%s\"\n", line);
  }
  else if (strncmp(line + 1, "PMTK", 4) == 0) {
    Serial.printf("[GPS] reponse module (PMTK, inattendu sur ce module): \"%s\"\n", line);
  }
#endif
}

// Accumule les octets recus caractere par caractere jusqu'a '\n',
// traite la ligne, reset le buffer -- a appeler a chaque tour de loop().
void pollGps() {
  while (GPSSerial.available()) {
    char c = (char)GPSSerial.read();

    if (c == '\r') continue; // ignore, on se cale sur '\n'

    if (c == '\n') {
      nmeaLine[nmeaIdx] = '\0';
      if (nmeaIdx > 0) parseNmeaSentence(nmeaLine);
      nmeaIdx = 0;
      continue;
    }

    if (nmeaIdx < sizeof(nmeaLine) - 1) {
      nmeaLine[nmeaIdx++] = c;
    } else {
      nmeaIdx = 0; // ligne trop longue/corrompue -- on resynchronise sur la suivante
    }
  }

  if (gpsActive && millis() - lastGpsFrameAt > GPS_TIMEOUT_MS) {
    gpsActive = false; // plus de trames depuis GPS_TIMEOUT_MS -- fil debranche ou module en panne
  }
}

// Configure et demarre le module GPS -- a appeler une fois dans setup().
void initGps() {
  // debugBaudScan(); // DEBUG -- decommenter si le fix redevient introuvable
                       // avec le module reellement recu (115200 annonce par defaut)

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  // ETAPE 2 : flux NMEA natif confirme (fix 3D, 16 satellites, cf. test
  // du 23/07) -- on peut desormais reduire le volume de trames (RMC/GGA/
  // GSA seulement, coupe VTG/GSV/GLL) et monter la cadence a 10Hz.
  configureOutputSentences();
  delay(100);
  configureFixRate10Hz();
  delay(100);

  Serial.println("GPS LC76G configure (115200 bauds, PAIR050/PAIR062, cible 10Hz sur GGA/GSA/RMC).");
}
