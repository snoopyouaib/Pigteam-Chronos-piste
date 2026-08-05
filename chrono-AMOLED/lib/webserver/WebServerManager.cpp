#include "WebServerManager.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <vector>
#include <algorithm>
#include <CourseManager.h> // pour MAX_COURSES (DovesLapTimer.h) -- reutilise la meme constante que main.cpp au lieu de dupliquer "8" en dur, evite un desalignement si la lib change un jour

// Plus de <SD.h> -- retire suite a la migration vers l'ESP32-S3-Tiny
// N8R8, qui n'a plus de carte SD sur ce montage (cf. README, section
// migration). g_logsFs pointe donc toujours vers LittleFS desormais
// (cf. begin() dans main.cpp). Si une carte SD revient un jour sur un
// futur montage, cf. l'historique git pour retrouver ce bloc + le
// parametre logsFs de begin(), deja concu pour l'accueillir (fs::FS
// generique, pas de dependance directe a LittleFS dans la signature).

WebServerManager webServerManager;

static WebServer httpServer(80);

static String g_sessionLogPath; // copies locales -- begin() ne garde qu'un pointeur cote WebServerManager
static String g_circuitsFilePath;
static fs::FS* g_logsFs = &LittleFS; // filesystem des logs GPS (log_*.csv) -- SD ou repli LittleFS, fourni par main.cpp
static WebServerFlushLogsFn g_flushLogs = nullptr;
static WebServerGetStatusFn g_getStatus = nullptr;

// Parcourt les fichiers de log GPS par session (/log_*.csv) sur le
// filesystem des logs (g_logsFs -- SD ou repli LittleFS, cf. begin())
// et appelle callback(nom, taille) pour chacun.
template<typename Fn>
static void forEachGpsLogFile(Fn callback) {
  File root = g_logsFs->open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!name.startsWith("/")) name = "/" + name;
    if (name.indexOf("/log_") >= 0 && name.endsWith(".csv")) {
      callback(name, (uint32_t)f.size());
    }
    f.close(); // sinon les descripteurs s'accumulent -- root.openNextFile()
               // ne garantit pas la fermeture de l'ancien handle avant reassignation
    f = root.openNextFile();
  }
  root.close(); // jamais ferme auparavant -- fuite de descripteur a chaque appel
}

static void serveFile(fs::FS& fsRef, const char* path, const char* downloadName) {
  File f = fsRef.open(path, "r");
  if (!f) { httpServer.send(404, "text/plain", "Fichier introuvable."); return; }
  httpServer.sendHeader("Content-Disposition", String("attachment; filename=") + downloadName);
  httpServer.streamFile(f, "text/csv");
  f.close();
}

// ===================== Sauvegarde complete (/backup) -- ZIP streame =====================
//
// Regroupe le carnet (sessions.csv) et TOUS les logs GPS detailles
// (log_*.csv) dans une seule archive .zip, generee a la volee et
// streamee au fur et a mesure (transfert chunked) -- jamais assemblee
// entierement en RAM ni sur la flash, ce qui serait hors de portee vu
// la taille cumulee possible de plusieurs sessions detaillees.
//
// Format "stored" (pas de compression, methode 0) : plus simple a
// generer en streaming (pas de lib de compression embarquee), et les
// logs CSV/texte compressent de toute facon mal vu leur repetitivite
// limitee. Utilise la technique standard du "data descriptor" (flag
// 0x0008) : CRC32 et taille sont inconnus au moment d'ecrire l'en-tete
// local (le fichier n'a pas encore ete lu), donc ecrits a zero puis
// suivis d'un descripteur juste apres les donnees, une fois calcules.
// Le repertoire central (ecrit a la toute fin, quand tout est connu)
// n'a pas besoin de cette astuce. Format standard, ouvrable par
// n'importe quel outil (Explorateur Windows, 7-Zip, Archive Utility
// macOS, unzip...) -- pas un format maison.
//
// N'inclut PAS circuits.csv (export dedie deja existant sur /circuits)
// -- perimetre volontairement limite a "carnet + sessions" tel que
// demande, la definition des circuits n'etant pas une "session".

struct ZipCentralEntry {
  String name;
  uint32_t crc;
  uint32_t size;
  uint32_t localOffset;
};

static uint32_t g_zipOffset = 0; // position courante dans le flux ZIP en cours de generation

static void zipSend(const uint8_t* data, size_t len) {
  if (len == 0) return;
  httpServer.sendContent((const char*)data, len);
  g_zipOffset += (uint32_t)len;
}

static void zipPut16(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back((uint8_t)(v & 0xFF));
  buf.push_back((uint8_t)((v >> 8) & 0xFF));
}
static void zipPut32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back((uint8_t)(v & 0xFF));
  buf.push_back((uint8_t)((v >> 8) & 0xFF));
  buf.push_back((uint8_t)((v >> 16) & 0xFF));
  buf.push_back((uint8_t)((v >> 24) & 0xFF));
}

// CRC32 (PKZIP/zlib, polynome reflechi 0xEDB88320) -- version bit-a-bit
// sans table (256 entrees economisees) : le volume total en jeu (carnet
// + logs) reste largement dans ce que l'UART/WiFi peut de toute facon
// debiter, la table n'apporterait rien de perceptible ici.
static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return crc;
}

// Ecrit UN fichier dans le flux ZIP en cours, et enregistre son entree
// pour le repertoire central ecrit plus tard par handleBackupDownload().
// Retourne false si le fichier source est illisible (ignore, ne fait
// pas echouer toute l'archive -- mieux vaut une sauvegarde partielle
// qu'aucune).
//
// CRC32/taille pre-calcules par une PREMIERE lecture du fichier (pure
// lecture flash, rien n'est envoye) avant d'ecrire l'en-tete local --
// contrairement a une premiere version qui utilisait un "data
// descriptor" (crc/taille ecrits APRES les donnees, technique standard
// pour streamer sans connaitre la taille a l'avance). Ce choix n'etait
// pas anodin : sans crc/taille fiables des l'en-tete local, relire
// l'archive plus tard (cf. /import/restore) obligeait a d'abord
// atteindre la FIN du fichier (repertoire central) avant de pouvoir
// rien extraire -- ce qui imposait de stocker une copie complete de
// l'archive sur la flash le temps de l'analyse, en plus des fichiers
// finalement restaures. Sur une LittleFS deja bien remplie, ce besoin
// de "2x la place" faisait typiquement echouer la restauration (LittleFS
// "No more free space") pile au moment ou on en a le plus besoin. Le
// cout ici (lire chaque fichier source deux fois, un aller-retour
// flash bon marche) est largement prefere a cette contrainte d'espace :
// desormais l'entete local est directement complet et valide, ce qui
// permet a /import/restore d'extraire en flux continu, un octet a la
// fois, sans jamais stocker l'archive entiere nulle part.
static bool zipWriteFile(fs::FS& fsRef, const String& path, const String& zipName, std::vector<ZipCentralEntry>& central) {
  File f = fsRef.open(path, "r");
  if (!f) return false;

  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t size = 0;
  {
    uint8_t buf[512];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
      crc = crc32Update(crc, buf, (size_t)n);
      size += (uint32_t)n;
    }
  }
  crc ^= 0xFFFFFFFFUL;
  f.seek(0);

  uint32_t localOffset = g_zipOffset;
  std::vector<uint8_t> header;
  header.reserve(30 + zipName.length());
  zipPut32(header, 0x04034b50); // signature en-tete local
  zipPut16(header, 20);         // version necessaire (2.0)
  zipPut16(header, 0);          // flag : 0 -- crc/tailles connus des le depart, pas de descripteur
  zipPut16(header, 0);          // methode : stored (pas de compression)
  zipPut16(header, 0);          // heure DOS (non renseignee -- pas critique)
  zipPut16(header, 0x0021);     // date DOS (placeholder 01/01/1980)
  zipPut32(header, crc);
  zipPut32(header, size);
  zipPut32(header, size);
  zipPut16(header, (uint16_t)zipName.length());
  zipPut16(header, 0);          // pas de champ extra
  for (size_t i = 0; i < zipName.length(); i++) header.push_back((uint8_t)zipName[i]);
  zipSend(header.data(), header.size());

  uint8_t buf[512];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) zipSend(buf, (size_t)n);
  f.close();

  central.push_back({ zipName, crc, size, localOffset });
  return true;
}

static void handleBackupDownload() {
  if (g_flushLogs) g_flushLogs(); // meme precaution que pour un telechargement simple -- fichiers ouverts (carnet, log en cours) flushes avant lecture

  httpServer.sendHeader("Content-Disposition", "attachment; filename=pigteam_backup.zip");
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN); // transfert chunked -- taille totale inconnue a l'avance
  httpServer.send(200, "application/zip", "");

  g_zipOffset = 0;
  std::vector<ZipCentralEntry> central;

  zipWriteFile(LittleFS, g_sessionLogPath, g_sessionLogPath.substring(1), central);

  forEachGpsLogFile([&](const String& name, uint32_t /*size*/) {
    zipWriteFile(*g_logsFs, name, name.substring(1), central);
  });

  // ----- Repertoire central : une entree par fichier, ecrite maintenant
  // que CRC32/taille/offset sont tous connus -----
  uint32_t cdOffset = g_zipOffset;
  for (const ZipCentralEntry& e : central) {
    std::vector<uint8_t> cd;
    cd.reserve(46 + e.name.length());
    zipPut32(cd, 0x02014b50); // signature en-tete central
    zipPut16(cd, 20);         // version "made by"
    zipPut16(cd, 20);         // version necessaire
    zipPut16(cd, 0);          // flag : coherent avec l'en-tete local, plus de data descriptor
    zipPut16(cd, 0);          // methode : stored
    zipPut16(cd, 0);          // heure DOS
    zipPut16(cd, 0x0021);     // date DOS
    zipPut32(cd, e.crc);
    zipPut32(cd, e.size);
    zipPut32(cd, e.size);
    zipPut16(cd, (uint16_t)e.name.length());
    zipPut16(cd, 0);          // extra
    zipPut16(cd, 0);          // commentaire
    zipPut16(cd, 0);          // numero de disque
    zipPut16(cd, 0);          // attributs internes
    zipPut32(cd, 0);          // attributs externes
    zipPut32(cd, e.localOffset);
    for (size_t i = 0; i < e.name.length(); i++) cd.push_back((uint8_t)e.name[i]);
    zipSend(cd.data(), cd.size());
  }
  uint32_t cdSize = g_zipOffset - cdOffset;

  std::vector<uint8_t> eocd;
  zipPut32(eocd, 0x06054b50); // signature fin de repertoire central
  zipPut16(eocd, 0);
  zipPut16(eocd, 0);
  zipPut16(eocd, (uint16_t)central.size());
  zipPut16(eocd, (uint16_t)central.size());
  zipPut32(eocd, cdSize);
  zipPut32(eocd, cdOffset);
  zipPut16(eocd, 0); // pas de commentaire d'archive
  zipSend(eocd.data(), eocd.size());

  httpServer.sendContent(""); // termine le transfert chunked
}

// ===================== Resume par session (nb de tours, meilleur temps) =====================
//
// Parcourt /sessions.csv une fois et construit la correspondance
// "horodatage compact -> resume" pour pouvoir l'afficher a cote de
// chaque fichier GPS detaille sur la page de gestion. Les deux fichiers
// partagent le meme horodatage de demarrage (juste un formatage
// different : "AAAA-MM-JJ HH:MM:SS" cote carnet, "AAAAMMJJ_HHMMSS" cote
// nom de fichier) -- on normalise les deux vers la meme forme compacte
// pour les recoller.

struct SessionSummaryLite {
  String compactKey; // "AAAAMMJJHHMMSS", sans separateurs -- cle de correspondance
  int lapCount = 0;
  String bestLapTime = "--:--.---";
  String circuit;    // nom du circuit de la 1ere ligne rencontree -- "Route" pour le mode Route (cf. finalizeRouteSessionIfNeeded() dans main.cpp)
  // Distance/V.moy de la DERNIERE ligne lue -- n'a de sens comme total
  // de session que pour le mode Route (une seule ligne). Pour une
  // session multi-tours, ce ne serait que le dernier tour, pas un total
  // cumule -- a ne jamais afficher hors du cas Route cote appelant.
  String lastDistanceKm;
  String lastAvgSpeedKmh;
};

static bool isRouteSummary(const SessionSummaryLite& s) { return s.circuit == "Route"; }

// Compte les lignes de donnees presentes HORS de tout bloc "# session
// demarree"/"# session arretee" -- ne devrait normalement jamais
// arriver, mais peut se produire si finalizeRouteSessionIfNeeded() (ou
// tout autre code d'ecriture) tourne alors qu'aucune session n'est
// ouverte (cf. bug d'ordre d'ecriture corrige le 29/07 -- ce compteur
// sert justement a detecter s'il en reste d'anciennes, ecrites avant
// le fix). Scalaire uniquement (un entier), meme profil memoire que
// loadSessionSummaries().
static int countOrphanSessionLines() {
  File f = LittleFS.open(g_sessionLogPath, "r");
  if (!f) return 0;
  int count = 0;
  bool inBlock = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("date,")) continue;
    if (line.startsWith("# session demarree")) { inBlock = true; continue; }
    if (line.startsWith("# session arretee")) { inBlock = false; continue; }
    if (!inBlock) count++;
  }
  f.close();
  return count;
}

// Retire du carnet toutes les lignes de donnees orphelines (hors de
// tout bloc demarree/arretee), sans toucher aux blocs eux-memes ni a
// leurs marqueurs -- symetrique de pruneSessionFromCarnet() (qui vise
// UN bloc precis par sa cle), ici on vise au contraire tout ce qui n'a
// PAS de bloc du tout.
static void stripOrphanSessionLines() {
  File f = LittleFS.open(g_sessionLogPath, "r");
  if (!f) return;
  std::vector<String> keptLines;
  bool inBlock = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("# session demarree")) { inBlock = true; keptLines.push_back(line); continue; }
    if (line.startsWith("# session arretee")) { inBlock = false; keptLines.push_back(line); continue; }
    if (inBlock) keptLines.push_back(line); // sinon (orpheline) -- pas conservee
  }
  f.close();

  File out = LittleFS.open(g_sessionLogPath, "w");
  if (!out) return;
  for (const String& l : keptLines) out.println(l);
  out.close();
}

static String stripSeparators(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c != '-' && c != ':' && c != ' ' && c != '_') out += c;
  }
  return out;
}

static std::vector<SessionSummaryLite> loadSessionSummaries() {
  std::vector<SessionSummaryLite> result;
  File f = LittleFS.open(g_sessionLogPath, "r");
  if (!f) return result;

  SessionSummaryLite* current = nullptr;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("date,")) continue;

    if (line.startsWith("# session demarree")) {
      String rest = line.substring(line.indexOf("demarree") + 9);
      result.push_back(SessionSummaryLite());
      current = &result.back();
      current->compactKey = stripSeparators(rest);
      continue;
    }
    if (line.startsWith("# session arretee")) { current = nullptr; continue; }
    if (!current) continue;

    // Format : date,heure,numero_tour,temps_tour,meilleur_temps,circuit[,vmax,vmin,vavg,distance,...]
    int commaIdx[10], found = 0, start = 0;
    for (size_t i = 0; i < line.length() && found < 10; i++) {
      if (line[i] == ',') { commaIdx[found++] = (int)i; }
    }
    if (found >= 5) {
      current->lapCount++;
      current->bestLapTime = line.substring(commaIdx[3] + 1, commaIdx[4]);
      // Circuit : jusqu'a la 6e virgule si des champs supplementaires
      // suivent (vmax/vmin/... -- format actuel), sinon jusqu'a la fin
      // de la ligne (anciennes sessions a 6 champs, sans virgule finale).
      current->circuit = (found >= 6) ? line.substring(commaIdx[4] + 1, commaIdx[5]) : line.substring(commaIdx[4] + 1);
      if (found >= 10) {
        current->lastAvgSpeedKmh = line.substring(commaIdx[7] + 1, commaIdx[8]);
        current->lastDistanceKm = line.substring(commaIdx[8] + 1, commaIdx[9]);
      }
    }
  }
  f.close();
  return result;
}

static SessionSummaryLite findSummaryForFile(const std::vector<SessionSummaryLite>& summaries, const String& compactKeyFromFilename) {
  for (size_t i = 0; i < summaries.size(); i++) {
    if (summaries[i].compactKey == compactKeyFromFilename) return summaries[i];
  }
  return SessionSummaryLite(); // pas trouve -- lapCount=0, bestLapTime par defaut
}

// ===================== Detail par tour pour une session (tableau style RaceChrono) =====================
//
// Contrairement a SessionSummaryLite (juste un compte + le meilleur),
// cette fonction relit /sessions.csv pour extraire la liste complete
// des tours d'UNE session precise (identifiee par son compactKey) --
// necessaire pour afficher le tableau par tour (temps + diff colore)
// sous chaque carte de session sur la page d'accueil. Reste volontairement
// une lecture separee de loadSessionSummaries() plutot qu'une fusion --
// deux besoins differents (resume global vs detail d'une session), pas
// de raison de complexifier une fonction pour l'autre.
//
// LapDetail/loadLapsForSession()/lapTimeToMs() RETIRES -- c'etait la
// vraie cause du plantage de tas identifie apres une longue
// investigation (cf. README) : construire un std::vector<LapDetail>
// (copie d'elements contenant des String) declenchait une corruption
// memoire reproductible. La page /lap affiche desormais tous les tours
// en streamant directement chaque ligne de /sessions.csv, sans jamais
// construire ce conteneur. msToLapTime() reste utilisee (import
// RaceChrono).

static String msToLapTime(long ms) {
  if (ms < 0) return "--:--.---";
  long minutes = ms / 60000L;
  float seconds = (ms % 60000L) / 1000.0f;
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld:%06.3f", minutes, seconds);
  return String(buf);
}

// Inverse de msToLapTime() -- reintroduit volontairement (l'original
// lapTimeToMs() avait ete retire en meme temps que LapDetail, cf. note
// ci-dessus), mais cantonne a des variables scalaires (long/float), sans
// jamais toucher a un conteneur qui grossit (vector/LapDetail). Utilise
// uniquement pour reperer le meilleur temps de la session en une passe
// (cf. handleLapTracePage()), donc meme profil memoire que le reste des
// fonctions de ce fichier deja jugees saines (parsing ligne par ligne,
// aucune accumulation).
static long lapTimeToMsSimple(const String& t) {
  int colon = t.indexOf(':');
  if (colon < 0) return -1;
  long minutes = t.substring(0, colon).toInt();
  float seconds = t.substring(colon + 1).toFloat();
  if (seconds < 0) return -1;
  return minutes * 60000L + (long)(seconds * 1000.0f + 0.5f);
}

// Extrait "AAAAMMJJHHMMSS" depuis "/log_AAAAMMJJ_HHMMSS.csv"
static String compactKeyFromLogFilename(const String& filename) {
  return stripSeparators(filename.substring(5, filename.length() - 4)); // retire "/log_" et ".csv"
}

// "AAAAMMJJ" -> "JJ/MM/AAAA" pour l'affichage groupe par date
static String prettyDate(const String& compactDate8) {
  if (compactDate8.length() < 8) return compactDate8;
  return compactDate8.substring(6, 8) + "/" + compactDate8.substring(4, 6) + "/" + compactDate8.substring(0, 4);
}

// "HHMMSS" -> "HH:MM:SS"
static String prettyTime(const String& compactTime6) {
  if (compactTime6.length() < 6) return compactTime6;
  return compactTime6.substring(0, 2) + ":" + compactTime6.substring(2, 4) + ":" + compactTime6.substring(4, 6);
}

// Retire de /sessions.csv le bloc correspondant a UNE session precise
// (delimite par ses marqueurs "# session demarree"/"# session arretee"),
// sans toucher a aucune autre session du carnet. Appele automatiquement
// quand on supprime le log GPS detaille correspondant (cf.
// handleDeleteRequest()) -- sinon la session "polluante" (sans tour, cf.
// discussion) disparait bien de la page web mais reste visible dans le
// menu OLED, qui se base UNIQUEMENT sur ce carnet, jamais sur la
// presence ou non d'un fichier log_*.csv. LittleFS ne permet pas de
// retirer des lignes en place -- on relit le fichier entier et on le
// reecrit sans le bloc vise, meme prudence que saveAllCircuits().
//
// Cas durci : coupure d'alimentation en plein enregistrement -- la
// session visee n'a alors jamais recu son marqueur "# session arretee"
// (le suivant dans le fichier appartient a la session D'APRES). Des
// qu'on retombe sur un nouveau "# session demarree" (n'importe lequel)
// pendant qu'on saute un bloc, on considere que ca cloture implicitement
// le bloc tronque, AVANT de regarder si ce nouveau marqueur correspond a
// autre chose -- sans quoi la session suivante (potentiellement complete)
// serait avalee avec.
static void pruneSessionFromCarnet(const String& targetCompactKey) {
  File f = LittleFS.open(g_sessionLogPath, "r");
  if (!f) return;

  std::vector<String> keptLines;
  bool skipping = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    bool isStartMarker = line.startsWith("# session demarree");
    bool isStopMarker = line.startsWith("# session arretee");

    if (skipping && isStartMarker) {
      skipping = false; // fin implicite du bloc tronque -- cette ligne appartient a la session suivante, traitee normalement ci-dessous
    }

    if (isStartMarker) {
      String rest = line.substring(line.indexOf("demarree") + 9);
      if (stripSeparators(rest) == targetCompactKey) {
        skipping = true;
        continue; // marqueur de debut du bloc vise -- pas conserve
      }
    }

    if (skipping) {
      if (isStopMarker) skipping = false; // fin normale du bloc -- egalement pas conservee
      continue;
    }

    keptLines.push_back(line);
  }
  f.close();

  File out = LittleFS.open(g_sessionLogPath, "w");
  if (!out) return;
  for (const String& l : keptLines) out.println(l);
  out.close();
}

// ===================== Cosmetique partagee (nav + style) =====================

static String pageHeader(const char* activeTab) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Chrono GPS moto piste</title><style>";
  html += "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#11161c;color:#e8eaed;margin:0;padding:0 16px 24px}";
  html += "h2{font-weight:600;padding-top:16px}";
  html += "h3{font-weight:600;opacity:0.85;margin:18px 0 6px}";
  html += "nav{display:flex;gap:8px;margin:12px 0;flex-wrap:wrap}";
  html += "nav a{padding:8px 14px;border-radius:8px;background:#1d242c;color:#e8eaed;text-decoration:none;font-size:14px}";
  html += "nav a.active{background:#3a7bd5;color:#fff}";
  html += ".card{background:#1d242c;border-radius:10px;padding:12px 16px;margin:8px 0;display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}";
  html += ".card .meta{font-size:13px;opacity:0.8;margin-top:2px}";
  html += ".big{font-size:28px;font-weight:700}";
  html += ".pill{display:inline-block;padding:3px 10px;border-radius:999px;font-size:12px;font-weight:600}";
  html += ".ok{background:#1e4620;color:#7ee787}";
  html += ".bad{background:#4a1f1f;color:#ff8585}";
  html += "a.file{color:#7eb6ff;text-decoration:none;font-weight:600}";
  html += "a.btn{display:inline-block;background:#3a7bd5;color:#fff;padding:8px 14px;border-radius:8px;text-decoration:none;font-weight:600;font-size:14px}";
  html += "a.export{color:#9aa4b2;text-decoration:none;font-size:13px;opacity:0.85}";
  html += "a.export:hover{opacity:1}";
  html += "button{padding:7px 14px;border-radius:8px;border:none;font-size:13px;font-weight:600;cursor:pointer;background:#b62324;color:#fff}";
  html += "form{margin:0}";
  html += "</style></head><body>";
  html += "<nav>";
  html += "<a href='/'"; if (strcmp(activeTab, "home") == 0) html += " class='active'"; html += ">Sessions</a>";
  html += "<a href='/circuits'"; if (strcmp(activeTab, "circuits") == 0) html += " class='active'"; html += ">Circuits</a>";
  html += "<a href='/status'"; if (strcmp(activeTab, "status") == 0) html += " class='active'"; html += ">Statut</a>";
  html += "<a href='/import'"; if (strcmp(activeTab, "import") == 0) html += " class='active'"; html += ">Import</a>";
  html += "<a href='/update'"; if (strcmp(activeTab, "firmware") == 0) html += " class='active'"; html += ">Firmware</a>";
  html += "</nav>";
  return html;
}

static const char* PAGE_FOOTER = "</body></html>";

// ===================== Circuits (/circuits) -- lecture/ecriture complete de circuits.csv =====================
//
// Contrairement au resume de sessions (lecture seule ici), cette page a
// besoin d'ecrire circuits.csv -- c'est le SEUL endroit du firmware qui
// le modifie. main.cpp le relit independamment au demarrage pour
// construire CourseManager (cf. loadActiveCircuitsIntoTracks() la-bas),
// meme decouplage que pour les sessions -- chaque module parse le fichier
// a sa facon, pour son propre besoin.
//
// Format CSV (identique cote main.cpp pour les 17 premiers champs, cf.
// commentaire la-bas -- main.cpp n'a pas besoin de connaitre "locked",
// il s'arrete a 17 champs et ignore le reste). "locked" ajoute en 18e
// position : proteger un circuit contre une suppression accidentelle
// (une simple case a cocher, verifiee cote handleCircuitDelete()).
// Retrocompatible : un fichier ecrit avant l'ajout de cette colonne
// (17 champs) reste lisible, "locked" vaut simplement false dans ce cas
// (cf. loadAllCircuits() plus bas).
// active,name,length_ft,sa_lat,sa_lng,sb_lat,sb_lng,has2,s2a_lat,s2a_lng,s2b_lat,s2b_lng,has3,s3a_lat,s3a_lng,s3b_lat,s3b_lng,locked

struct CircuitEntry {
  bool active = false;
  String name;
  float lengthFt = 0; // stockage interne en pieds (ce qu'attend CourseConfig cote lib) -- converti en mètres uniquement a l'affichage/saisie (cf. FT_PER_METER)
  double saLat = 0, saLng = 0, sbLat = 0, sbLng = 0;
  bool has2 = false;
  double s2aLat = 0, s2aLng = 0, s2bLat = 0, s2bLng = 0;
  bool has3 = false;
  double s3aLat = 0, s3aLng = 0, s3bLat = 0, s3bLng = 0;
  bool locked = false; // protege contre la suppression accidentelle depuis la page web
};

static const char* CIRCUITS_CSV_HEADER =
  "active,name,length_ft,sa_lat,sa_lng,sb_lat,sb_lng,has2,s2a_lat,s2a_lng,s2b_lat,s2b_lng,has3,s3a_lat,s3a_lng,s3b_lat,s3b_lng,locked";

// 1 pied = 0.3048 m exactement (definition legale du pied international) --
// FT_PER_METER = 1/0.3048. La page web affiche/saisit des metres (plus
// naturel pour toi), le CSV et la lib DovesLapTimer restent en pieds --
// conversion faite uniquement ici, aux deux points d'entree/sortie
// (formulaire d'edition + liste), jamais dans le fichier stocke.
static const float FT_PER_METER = 3.28083989501f;

// Coupe une ligne CSV en champs -- generique, jusqu'a maxFields (meme
// logique que splitCsvLine() cote main.cpp, dupliquee volontairement --
// cf. note de decouplage en tete de section).
static int splitCsvLineWS(const String& line, String fields[], int maxFields) {
  int count = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && count < maxFields; i++) {
    if (i == (int)line.length() || line[i] == ',') {
      fields[count++] = line.substring(start, i);
      start = i + 1;
    }
  }
  return count;
}

static std::vector<CircuitEntry> loadAllCircuits() {
  std::vector<CircuitEntry> result;
  File f = LittleFS.open(g_circuitsFilePath, "r");
  if (!f) return result;

  bool firstLine = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (firstLine) { firstLine = false; continue; }

    String fld[18];
    int n = splitCsvLineWS(line, fld, 18);
    if (n < 17) continue; // ligne malformee -- ignoree

    CircuitEntry e;
    e.active = fld[0].toInt() == 1;
    e.name = fld[1];
    e.lengthFt = fld[2].toFloat();
    e.saLat = atof(fld[3].c_str());  e.saLng = atof(fld[4].c_str());
    e.sbLat = atof(fld[5].c_str());  e.sbLng = atof(fld[6].c_str());
    e.has2 = fld[7].toInt() == 1;
    e.s2aLat = atof(fld[8].c_str());  e.s2aLng = atof(fld[9].c_str());
    e.s2bLat = atof(fld[10].c_str()); e.s2bLng = atof(fld[11].c_str());
    e.has3 = fld[12].toInt() == 1;
    e.s3aLat = atof(fld[13].c_str()); e.s3aLng = atof(fld[14].c_str());
    e.s3bLat = atof(fld[15].c_str()); e.s3bLng = atof(fld[16].c_str());
    e.locked = (n >= 18) ? (fld[17].toInt() == 1) : false; // absent (fichier ecrit avant l'ajout de cette colonne) -- pas protege par defaut
    result.push_back(e);
  }
  f.close();
  return result;
}

static void saveAllCircuits(const std::vector<CircuitEntry>& circuits) {
  File f = LittleFS.open(g_circuitsFilePath, "w");
  if (!f) { Serial.println("Circuits: impossible d'ecrire circuits.csv."); return; }
  f.println(CIRCUITS_CSV_HEADER);
  for (const CircuitEntry& e : circuits) {
    f.printf("%d,%s,%.1f,%.7f,%.7f,%.7f,%.7f,%d,%.7f,%.7f,%.7f,%.7f,%d,%.7f,%.7f,%.7f,%.7f,%d\n",
              e.active ? 1 : 0, e.name.c_str(), e.lengthFt, e.saLat, e.saLng, e.sbLat, e.sbLng,
              e.has2 ? 1 : 0, e.s2aLat, e.s2aLng, e.s2bLat, e.s2bLng,
              e.has3 ? 1 : 0, e.s3aLat, e.s3aLng, e.s3bLat, e.s3bLng,
              e.locked ? 1 : 0);
  }
  f.close();
}

static int countActiveCircuits(const std::vector<CircuitEntry>& circuits) {
  int n = 0;
  for (const CircuitEntry& e : circuits) if (e.active) n++;
  return n;
}

// Redemarre l'ESP apres une modification d'un fichier garde ouvert en
// continu par main.cpp (circuits.csv rechargeable a chaud en theorie,
// mais sessions.csv est carrement garde ouvert en mode ajout depuis
// setup() -- cf. discussion sur le nettoyage du carnet). Plus robuste
// qu'un rechargement a chaud pendant qu'une session pourrait etre active
// (meme logique que la fin d'un flash OTA, cf. handleUpdateResult()).
// Repond au navigateur avant de couper.
static void restartToApplyChange(const char* activeTab, const String& message) {
  String html = pageHeader(activeTab);
  html += "<h2>" + message + "</h2>";
  html += "<div class='card' style='display:block'>Redemarrage en cours pour appliquer le changement...</div>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
  httpServer.client().setNoDelay(true);
  delay(500);
  ESP.restart();
}

static void restartAfterCircuitChange(const String& message) {
  restartToApplyChange("circuits", message);
}

// ----- Page "/circuits" -- liste, avec case Actif par circuit -----

static void handleCircuitsListPage() {
  std::vector<CircuitEntry> circuits = loadAllCircuits();
  int activeCount = countActiveCircuits(circuits);

  String html = pageHeader("circuits");
  html += "<h2>Circuits</h2>";

  html += "<div class='card' style='display:block;font-size:13px'>";
  html += String(activeCount) + " / " + String(MAX_COURSES) + " circuits actifs (detectes automatiquement au demarrage). ";
  html += "Coche seulement ceux prevus pour la sortie du jour -- le reste dort ici, reactivable a tout moment.</div>";

  html += "<p><a class='btn' href='/circuits/edit?idx=-1'>+ Nouveau circuit</a></p>";
  html += "<p><a class='export' href='/download?file=circuits.csv'>&#8681; Exporter circuits.csv</a></p>";
  html += "<div class='card' style='display:block;font-size:12px;opacity:0.7'>Pour migrer vers une nouvelle carte : exporte ce fichier ci-dessus, place-le dans data/circuits.csv du nouveau firmware, <code>pio run -t uploadfs</code>.</div>";

  if (circuits.empty()) {
    html += "<p><i>Aucun circuit enregistre.</i></p>";
  }

  for (size_t i = 0; i < circuits.size(); i++) {
    const CircuitEntry& e = circuits[i];
    html += "<div class='card'><div>";
    html += "<b>" + e.name + "</b>";
    if (e.locked) html += " <span class='pill ok' style='font-size:10px'>Protege</span>";
    html += "<div class='meta'>" + String(e.lengthFt / FT_PER_METER, 0) + " m";
    if (e.has2) html += " -- secteur 2";
    if (e.has3) html += " -- secteur 3";
    html += "</div></div>";
    html += "<div style='display:flex;gap:8px;align-items:center'>";

    html += "<form action='/circuits/toggle' method='GET'>";
    html += "<input type='hidden' name='idx' value='" + String((int)i) + "'>";
    html += "<button type='submit' style='background:" + String(e.active ? "#238636" : "#3a3f47") + "'>";
    html += e.active ? "Actif" : "Inactif";
    html += "</button></form>";

    html += "<a class='file' href='/circuits/edit?idx=" + String((int)i) + "'>Modifier</a>";

    if (e.locked) {
      // Bouton grise plutot que masque -- visible que le circuit existe
      // et pourquoi on ne peut pas le supprimer directement (decoche
      // "Protege" dans Modifier si tu veux vraiment le faire).
      html += "<button disabled title=\"Decoche 'Protege' dans Modifier pour pouvoir supprimer\" style='background:#3a3f47;opacity:0.6;cursor:not-allowed'>Supprimer</button>";
    } else {
      html += "<form action='/circuits/delete' method='GET' onsubmit=\"return confirm('Supprimer ce circuit ? Irreversible.');\">";
      html += "<input type='hidden' name='idx' value='" + String((int)i) + "'>";
      html += "<button type='submit'>Supprimer</button></form>";
    }

    html += "</div></div>";
  }

  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

static void handleCircuitToggle() {
  int idx = httpServer.arg("idx").toInt();
  std::vector<CircuitEntry> circuits = loadAllCircuits();
  if (idx < 0 || idx >= (int)circuits.size()) { httpServer.sendHeader("Location", "/circuits"); httpServer.send(302, "text/plain", ""); return; }

  if (!circuits[idx].active && countActiveCircuits(circuits) >= MAX_COURSES) {
    // Blocage cote web -- la lib DovesLapTimer ne gere pas plus de
    // MAX_COURSES circuits en detection simultanee (cf. main.cpp).
    String html = pageHeader("circuits");
    html += "<h2>Deja " + String(MAX_COURSES) + " circuits actifs</h2>";
    html += "<div class='card' style='display:block'>Decoche-en un avant d'en activer un nouveau -- la detection automatique ne peut pas gerer plus de " + String(MAX_COURSES) + " circuits a la fois.</div>";
    html += "<p><a class='file' href='/circuits'>Retour</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }

  circuits[idx].active = !circuits[idx].active;
  saveAllCircuits(circuits);
  restartAfterCircuitChange(circuits[idx].active ? "Circuit active" : "Circuit desactive");
}

static void handleCircuitDelete() {
  int idx = httpServer.arg("idx").toInt();
  std::vector<CircuitEntry> circuits = loadAllCircuits();
  if (idx < 0 || idx >= (int)circuits.size()) { httpServer.sendHeader("Location", "/circuits"); httpServer.send(302, "text/plain", ""); return; }

  if (circuits[idx].locked) {
    // Cote serveur, pas juste le bouton grise cote HTML -- une requete
    // GET /circuits/delete?idx=N construite a la main (ou un vieux
    // signet) ne doit pas pouvoir contourner la protection.
    String html = pageHeader("circuits");
    html += "<h2>Circuit protege</h2>";
    html += "<div class='card' style='display:block'>\"" + circuits[idx].name + "\" est marque protege contre la suppression. Decoche \"Protege\" depuis Modifier si tu veux vraiment le supprimer.</div>";
    html += "<p><a class='file' href='/circuits'>Retour</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }

  bool wasActive = circuits[idx].active;
  circuits.erase(circuits.begin() + idx);
  saveAllCircuits(circuits);
  if (wasActive) restartAfterCircuitChange("Circuit supprime"); // affectait la detection -- redemarrage necessaire
  else { httpServer.sendHeader("Location", "/circuits"); httpServer.send(302, "text/plain", ""); } // inactif -- aucun impact sur CourseManager, pas besoin de redemarrer
}

// ----- Page "/circuits/edit" -- formulaire (nouveau si idx=-1, edition sinon) -----

// Petit champ lat/lng avec bouton "Capturer" -- rempli en JS via
// fetch('/gps-position') (le GPS tourne aussi pendant le WiFi, cf. notes
// GPS cable dans main.cpp -- plus de coexistence radio a arbitrer).
static String latLngFieldPair(const char* label, const char* latName, const char* lngName, double latVal, double lngVal) {
  String html = "<div style='margin:10px 0'><div style='font-size:13px;opacity:0.8;margin-bottom:4px'>" + String(label) + "</div>";
  html += "<div style='display:flex;gap:6px;flex-wrap:wrap;align-items:center'>";
  html += "<input type='text' name='" + String(latName) + "' id='" + String(latName) + "' value='" + String(latVal, 7) + "' placeholder='latitude' style='background:#1d242c;color:#e8eaed;border:1px solid #2a323c;border-radius:6px;padding:6px 8px;font-size:13px;width:140px'>";
  html += "<input type='text' name='" + String(lngName) + "' id='" + String(lngName) + "' value='" + String(lngVal, 7) + "' placeholder='longitude' style='background:#1d242c;color:#e8eaed;border:1px solid #2a323c;border-radius:6px;padding:6px 8px;font-size:13px;width:140px'>";
  html += "<button type='button' onclick=\"capturePosition('" + String(latName) + "','" + String(lngName) + "')\" style='background:#3a7bd5'>Capturer position GPS</button>";
  html += "</div></div>";
  return html;
}

static void handleCircuitEditPage() {
  int idx = httpServer.arg("idx").toInt();
  std::vector<CircuitEntry> circuits = loadAllCircuits();

  CircuitEntry e; // valeurs par defaut (0/vide) si idx=-1, nouveau circuit
  bool isNew = (idx < 0 || idx >= (int)circuits.size());
  if (!isNew) e = circuits[idx];

  String html = pageHeader("circuits");
  html += "<h2>" + String(isNew ? "Nouveau circuit" : "Modifier : " + e.name) + "</h2>";

  html += "<form action='/circuits/save' method='POST'>";
  html += "<input type='hidden' name='idx' value='" + String(idx) + "'>";

  html += "<div style='margin:10px 0'><label style='font-size:13px;opacity:0.8'>Nom</label><br>";
  html += "<input type='text' name='name' value='" + e.name + "' required style='background:#1d242c;color:#e8eaed;border:1px solid #2a323c;border-radius:6px;padding:7px 10px;font-size:14px;width:100%;max-width:300px'></div>";

  html += "<div style='margin:10px 0'><label style='font-size:13px;opacity:0.8'>Longueur (m)</label><br>";
  html += "<input type='text' name='length_m' value='" + String(e.lengthFt / FT_PER_METER, 1) + "' style='background:#1d242c;color:#e8eaed;border:1px solid #2a323c;border-radius:6px;padding:7px 10px;font-size:14px;width:140px'></div>";

  html += "<div style='margin:10px 0'><label><input type='checkbox' name='active' " + String(e.active ? "checked" : "") + "> Actif (charge au demarrage)</label></div>";
  html += "<div style='margin:10px 0'><label><input type='checkbox' name='locked' " + String(e.locked ? "checked" : "") + "> Protege contre la suppression accidentelle</label></div>";

  html += "<h3>Ligne depart/arrivee</h3>";
  html += latLngFieldPair("Point A", "sa_lat", "sa_lng", e.saLat, e.saLng);
  html += latLngFieldPair("Point B", "sb_lat", "sb_lng", e.sbLat, e.sbLng);

  html += "<h3>Secteur 2 (optionnel)</h3>";
  html += "<div style='margin:6px 0'><label><input type='checkbox' name='has2' id='has2' " + String(e.has2 ? "checked" : "") + "> Activer le secteur 2</label></div>";
  html += latLngFieldPair("Point A", "s2a_lat", "s2a_lng", e.s2aLat, e.s2aLng);
  html += latLngFieldPair("Point B", "s2b_lat", "s2b_lng", e.s2bLat, e.s2bLng);

  html += "<h3>Secteur 3 (optionnel)</h3>";
  html += "<div style='margin:6px 0'><label><input type='checkbox' name='has3' id='has3' " + String(e.has3 ? "checked" : "") + "> Activer le secteur 3</label></div>";
  html += latLngFieldPair("Point A", "s3a_lat", "s3a_lng", e.s3aLat, e.s3aLng);
  html += latLngFieldPair("Point B", "s3b_lat", "s3b_lng", e.s3bLat, e.s3bLng);

  html += "<p style='margin-top:18px'><button type='submit' style='background:#3a7bd5'>Enregistrer</button> ";
  html += "<a class='file' href='/circuits' style='margin-left:10px'>Annuler</a></p>";
  html += "</form>";

  html += R"JS(<script>
async function capturePosition(latId, lngId) {
  try {
    const r = await fetch('/gps-position');
    const txt = await r.text();
    const parts = txt.split(',');
    if (parts[0] !== 'ok') { alert('Pas de fix GPS pour le moment -- verifie que le module capte bien le ciel.'); return; }
    document.getElementById(latId).value = parts[1];
    document.getElementById(lngId).value = parts[2];
  } catch (err) {
    alert('Erreur de lecture GPS.');
  }
}
</script>)JS";

  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

static void handleCircuitSave() {
  int idx = httpServer.arg("idx").toInt();
  std::vector<CircuitEntry> circuits = loadAllCircuits();
  bool isNew = (idx < 0 || idx >= (int)circuits.size());

  CircuitEntry e;
  e.name = httpServer.arg("name");
  e.name.replace(",", ""); e.name.replace("\n", ""); e.name.replace("\r", ""); // securite format CSV -- une virgule dans le nom decalerait tous les champs suivants
  e.name.trim();
  e.lengthFt = httpServer.arg("length_m").toFloat() * FT_PER_METER; // formulaire en metres -- stockage interne en pieds (cf. commentaire FT_PER_METER)
  e.active = httpServer.hasArg("active");
  e.locked = httpServer.hasArg("locked");
  e.saLat = atof(httpServer.arg("sa_lat").c_str());  e.saLng = atof(httpServer.arg("sa_lng").c_str());
  e.sbLat = atof(httpServer.arg("sb_lat").c_str());  e.sbLng = atof(httpServer.arg("sb_lng").c_str());
  e.has2 = httpServer.hasArg("has2");
  e.s2aLat = atof(httpServer.arg("s2a_lat").c_str()); e.s2aLng = atof(httpServer.arg("s2a_lng").c_str());
  e.s2bLat = atof(httpServer.arg("s2b_lat").c_str()); e.s2bLng = atof(httpServer.arg("s2b_lng").c_str());
  e.has3 = httpServer.hasArg("has3");
  e.s3aLat = atof(httpServer.arg("s3a_lat").c_str()); e.s3aLng = atof(httpServer.arg("s3a_lng").c_str());
  e.s3bLat = atof(httpServer.arg("s3b_lat").c_str()); e.s3bLng = atof(httpServer.arg("s3b_lng").c_str());

  if (e.name.length() == 0) { httpServer.send(400, "text/plain", "Nom requis."); return; }

  // Limite 8 actifs max -- meme regle que le toggle rapide depuis la
  // liste (cf. handleCircuitToggle()), ici pour le cas ou on coche
  // "Actif" sur un circuit qui ne l'etait pas encore via ce formulaire.
  // IMPORTANT : on ne rejette plus tout le formulaire dans ce cas (ca
  // faisait perdre toutes les coordonnees tout juste saisies, en
  // particulier genant pour un nouveau circuit) -- le circuit est
  // enregistre quand meme, juste force en inactif, avec un message clair.
  bool wasActive = isNew ? false : circuits[idx].active;
  bool blockedFromActivating = false;
  if (e.active && !wasActive) {
    int activeWithoutThis = countActiveCircuits(circuits) - (wasActive ? 1 : 0);
    if (activeWithoutThis >= MAX_COURSES) {
      blockedFromActivating = true;
      e.active = false;
    }
  }

  bool affectsDetection; // determine si un redemarrage est necessaire
  if (isNew) {
    circuits.push_back(e);
    affectsDetection = e.active;
  } else {
    affectsDetection = wasActive || e.active; // actif avant, apres, ou les deux -- coordonnees potentiellement changees
    circuits[idx] = e;
  }
  saveAllCircuits(circuits);

  if (blockedFromActivating) {
    // Pas de redemarrage necessaire ici -- le circuit reste inactif,
    // donc CourseManager n'est pas concerne par ce changement.
    String html = pageHeader("circuits");
    html += "<h2>Circuit enregistre, mais pas active</h2>";
    html += "<div class='card' style='display:block'>Deja " + String(MAX_COURSES) + " circuits actifs -- \"" + e.name + "\" a bien ete sauvegarde (coordonnees comprises), mais reste inactif. Decoche-en un autre puis reviens l'activer depuis la liste.</div>";
    html += "<p><a class='file' href='/circuits'>Retour a la liste</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }

  if (affectsDetection) restartAfterCircuitChange("Circuit enregistre");
  else { httpServer.sendHeader("Location", "/circuits"); httpServer.send(302, "text/plain", ""); }
}

// ----- "/gps-position" -- position live, pour le bouton "Capturer" en JS -----

static void handleGpsPositionRequest() {
  WebServerStatusInfo s;
  if (g_getStatus) s = g_getStatus();
  if (!s.hasGpsFix) { httpServer.send(200, "text/plain", "no-fix"); return; }
  httpServer.send(200, "text/plain", "ok," + String(s.latitude, 7) + "," + String(s.longitude, 7));
}

// ===================== Page "/" -- gestion des sessions enregistrees =====================

// Barre "espace disponible" generique, reutilisee pour LittleFS et pour le
// filesystem des logs GPS (SD, quand elle est active -- cf. handleHomePage()).
// uint64_t plutot que size_t : une carte SD peut largement depasser les
// ~4 Go plafond d'un size_t 32 bits (LittleFS.usedBytes()/totalBytes(),
// bien plus petits, se convertissent sans souci vers ce type plus large).
static String storageBarHtml(const char* label, uint64_t used, uint64_t total) {
  int pctUsed = (total > 0) ? (int)(used * 100 / total) : 0;
  int pctFree = 100 - pctUsed;
  String barColor = pctUsed < 70 ? "#238636" : (pctUsed < 90 ? "#BB8009" : "#B62324");
  String html = "<div class='card' style='display:block'>";
  html += "<div style='font-size:13px;margin-bottom:4px'>" + String(label) + " : <b>" + String(pctFree) + "%</b> libres (" + String((uint32_t)((total - used) / 1024)) + " Ko libres / " + String((uint32_t)(total / 1024)) + " Ko)</div>";
  html += "<div style='background:#2a323c;border-radius:4px;height:8px;overflow:hidden'><div style='background:" + barColor + ";width:" + String(pctUsed) + "%;height:8px'></div></div>";
  html += "</div>";
  return html;
}

static void handleHomePage() {
  String html = pageHeader("home");
  html += "<h2>Sessions enregistrees</h2>";

  html += storageBarHtml("LittleFS (config, carnet)", LittleFS.usedBytes(), LittleFS.totalBytes());

  // Barre SD -- affichee seulement si une carte est reellement active
  // (g_getStatus->hasSeparateLogsFs) : pas de raison d'afficher une 2e
  // barre redondante quand g_logsFs est en repli sur LittleFS (deja
  // couverte par la barre ci-dessus). usedBytes()/totalBytes() ne font
  // pas partie de l'interface commune fs::FS -- main.cpp (qui connait le
  // type concret) les fournit via WebServerStatusInfo plutot que de les
  // lire ici a travers g_logsFs (fs::FS* generique).
  {
    WebServerStatusInfo s;
    if (g_getStatus) s = g_getStatus();
    if (s.hasSeparateLogsFs) {
      html += storageBarHtml(s.logsFsLabel.c_str(), s.logsFsUsedBytes, s.logsFsTotalBytes);
    }
  }

  html += "<div class='card'><div><b>Carnet de session</b><div class='meta'>Resume par tour, cumulatif (toutes les journees)</div></div>";
  html += "<a class='file' href='/download?file=" + g_sessionLogPath.substring(1) + "'>Telecharger</a></div>";

  html += "<p><a class='export' href='/backup'>&#8681; Sauvegarde complete (.zip -- carnet + tous les logs GPS detailles)</a></p>";

  // ----- A partir d'ici : envoi en streaming (chunked), pas d'accumulation -----
  //
  // Avec un vrai volume de sessions/tours (constate au banc : crash
  // LoadProhibited dans String::indexOf(), cf. lapTimeToMs(), en aval
  // d'un echec d'allocation silencieux quelque part dans cette fonction),
  // accumuler TOUTE la page (potentiellement plusieurs dizaines de Ko --
  // une carte + un tableau de tours + un graphique SVG par session) dans
  // une seule String faisait exploser le tas, deja reduit par
  // WiFi.softAP() (~70Ko libres seulement). Fix : envoyer chaque section
  // au fur et a mesure (httpServer.sendContent()) plutot que d'attendre
  // la fin -- `html` ne contient plus jamais qu'un morceau borne, jamais
  // la page entiere.
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "text/html", "");
  httpServer.sendContent(html);
  html = "";

  std::vector<SessionSummaryLite> summaries = loadSessionSummaries();

  // Recupere tous les fichiers GPS detailles, tries du plus recent au plus ancien
  // (les noms "log_AAAAMMJJ_HHMMSS.csv" se comparent dans le bon ordre en texte).
  std::vector<std::pair<String, uint32_t>> files;
  forEachGpsLogFile([&](const String& name, uint32_t size) { files.push_back({name, size}); });
  std::sort(files.begin(), files.end(), [](const std::pair<String,uint32_t>& a, const std::pair<String,uint32_t>& b) {
    return a.first > b.first; // ordre decroissant -- plus recent en premier
  });

  if (files.empty()) {
    html += "<p><i>(aucune session GPS enregistree pour l'instant)</i></p>";
  } else {
    String lastDateShown = "";
    for (size_t i = 0; i < files.size(); i++) {
      String compactKey = compactKeyFromLogFilename(files[i].first);
      String dateCompact8 = compactKey.substring(0, 8);
      String timeCompact6 = compactKey.length() >= 14 ? compactKey.substring(8, 14) : "";

      if (dateCompact8 != lastDateShown) {
        html += "<h3>" + prettyDate(dateCompact8) + "</h3>";
        lastDateShown = dateCompact8;
      }

      SessionSummaryLite summary = findSummaryForFile(summaries, compactKey);
      String shortName = files[i].first.substring(1);

      html += "<div class='card' style='display:block'><div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px'><div>";
      html += "<b>" + prettyTime(timeCompact6) + "</b>";
      html += "<div class='meta'>";
      if (summary.lapCount > 0) {
        if (isRouteSummary(summary)) {
          html += "Route -- duree " + summary.bestLapTime;
          if (summary.lastDistanceKm.length()) html += " -- " + summary.lastDistanceKm + " km";
          if (summary.lastAvgSpeedKmh.length()) html += " -- V.moy " + summary.lastAvgSpeedKmh + " km/h";
        } else {
          html += String(summary.lapCount) + " tour(s) -- meilleur : " + summary.bestLapTime;
          if (summary.circuit.length()) html += " -- " + summary.circuit;
        }
      } else {
        html += "(pas de resume disponible)";
      }
      html += " -- " + String(files[i].second) + " octets</div></div>";
      html += "<div style='display:flex;gap:8px'>";
      html += "<a class='file' href='/download?file=" + shortName + "'>Telecharger</a>";
      html += "<form action='/delete' method='GET' onsubmit=\"return confirm('Supprimer cette session ? Irreversible.');\">";
      html += "<input type='hidden' name='file' value='" + shortName + "'>";
      html += "<button type='submit'>Supprimer</button></form>";
      html += "</div></div>";

      // ----- Tableau par tour + graphique -- DESACTIVE TEMPORAIREMENT -----
      //
      // Plante de facon reproductible sur certaines sessions reelles
      // (constate au banc : String corrompue en retour de
      // loadLapsForSession(), cause exacte non identifiee malgre plusieurs
      // pistes eliminees -- pile agrandie sans effet, format CSV verifie
      // sain a l'octet pres. Necessiterait un vrai debogueur JTAG/GDB pour
      // etre isole precisement, pas fait a ce stade). En attendant : la
      // page d'accueil n'affiche plus que le resume (deja disponible via
      // loadSessionSummaries(), jamais implique dans le plantage) -- le
      // detail tour par tour reste consultable via la page /lap dediee.
      // Lien vers /lap -- desormais protege par la quarantaine dans
      // loadLapsForSession() (cf. cette fonction) pour la session
      // precise qui declenchait le bug memoire non resolu.
      if (summary.lapCount > 0) {
        String linkLabel = isRouteSummary(summary) ? "Voir le detail du parcours &rarr;" : "Voir le detail des tours &rarr;";
        html += "<p style='margin-top:8px'><a class='file' href='/lap?file=" + shortName + "&lap=1'>" + linkLabel + "</a></p>";
      }

      html += "</div>";

      // Fin de traitement de cette session -- on envoie ce qu'on a
      // accumule pour elle et on repart d'une String vide pour la
      // suivante, plutot que de laisser grossir indefiniment.
      httpServer.sendContent(html);
      html = "";
    }
  }

  html += PAGE_FOOTER;
  httpServer.sendContent(html);
}

static bool isAllowedGpsLogFile(const String& file) {
  return file.indexOf("/log_") >= 0 && file.endsWith(".csv");
}

static void handleDownloadRequest() {
  String file = httpServer.arg("file");
  if (!file.startsWith("/")) file = "/" + file;

  bool isGpsLog = isAllowedGpsLogFile(file);
  bool isSessions = (file == g_sessionLogPath);
  bool isCircuits = (file == g_circuitsFilePath); // export pour migration vers une nouvelle carte (cf. lien sur /circuits)
  if (!isGpsLog && !isSessions && !isCircuits) { httpServer.send(403, "text/plain", "Fichier non autorise."); return; }

  if (g_flushLogs) g_flushLogs(); // flush inconditionnel, simple et sans risque -- evite d'avoir a connaitre quel fichier est actuellement ouvert cote main.cpp

  String downloadName = file.substring(1);
  fs::FS& fsRef = isGpsLog ? *g_logsFs : LittleFS;
  serveFile(fsRef, file.c_str(), downloadName.c_str());
}

static void handleDeleteRequest() {
  String file = httpServer.arg("file");
  if (!file.startsWith("/")) file = "/" + file;

  // Volontairement plus restrictif que le telechargement : on ne permet
  // JAMAIS de supprimer le carnet de session cumulatif EN ENTIER depuis
  // cette page (perte de tout l'historique) -- seulement les fichiers
  // GPS detailles individuels, un par un. Toujours sur g_logsFs (SD ou
  // repli LittleFS). On retire cela dit le bloc correspondant du carnet
  // (cf. pruneSessionFromCarnet()) pour que la session disparaisse aussi
  // du menu OLED, qui ne connait que ce carnet -- sans quoi une session
  // "polluante" supprimee ici reste fantome sur le chrono.
  if (!isAllowedGpsLogFile(file)) { httpServer.send(403, "text/plain", "Fichier non autorise."); return; }

  g_logsFs->remove(file);
  pruneSessionFromCarnet(compactKeyFromLogFilename(file));
  // Redemarrage necessaire : main.cpp garde sessions.csv ouvert en mode
  // ajout depuis setup() (jamais ferme sauf commande Serial 'x') -- le
  // reecrire depuis ici pendant que ce handle est ouvert desynchronise
  // sa position/taille interne, ce qui corromprait le prochain tour
  // ecrit par le firmware. Le redemarrage force main.cpp a rouvrir un
  // handle propre sur le fichier tel qu'on vient de le laisser.
  restartToApplyChange("home", "Session supprimee");
}


// ===================== Page "/lap" -- trace d'un tour colore par vitesse (style RaceChrono) =====================
//
// Contrairement a /compare (plusieurs tours d'une session superposes,
// couleur unie par tour), cette page isole UN SEUL tour d'UN SEUL log et le
// colore par vitesse (degrade rouge=lent -> vert=rapide, via jaune/orange),
// avec des etiquettes aux points de vitesse locale min/max (freinages et
// relances) -- comme la vue "tour" de RaceChrono. Pas de gestion de
// secteurs/splits (pas de notion equivalente dans ce firmware).
//
// Decoupage du tour : la colonne "laps" du log detaille (nombre de tours
// DEJA valides au moment de la ligne) permet d'isoler un tour precis sans
// avoir a recouper les horodatages avec /sessions.csv -- le tour affiche
// "numero N" correspond aux lignes ou laps == N-1 (on roule encore dessus
// tant que le Nieme tour n'est pas valide).

static String compareFileLabel(const String& shortName) {
  // "log_AAAAMMJJ_HHMMSS.csv" -> "JJ/MM/AAAA HH:MM:SS"
  String key = compactKeyFromLogFilename("/" + shortName);
  if (key.length() < 14) return shortName;
  return prettyDate(key.substring(0, 8)) + " " + prettyTime(key.substring(8, 14));
}

static void handleLapTracePage() {
  String file = httpServer.arg("file");
  int lapNumber = httpServer.arg("lap").toInt();

  if (!isAllowedGpsLogFile("/" + file) || lapNumber < 1) {
    httpServer.send(400, "text/plain", "Parametres invalides.");
    return;
  }

  // ----- Page volontairement simplifiee, MAIS un peu enrichie -----
  //
  // Deux choses retirees suite a une longue investigation (cf. README) :
  // le graphique JS/SVG de tracé, et l'usage de loadLapsForSession()
  // (construction d'un std::vector<LapDetail>, dont la copie/croissance
  // declenchait de facon reproductible une corruption du tas -- vraie
  // cause identifiee, contrairement au JS accuse a tort au debut).
  // Le tableau reste construit en lisant /sessions.csv LIGNE PAR LIGNE et
  // en streamant chaque ligne directement (httpServer.sendContent()) des
  // qu'elle est prete -- jamais de conteneur qui grossit en memoire,
  // jamais de copie de String en vrac cote firmware.
  //
  // Colonnes ajoutees (best/diff) : calculees a partir des seules
  // donnees deja lues de /sessions.csv (petit fichier, lignes courtes) --
  // une 1ere passe scalaire (juste un "long bestMs", pas de conteneur)
  // pour reperer le meilleur temps, puis la 2e passe habituelle pour
  // streamer le tableau. Meme profil memoire qu'avant.
  //
  // Colonnes Depart/Distance/V.max/V.min/V.moy : desormais ecrites
  // DIRECTEMENT par le firmware dans /sessions.csv (11 champs au lieu de
  // 6/7, cf. checkLapCompletion() dans main.cpp) -- affichees telles
  // quelles, sans aucun calcul supplementaire ni requete vers le log GPS
  // detaille. Pour les sessions plus anciennes encore au format 6/7
  // champs (idx<11 ci-dessous), ces colonnes affichent simplement "--" :
  // l'ancien repli par fetch+parsing JS du log detaille a ete retire
  // (plus de <script> du tout sur cette page desormais) pour alleger la
  // page et couper court a tout risque residuel lie a ce pattern.
  String compactKey = compactKeyFromLogFilename("/" + file);

  // ----- Passe 1 : meilleur temps de la session + detection Route -----
  // (scalaire uniquement -- un "long bestMs" et un "bool isRoute", pas
  // de conteneur)
  long bestMs = -1;
  bool isRoute = false;
  {
    File fb = LittleFS.open(g_sessionLogPath, "r");
    if (fb) {
      bool inTarget = false;
      while (fb.available()) {
        String line = fb.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("date,")) continue;
        if (line.startsWith("# session demarree")) {
          String rest = line.substring(line.indexOf("demarree") + 9);
          inTarget = (stripSeparators(rest) == compactKey);
          continue;
        }
        if (line.startsWith("# session arretee")) { inTarget = false; continue; }
        if (!inTarget) continue;

        int commaIdx[6], found = 0;
        for (int i = 0; i < (int)line.length() && found < 6; i++) if (line[i] == ',') commaIdx[found++] = i;
        if (found >= 5) {
          long ms = lapTimeToMsSimple(line.substring(commaIdx[2] + 1, commaIdx[3]));
          if (ms >= 0 && (bestMs < 0 || ms < bestMs)) bestMs = ms;
          String circuit = (found >= 6) ? line.substring(commaIdx[4] + 1, commaIdx[5]) : line.substring(commaIdx[4] + 1);
          if (circuit == "Route") isRoute = true;
        }
      }
      fb.close();
    }
  }

  String html = pageHeader("home");
  html += isRoute ? "<h2>Detail du parcours (mode Route)</h2>" : "<h2>Tours de la session</h2>";
  html += "<p class='meta' style='opacity:0.75'>" + compareFileLabel(file) + " -- <a class='file' href='/'>&larr; retour aux sessions</a></p>";

  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "text/html", "");
  httpServer.sendContent(html);
  html = "";

  html += "<div style='overflow-x:auto'>";
  html += "<table style='width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;white-space:nowrap'>";
  html += "<tr style='opacity:0.7;text-align:left'>";
  if (isRoute) {
    html += "<th style='padding:4px 8px'>Duree</th>";
  } else {
    html += "<th style='padding:4px 8px'>Tour</th><th style='padding:4px 8px'>Temps</th><th style='padding:4px 8px'>Diff</th>";
  }
  html += "<th style='padding:4px 8px'>Depart tour</th><th style='padding:4px 8px'>Distance</th>";
  html += "<th style='padding:4px 8px'>V.max</th><th style='padding:4px 8px'>V.min</th><th style='padding:4px 8px'>V.moy</th>";
  html += "<th style='padding:4px 8px'>Wheelie</th><th style='padding:4px 8px'>Stoppie</th>";
  html += "<th style='padding:4px 8px'>Angle D</th><th style='padding:4px 8px'>Angle G</th>";
  if (!isRoute) html += "<th style='padding:4px 8px'>Circuit</th>";
  html += "</tr>";
  httpServer.sendContent(html);
  html = "";

  File f = LittleFS.open(g_sessionLogPath, "r");
  bool anyLap = false;
  if (f) {
    bool inTarget = false;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0 || line.startsWith("date,")) continue;

      if (line.startsWith("# session demarree")) {
        String rest = line.substring(line.indexOf("demarree") + 9);
        inTarget = (stripSeparators(rest) == compactKey);
        continue;
      }
      if (line.startsWith("# session arretee")) { inTarget = false; continue; }
      if (!inTarget) continue;

      String fld[15];
      int idx = 0, start = 0;
      for (int i = 0; i < (int)line.length() && idx < 15; i++) {
        if (line[i] == ',' || i == (int)line.length() - 1) {
          int end = (line[i] == ',') ? i : i + 1;
          fld[idx++] = line.substring(start, end);
          start = i + 1;
        }
      }
      if (idx >= 6) {
        anyLap = true;
        String n = fld[2];
        long ms = lapTimeToMsSimple(fld[3]);
        bool isBest = (bestMs >= 0 && ms == bestMs);
        String diffText = "--";
        if (isBest) diffText = "0.000";
        else if (ms >= 0 && bestMs >= 0) diffText = "+" + String((ms - bestMs) / 1000.0f, 3);

        bool hasExtended = (idx >= 11); // vmax/vmin/vavg/distance/depart
        bool hasWheelie = (idx >= 13);  // + compteurs wheelie/stoppie
        bool hasAngles = (idx >= 15);   // + angle max droite/gauche

        String row = isBest ? "<tr style='background:#1d3a5c'>" : "<tr>";
        if (isRoute) {
          row += "<td style='padding:4px 8px'><b>" + fld[3] + "</b></td>";
        } else {
          row += "<td style='padding:4px 8px'>" + n + "</td>";
          row += "<td style='padding:4px 8px'><b>" + fld[3] + "</b></td>";
          row += "<td style='padding:4px 8px'>" + diffText + "</td>";
        }
        if (hasExtended) {
          row += "<td style='padding:4px 8px'>" + fld[10] + "</td>";
          row += "<td style='padding:4px 8px'>" + (fld[9].length() ? fld[9] + " km" : "--") + "</td>";
          row += "<td style='padding:4px 8px'>" + (fld[6].length() ? fld[6] + " km/h" : "--") + "</td>";
          row += "<td style='padding:4px 8px'>" + (fld[7].length() ? fld[7] + " km/h" : "--") + "</td>";
          row += "<td style='padding:4px 8px'>" + (fld[8].length() ? fld[8] + " km/h" : "--") + "</td>";
        } else {
          // Session anterieure au 11e champ -- rien a afficher, plus de
          // calcul de repli cote navigateur (retire pour alleger la
          // page : plus de fetch/parsing JS du tout, meme pour ces
          // vieilles sessions).
          row += "<td style='padding:4px 8px'>--</td>";
          row += "<td style='padding:4px 8px'>--</td>";
          row += "<td style='padding:4px 8px'>--</td>";
          row += "<td style='padding:4px 8px'>--</td>";
          row += "<td style='padding:4px 8px'>--</td>";
        }
        row += "<td style='padding:4px 8px'>" + (hasWheelie ? fld[11] : "--") + "</td>";
        row += "<td style='padding:4px 8px'>" + (hasWheelie ? fld[12] : "--") + "</td>";
        row += "<td style='padding:4px 8px'>" + (hasAngles && fld[13].length() ? fld[13] + "&deg;" : "--") + "</td>";
        row += "<td style='padding:4px 8px'>" + (hasAngles && fld[14].length() ? fld[14] + "&deg;" : "--") + "</td>";
        if (!isRoute) row += "<td style='padding:4px 8px'>" + fld[5] + "</td>";
        row += "</tr>";
        httpServer.sendContent(row); // une ligne a la fois -- jamais accumulee
      }
    }
    f.close();
  }

  html = "</table></div>";
  if (!anyLap) html += isRoute ? "<p><i>Aucune donnee trouvee pour ce parcours.</i></p>" : "<p><i>Aucun tour trouve pour cette session.</i></p>";
  html += "<p style='margin-top:12px'><a class='file' href='/download?file=" + file + "'>&#8681; Telecharger le CSV complet de la session</a></p>";
  httpServer.sendContent(html);
  html = "";

  html += PAGE_FOOTER;
  httpServer.sendContent(html);
}


// ===================== Page "/status" -- etat systeme =====================

static void handleStatusPage() {
  WebServerStatusInfo s;
  if (g_getStatus) s = g_getStatus();

  String html = pageHeader("status");
  html += "<h2>Statut systeme</h2>";

  html += "<div class='card' style='display:block'>GPS : <span class='pill ";
  html += s.bleConnected ? "ok'>CONNECTE" : "bad'>DECONNECTE";
  html += "</span><br>Fix GPS : " + String(s.fixStatus) + " &nbsp; Satellites : " + String(s.numSats);
  html += " &nbsp; Debit : <span class='pill " + String(s.gpsRmcHz >= 8.0f ? "ok" : "bad") + "'>" + String(s.gpsRmcHz, 1) + "Hz</span>";
  html += " &nbsp; ACK PAIR050 : " + String(s.gpsFixRateAckOk ? "oui" : "non") + "</div>";

  html += "<div class='card' style='display:block'>Circuit actif<br><span class='big'>" + s.circuitName + "</span>";
  html += "<br>Rejets de detection : " + String(s.detectionRejectionCount) + "</div>";

  html += "<div class='card' style='display:block'>Enregistrement : <span class='pill ";
  html += s.recordingEnabled ? "ok'>ACTIF" : "bad'>ARRETE";
  html += "</span><br>Tours : " + String(s.lapsCount) +
          " &nbsp; Dernier : " + s.lastLapTime + " &nbsp; Meilleur : " + s.bestLapTime + "</div>";

  html += "<div class='card' style='display:block'>Batterie : <span class='pill " + String(s.battPercent <= 15 ? "bad" : "ok") + "'>" + String(s.battPercent) + "%</span>";
  html += " (" + String(s.battVoltage, 2) + "V) &nbsp; CPU : <span class='pill " + String(s.cpuTempC >= 70.0f ? "bad" : "ok") + "'>" + String(s.cpuTempC, 0) + "C</span></div>";

  html += "<div class='card' style='display:block'>Memoire libre (heap) : " + String(ESP.getFreeHeap()) + " octets<br>";
  html += "Uptime : " + String(millis() / 1000) + " s</div>";

  html += "<p><a class='file' href='/status'>Rafraichir</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

// ===================== Page "/debug" -- nettoyage du carnet sans passer par Serial =====================
//
// Equivalent web de la commande Serial 'x' (efface completement
// /sessions.csv puis le recree avec son en-tete) -- utile en phase de
// test/reglage pour repartir propre sans avoir a rouler un tour juste
// pour "faire descendre" les sessions polluantes de la liste. Page
// volontairement absente du menu de navigation principal (pas de lien
// dans pageHeader()) -- accessible uniquement en tapant /debug dans le
// navigateur, pour eviter un clic accidentel en usage normal sur la
// piste. Un lien discret existe cela dit en bas de la page Sessions.
//
// Meme en-tete que celui ecrit par initSessionLog() cote main.cpp --
// a garder synchronise si ce format venait a changer un jour.
static const char* SESSIONS_CSV_HEADER = "date,local_time,lap_number,lap_time,best_lap_time,circuit,vmax_kmh,vmin_kmh,vavg_kmh,distance_km,heure_depart,wheelie_count,stoppie_count,angle_droit_max_deg,angle_gauche_max_deg";

static void handleDebugPage() {
  String html = pageHeader("firmware");
  html += "<h2>Mode debug</h2>";
  html += "<div class='card' style='display:block;font-size:13px'>Reserve a la phase de test/reglage -- ";
  html += "vide le carnet cumulatif de tours (<code>sessions.csv</code>) sans toucher aux logs GPS detailles. ";
  html += "Equivalent de la commande Serial <code>x</code>.</div>";

  html += "<form action='/debug/clear-sessions' method='GET' onsubmit=\"return confirm('Vider tout le carnet de session ? Irreversible -- tout l\\'historique des tours sera perdu (les logs GPS detailles restent).');\" style='margin-top:12px'>";
  html += "<button type='submit' style='background:#b62324'>Vider tout le carnet de session</button>";
  html += "</form>";

  // ----- Lignes orphelines (hors de tout bloc demarree/arretee) -----
  // Ne devrait normalement pas arriver -- symptome d'un bug d'ecriture
  // passe (cf. commentaire de countOrphanSessionLines()) plutot qu'un
  // etat normal. N'apparaissent PAS dans la liste "Sessions du carnet"
  // ci-dessous (elle vient de loadSessionSummaries(), qui ne les voit
  // pas non plus faute de marqueur) -- sans ce bouton dedie, aucun
  // moyen de les retirer depuis le webserver.
  int orphanCount = countOrphanSessionLines();
  if (orphanCount > 0) {
    html += "<div class='card' style='display:block;font-size:13px;margin-top:12px'>";
    html += String(orphanCount) + " ligne(s) orpheline(s) trouvee(s) dans le carnet (hors de tout bloc session -- ";
    html += "ne peuvent pas apparaitre dans la liste ci-dessous ni etre selectionnees individuellement).</div>";
    html += "<form action='/debug/strip-orphans' method='GET' onsubmit=\"return confirm('Retirer les " + String(orphanCount) + " ligne(s) orpheline(s) du carnet ? Irreversible -- les sessions normales ne sont pas affectees.');\">";
    html += "<button type='submit' style='background:#b62324'>Retirer les lignes orphelines</button>";
    html += "</form>";
  }

  // ----- Fichier /croix_replay.csv (mode exemple, retire du firmware) -----
  // uploadfs (USB) reecrit toute l'image LittleFS d'un coup et efface
  // donc ce fichier avec elle, mais tant que l'USB n'est pas dispo (OTA
  // uniquement, cf. discussion), il continue d'occuper de la place --
  // le supprimer un par un cote LittleFS ici permet de recuperer cette
  // place immediatement, sans attendre un uploadfs complet.
  if (LittleFS.exists("/croix_replay.csv")) {
    File rf = LittleFS.open("/croix_replay.csv", "r");
    size_t rfSize = rf ? rf.size() : 0;
    if (rf) rf.close();
    html += "<form action='/debug/delete-file' method='GET' onsubmit=\"return confirm('Supprimer /croix_replay.csv ? Irreversible -- fichier plus utilise depuis le retrait du mode exemple.');\" style='margin-top:12px'>";
    html += "<input type='hidden' name='file' value='croix_replay.csv'>";
    html += "<button type='submit' style='background:#b62324'>Supprimer croix_replay.csv (" + String(rfSize / 1024) + " Ko)</button>";
    html += "</form>";
  }

  // ----- Liste ligne par ligne, y compris les sessions "orphelines" -----
  // (dont le log_*.csv a deja ete supprime avant l'ajout du nettoyage
  // automatique du carnet -- cf. discussion) : la page Sessions ne peut
  // afficher un bouton Supprimer que pour les sessions qui ont ENCORE
  // leur fichier GPS detaille (la liste vient des fichiers, pas du
  // carnet) -- ici on part au contraire du carnet lui-meme, donc meme
  // les entrees sans fichier associe restent atteignables.
  //
  // Cases a cocher + UN SEUL bouton de suppression en bas -- plutot
  // qu'un bouton par ligne (qui redemarrait l'ESP a chaque clic) :
  // permet de retirer plusieurs sessions polluantes en un seul
  // redemarrage au lieu d'un par ligne.
  // Trie du plus recent au plus ancien, comme l'onglet Sessions -- meme
  // conteneur (vector<SessionSummaryLite>) que celui deja juge sain
  // (jamais implique dans le bug de tas, cf. note plus haut), on se
  // contente d'y ajouter un std::sort, pas de nouveau conteneur.
  std::vector<SessionSummaryLite> summaries = loadSessionSummaries();
  std::sort(summaries.begin(), summaries.end(), [](const SessionSummaryLite& a, const SessionSummaryLite& b) {
    return a.compactKey > b.compactKey; // "AAAAMMJJHHMMSS" se compare correctement en texte
  });
  html += "<h3 style='margin-top:20px'>Sessions du carnet (" + String(summaries.size()) + ")</h3>";
  if (summaries.empty()) {
    html += "<p><i>(carnet vide)</i></p>";
  } else {
    html += "<form action='/debug/delete-sessions' method='GET' onsubmit=\"return confirm('Retirer les sessions cochees du carnet ET leurs logs GPS associes ? Irreversible.');\">";
    String lastDateShown = "";
    for (const SessionSummaryLite& s : summaries) {
      String dateCompact8 = s.compactKey.length() >= 8 ? s.compactKey.substring(0, 8) : "";
      if (dateCompact8.length() == 8 && dateCompact8 != lastDateShown) {
        html += "<h4 style='margin:14px 0 4px;opacity:0.75'>" + prettyDate(dateCompact8) + "</h4>";
        lastDateShown = dateCompact8;
      }
      String label = (s.compactKey.length() >= 14)
        ? prettyTime(s.compactKey.substring(8, 14))
        : s.compactKey;
      bool logFileExists = (s.compactKey.length() >= 14) &&
        g_logsFs->exists("/log_" + s.compactKey.substring(0, 8) + "_" + s.compactKey.substring(8, 14) + ".csv");
      html += "<div class='card' style='padding:8px 16px'>";
      html += "<label style='display:flex;align-items:center;gap:12px;cursor:pointer;width:100%'>";
      html += "<input type='checkbox' name='keys' value='" + s.compactKey + "' style='width:18px;height:18px;flex-shrink:0'>";
      html += "<div><b>" + label + "</b>";
      String debugMeta = isRouteSummary(s)
        ? ("Route -- duree " + s.bestLapTime + (s.lastDistanceKm.length() ? (" -- " + s.lastDistanceKm + " km") : ""))
        : (String(s.lapCount) + " tour(s) -- meilleur : " + s.bestLapTime + (s.circuit.length() ? (" -- " + s.circuit) : ""));
      html += "<div class='meta'>" + debugMeta;
      if (!logFileExists) html += " -- <span style='color:#ff9d9d'>orpheline (log GPS deja supprime)</span>";
      html += "</div></div>";
      html += "</label>";
      html += "</div>";
    }
    html += "<button type='submit' style='background:#b62324;margin-top:12px'>Retirer la selection</button>";
    html += "</form>";
  }

  html += "<p style='margin-top:16px'><a class='file' href='/update'>&larr; retour a Firmware</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

static void handleDebugClearSessions() {
  LittleFS.remove(g_sessionLogPath);
  File f = LittleFS.open(g_sessionLogPath, "w");
  if (f) { f.println(SESSIONS_CSV_HEADER); f.close(); }
  // Redemarrage necessaire -- meme raison que dans handleDeleteRequest()
  // (main.cpp garde ce fichier ouvert en continu depuis setup()).
  restartToApplyChange("home", "Carnet de session vide");
}

// Suppression volontairement restreinte a "croix_replay.csv" (seul cas
// d'usage actuel : liberer la place laissee par le mode exemple retire
// du firmware, avant meme d'avoir un acces USB pour un uploadfs complet)
// -- pas un endpoint generique de suppression de fichier LittleFS.
static void handleDebugDeleteFile() {
  String file = httpServer.arg("file");
  if (file != "croix_replay.csv") { httpServer.send(403, "text/plain", "Fichier non autorise."); return; }
  LittleFS.remove("/" + file);
  String html = pageHeader("firmware");
  html += "<h2>Fichier supprime</h2>";
  html += "<div class='card' style='display:block'>/" + file + " retire de la LittleFS.</div>";
  html += "<p><a class='file' href='/debug'>Retour</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

// Retire en une seule fois toutes les sessions cochees (checkboxes
// "keys", potentiellement plusieurs occurrences du meme nom dans la
// requete GET) -- un seul redemarrage a la fin, quel que soit le nombre
// de sessions selectionnees. Supprime aussi le fichier log_*.csv
// correspondant a chaque session, quand il existe encore -- sinon on se
// retrouvait avec le bloc du carnet propre mais le fichier GPS detaille
// (souvent le plus gros, cf. octets affiches sur la page Sessions) qui
// trainait toujours sur la LittleFS.
static void handleDebugStripOrphans() {
  int before = countOrphanSessionLines();
  stripOrphanSessionLines();
  String message = (before > 0) ? (String(before) + " ligne(s) orpheline(s) retiree(s) du carnet") : "Aucune ligne orpheline trouvee";
  restartToApplyChange("home", message);
}

static void handleDebugDeleteSessions() {
  int removed = 0;
  int n = httpServer.args();
  for (int i = 0; i < n; i++) {
    if (httpServer.argName(i) == "keys") {
      String key = httpServer.arg(i);
      pruneSessionFromCarnet(key);
      if (key.length() >= 14) {
        String logPath = "/log_" + key.substring(0, 8) + "_" + key.substring(8, 14) + ".csv";
        g_logsFs->remove(logPath);
      }
      removed++;
    }
  }
  String message = (removed > 0) ? (String(removed) + " session(s) retiree(s) du carnet (+ logs GPS associes)") : "Aucune session selectionnee";
  restartToApplyChange("home", message);
}

// ===================== Page "/import" -- import de sessions depuis un autre appareil =====================
//
// Deux formulaires independants, chacun avec son propre upload
// multipart/form-data (meme mecanique que /update juste apres) :
//  - /import/logs : copie UN fichier log_*.csv detaille tel quel sur
//    g_logsFs (aucune analyse du contenu, juste le nom valide -- c'est
//    ce nom qui sert de cle pour relier ensuite le log au bon bloc du
//    carnet, cf. compactKeyFromLogFilename()). Un seul fichier a la
//    fois -- pas de <input multiple> : le support de plusieurs fichiers
//    dans un meme POST multipart varie selon les versions de la lib
//    WebServer ESP32, mieux vaut un comportement garanti que rapide.
//  - /import/carnet : fusionne dans le /sessions.csv LOCAL les blocs
//    d'un /sessions.csv externe (carnet complet exporte depuis un autre
//    boitier PigTeam, ou une sauvegarde) -- seuls les blocs dont la cle
//    n'existe PAS deja localement sont ajoutes, en streaming (jamais le
//    fichier uploade entier en RAM, juste la ligne courante + la liste
//    des cles locales deja connues) -- reimporter plusieurs fois le
//    meme carnet ne cree donc pas de doublons.
//
// Les deux imports sont volontairement separes : un log_*.csv seul
// (sans bloc carnet correspondant) reste utilisable -- il apparait dans
// la liste des sessions mais avec "0 tour" (meme comportement qu'une
// session "polluante" existante, cf. pruneSessionFromCarnet plus haut)
// -- pas bloquant, juste incomplet tant que le carnet n'est pas aussi
// importe.
//
// Comme pour pruneSessionFromCarnet()/handleDebugClearSessions(), toute
// ecriture reussie dans sessions.csv est suivie d'un redemarrage
// (restartToApplyChange()) -- main.cpp garde ce fichier ouvert en
// continu depuis setup(), memes precautions que partout ailleurs dans
// ce fichier des qu'on touche au carnet.

// Format strict attendu : log_AAAAMMJJ_HHMMSS.csv (23 caracteres) --
// plus strict que isAllowedGpsLogFile() (qui accepte large, pour le
// telechargement/la suppression d'un fichier deja connu) car
// compactKeyFromLogFilename()/prettyDate()/prettyTime() supposent ce
// format exact ; un nom invalide accepte ici casserait l'affichage de
// toutes les pages qui listent les sessions.
static bool isValidLogFilename(const String& name) {
  if (name.length() != 23 || !name.startsWith("log_") || !name.endsWith(".csv")) return false;
  for (int i = 4; i < 12; i++) if (!isDigit(name[i])) return false;
  if (name[12] != '_') return false;
  for (int i = 13; i < 19; i++) if (!isDigit(name[i])) return false;
  return true;
}

static void handleImportPage() {
  String html = pageHeader("import");
  html += "<h2>Importer des sessions</h2>";
  html += "<p style='opacity:0.8;font-size:13px'>Pour recuperer une session complete depuis un autre appareil PigTeam (ou une sauvegarde) : ";
  html += "telecharge le fichier <code>log_AAAAMMJJ_HHMMSS.csv</code> de la session ET le fichier <code>sessions.csv</code> ";
  html += "(carnet complet -- lien tout en haut de la page Sessions) depuis l'appareil source, puis importe les deux ci-dessous. ";
  html += "Un log importe seul (sans le carnet) reste utilisable mais apparait avec 0 tour tant que le carnet n'est pas aussi importe.</p>";

  html += "<h3>1. Log GPS detaille</h3>";
  html += "<div class='card' style='display:block'>";
  html += "<form method='POST' action='/import/logs' enctype='multipart/form-data'>";
  html += "<input type='file' name='logs' accept='.csv' required style='display:block;margin-bottom:12px;color:#e8eaed'>";
  html += "<button type='submit' style='background:#3a7bd5'>Importer ce log</button>";
  html += "</form>";
  html += "<p style='opacity:0.7;font-size:12px;margin-top:10px'>Un seul fichier a la fois -- repete l'operation pour chaque session a importer.</p>";
  html += "</div>";

  html += "<h3>2. Carnet de sessions (sessions.csv)</h3>";
  html += "<div class='card' style='display:block'>";
  html += "<form method='POST' action='/import/carnet' enctype='multipart/form-data'>";
  html += "<input type='file' name='carnet' accept='.csv' required style='display:block;margin-bottom:12px;color:#e8eaed'>";
  html += "<button type='submit' style='background:#3a7bd5'>Fusionner le carnet</button>";
  html += "</form>";
  html += "<p style='opacity:0.7;font-size:12px;margin-top:10px'>Les sessions deja presentes localement sont automatiquement ignorees -- reimporter plusieurs fois le meme carnet ne cree pas de doublons.</p>";
  html += "</div>";

  html += "<h3>3. Session RaceChrono</h3>";
  html += "<div class='card' style='display:block'>";
  html += "<p style='opacity:0.8;font-size:13px;margin-top:0'>Exporte une session depuis RaceChrono au format CSV (celui avec les colonnes ";
  html += "<code>Lap #, Timestamp (s), ..., Trap name</code>) et importe-la directement -- convertie automatiquement en session locale, ";
  html += "log GPS detaille + entree carnet compris, pas besoin des deux fichiers separement comme ci-dessus.</p>";
  html += "<form method='POST' action='/import/racechrono' enctype='multipart/form-data'>";
  html += "<input type='file' name='racechrono' accept='.csv' required style='display:block;margin-bottom:12px;color:#e8eaed'>";
  html += "<button type='submit' style='background:#3a7bd5'>Importer cette session</button>";
  html += "</form>";
  html += "</div>";

  html += "<h3>4. Sauvegarde complete (.zip)</h3>";
  html += "<div class='card' style='display:block'>";
  html += "<p style='opacity:0.8;font-size:13px;margin-top:0'>Reinjecte une archive .zip generee par le bouton ";
  html += "\"Sauvegarde complete\" (page Sessions) de CE boitier ou d'un autre -- carnet fusionne (anti-doublon comme ci-dessus) ";
  html += "et logs GPS restaures d'un coup, sans avoir a extraire l'archive a la main.</p>";
  html += "<form method='POST' action='/import/restore' enctype='multipart/form-data'>";
  html += "<input type='file' name='backup' accept='.zip' required style='display:block;margin-bottom:12px;color:#e8eaed'>";
  html += "<button type='submit' style='background:#3a7bd5'>Restaurer cette sauvegarde</button>";
  html += "</form>";
  html += "</div>";

  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

// ----- /import/logs : copie directe d'UN log_*.csv sur g_logsFs -----

static File importLogOut;
static bool importLogHasError = false;
static int importLogSuccessCount = 0;
static std::vector<String> importLogSkippedNames;

static void handleImportLogsUpload() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    importLogHasError = false;
    importLogSuccessCount = 0;
    importLogSkippedNames.clear();

    String name = upload.filename;
    if (!isValidLogFilename(name)) {
      importLogHasError = true;
      importLogSkippedNames.push_back(name + " (nom invalide, attendu log_AAAAMMJJ_HHMMSS.csv)");
      return;
    }
    String path = "/" + name;
    if (g_logsFs->exists(path)) {
      importLogHasError = true;
      importLogSkippedNames.push_back(name + " (une session avec ce nom existe deja ici, ignoree)");
      return;
    }
    importLogOut = g_logsFs->open(path, "w");
    if (!importLogOut) {
      importLogHasError = true;
      importLogSkippedNames.push_back(name + " (echec de creation -- flash pleine ?)");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!importLogHasError && importLogOut) {
      size_t written = importLogOut.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        // Espace insuffisant (ou autre echec d'ecriture) -- s'arrete net
        // plutot que de continuer a essayer d'ecrire le reste du fichier
        // (chaque tentative en echec spamme le log serie "No more free
        // space" pour rien) et supprime le fichier partiel : un fichier
        // log_*.csv tronque ne serait de toute facon pas exploitable, et
        // continuerait a occuper la place qu'il a deja pu prendre.
        importLogHasError = true;
        importLogOut.close();
        g_logsFs->remove("/" + upload.filename);
        importLogSkippedNames.push_back(upload.filename + " (plus d'espace disponible -- fichier partiel supprime)");
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (importLogOut) importLogOut.close();
    if (!importLogHasError) importLogSuccessCount++;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (importLogOut) importLogOut.close();
    if (upload.filename.length() > 0) g_logsFs->remove("/" + upload.filename); // ne laisse pas un fichier partiel/corrompu
    importLogHasError = true;
  }
}

static void handleImportLogsResult() {
  String html = pageHeader("import");
  html += "<h2>Import du log GPS</h2>";
  if (importLogSuccessCount > 0) {
    html += "<div class='card' style='display:block'>Log importe avec succes.</div>";
  }
  if (!importLogSkippedNames.empty()) {
    html += "<div class='card' style='display:block'><b>Non importe :</b><ul style='margin:6px 0 0;padding-left:18px'>";
    for (const String& s : importLogSkippedNames) html += "<li>" + s + "</li>";
    html += "</ul></div>";
  }
  html += "<p><a class='file' href='/import'>Importer un autre fichier</a> -- <a class='file' href='/'>Voir les sessions</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

// ----- /import/carnet : fusion des blocs absents localement dans sessions.csv -----

static std::vector<String> importCarnetExistingKeys;
static File importCarnetOut;
static String importCarnetLineBuf;
static bool importCarnetSkippingBlock = false;
static int importCarnetAddedBlocks = 0;
static int importCarnetSkippedBlocks = 0;
static bool importCarnetHasError = false;
static String importCarnetErrorMsg;

static bool carnetKeyExistsLocally(const String& key) {
  for (const String& k : importCarnetExistingKeys) if (k == key) return true;
  return false;
}

// Traite UNE ligne complete (deja isolee par le decoupage sur '\n' cote
// UPLOAD_FILE_WRITE) du carnet uploade : decide si elle appartient a un
// bloc nouveau (a garder) ou deja present localement (a ignorer), et
// l'ecrit immediatement dans sessions.csv le cas echeant -- rien n'est
// jamais accumule en RAM au-dela de la ligne courante, contrairement a
// pruneSessionFromCarnet() qui doit charger tout le fichier LOCAL car
// il modifie des blocs existants ; ici on ne fait qu'ajouter a la fin.
//
// Verifie desormais la valeur de retour de println() -- jusque-la
// silencieusement ignoree, ce qui laissait une fusion continuer (et
// spammer "No more free space" sur le port serie a chaque ligne) meme
// une fois la LittleFS pleine, sans que l'appelant (handleImportCarnetUpload
// OU la restauration ZIP, qui partage cette fonction) ne le sache jamais.
static void importCarnetProcessLine(String line) {
  if (importCarnetHasError) return; // deja en echec (ex: espace insuffisant sur une ligne precedente) -- n'ecrit plus rien
  line.trim();
  if (line.length() == 0 || line.startsWith("date,")) return; // ligne d'en-tete CSV -- deja presente localement, jamais dupliquee

  bool isStartMarker = line.startsWith("# session demarree");
  bool isStopMarker = line.startsWith("# session arretee");

  if (isStartMarker) {
    String rest = line.substring(line.indexOf("demarree") + 9);
    String key = stripSeparators(rest);
    importCarnetSkippingBlock = carnetKeyExistsLocally(key);
    if (importCarnetSkippingBlock) { importCarnetSkippedBlocks++; return; }
    importCarnetAddedBlocks++;
    if (importCarnetOut.println(line) == 0) {
      importCarnetHasError = true;
      importCarnetErrorMsg = "Plus d'espace disponible sur la LittleFS -- fusion du carnet interrompue.";
    }
    return;
  }

  if (importCarnetSkippingBlock) {
    if (isStopMarker) importCarnetSkippingBlock = false;
    return;
  }

  if (importCarnetOut.println(line) == 0) {
    importCarnetHasError = true;
    importCarnetErrorMsg = "Plus d'espace disponible sur la LittleFS -- fusion du carnet interrompue.";
  }
}

static void handleImportCarnetUpload() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    importCarnetHasError = false;
    importCarnetErrorMsg = "";
    importCarnetAddedBlocks = 0;
    importCarnetSkippedBlocks = 0;
    importCarnetSkippingBlock = false;
    importCarnetLineBuf = "";

    // Cles deja presentes localement -- une seule lecture rapide avant
    // de commencer, pour decider bloc par bloc au fil de l'upload sans
    // jamais recharger le carnet uploade en entier.
    importCarnetExistingKeys.clear();
    File local = LittleFS.open(g_sessionLogPath, "r");
    if (local) {
      while (local.available()) {
        String l = local.readStringUntil('\n');
        l.trim();
        if (l.startsWith("# session demarree")) {
          String rest = l.substring(l.indexOf("demarree") + 9);
          importCarnetExistingKeys.push_back(stripSeparators(rest));
        }
      }
      local.close();
    }

    importCarnetOut = LittleFS.open(g_sessionLogPath, "a");
    if (!importCarnetOut) {
      importCarnetHasError = true;
      importCarnetErrorMsg = "Impossible d'ouvrir le carnet local en ecriture.";
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (importCarnetHasError) return;
    for (size_t i = 0; i < upload.currentSize; i++) {
      char c = (char)upload.buf[i];
      if (c == '\n') {
        importCarnetProcessLine(importCarnetLineBuf);
        importCarnetLineBuf = "";
        if (importCarnetHasError) return; // espace insuffisant detecte en cours de chunk -- inutile de continuer a decouper le reste
      } else if (c != '\r') {
        importCarnetLineBuf += c;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!importCarnetHasError) {
      if (importCarnetLineBuf.length() > 0) { importCarnetProcessLine(importCarnetLineBuf); importCarnetLineBuf = ""; }
      if (importCarnetOut) importCarnetOut.close();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (importCarnetOut) importCarnetOut.close();
    importCarnetHasError = true;
    importCarnetErrorMsg = "Transfert annule.";
  }
}

static void handleImportCarnetResult() {
  if (importCarnetHasError) {
    String html = pageHeader("import");
    html += "<h2>Echec de la fusion du carnet</h2>";
    html += "<div class='card' style='display:block'>" + importCarnetErrorMsg + "</div>";
    html += "<p><a class='file' href='/import'>Retour</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }
  String message = String(importCarnetAddedBlocks) + " session(s) ajoutee(s) au carnet local";
  if (importCarnetSkippedBlocks > 0) message += " (" + String(importCarnetSkippedBlocks) + " deja presente(s), ignoree(s))";
  restartToApplyChange("import", message);
}

// ----- /import/restore : reinjection d'une sauvegarde complete (.zip, cf. /backup) -----
//
// Contrairement a une premiere version, ne stocke JAMAIS l'archive
// entiere nulle part : elle est analysee et extraite en un seul passage,
// OCTET PAR OCTET, directement depuis le flux HTTP entrant (callback
// UPLOAD_FILE_WRITE), au fur et a mesure de sa reception -- meme
// principe qu'une machine a etats deja utilisee ailleurs dans ce
// firmware pour un flux binaire recu en morceaux de taille arbitraire
// (cf. pollGps() dans GpsManager.cpp, qui fait exactement ca pour les
// trames UBX). Zero copie intermediaire sur la flash : la place occupee
// pendant une restauration se limite aux fichiers reellement restaures,
// exactement comme pour /import/logs ou /import/carnet -- pas le double.
//
// Ceci n'est possible QUE parce que handleBackupDownload()/zipWriteFile()
// ecrivent desormais un en-tete local ZIP complet et fiable des le
// depart (CRC32 + taille reelle connus AVANT le premier octet de
// donnees, cf. commentaire dans zipWriteFile()) -- plus besoin d'un
// "data descriptor" ni de sauter a la fin du fichier pour retrouver le
// repertoire central : chaque fichier de l'archive s'auto-decrit
// entierement dans son en-tete local, dans l'ordre ou il arrive.
//
// Repartition par fichier trouve dans l'archive :
//  - sessions.csv (carnet) -> fusionne bloc par bloc via
//    importCarnetProcessLine(), EXACTEMENT la meme logique que
//    /import/carnet juste au-dessus (rien de duplique, juste re-alimente
//    ligne par ligne depuis l'archive au lieu du flux HTTP direct) --
//    donc memes garanties anti-doublon.
//  - log_AAAAMMJJ_HHMMSS.csv -> copie sur g_logsFs, ignore si un fichier
//    du meme nom existe deja localement (meme prudence que /import/logs
//    -- ne jamais ecraser une session locale plus recente).
//  - tout le reste (nom non reconnu, fichier compresse -- archive
//    generee par un autre outil ou par une version anterieure de ce
//    firmware qui utilisait encore le data descriptor) -> les octets
//    de donnees sont quand meme consommes (pour rester synchronise avec
//    la suite du flux) mais rien n'est ecrit, et le fichier est signale
//    dans le rapport final.

enum RestoreState { RS_SIG, RS_HDR, RS_NAME, RS_EXTRA, RS_DATA, RS_STOP };

static RestoreState rsState;
static uint8_t rsAccum[26];   // accumulateur generique (signature 4o OU reste d'en-tete local 26o)
static int rsAccumGot;
static uint16_t rsMethod, rsNameLen, rsExtraLen;
static uint32_t rsExpectedCrc, rsUncompSize, rsRemaining;
static uint32_t rsCrcRunning;
static String rsCurName;
static String rsLineBuf;
static bool rsIsCarnetEntry, rsIsLogEntry, rsSkipCurrent;
static File rsLogOut;
static String rsCarnetEntryName; // g_sessionLogPath sans le '/' -- calcule une fois au demarrage de l'upload

static bool rsHasFatalError;
static String rsFatalErrorMsg;
static int rsLogsImported, rsLogsSkipped;
static std::vector<String> rsNotes;

static void rsFinishCurrentEntry(); // definie plus bas -- utilisee par rsStartData() pour le cas d'un fichier vide (0 octet de donnees, cloture immediate)

// Decide, des que le nom de fichier de l'entree ZIP courante est connu,
// ou (et si) ses octets de donnees doivent etre ecrits.
static void rsStartData() {
  rsIsCarnetEntry = false;
  rsIsLogEntry = false;
  rsSkipCurrent = false;
  rsLineBuf = "";
  rsCrcRunning = 0xFFFFFFFFUL;

  if (rsMethod != 0) {
    rsNotes.push_back(rsCurName + " (compresse -- non supporte, seules les archives 'stored' generees par ce boitier sont restaurables)");
    rsSkipCurrent = true;
  } else if (rsCurName == rsCarnetEntryName) {
    rsIsCarnetEntry = true;
    importCarnetOut = LittleFS.open(g_sessionLogPath, "a");
    if (!importCarnetOut) { rsNotes.push_back(rsCurName + " (impossible d'ouvrir le carnet local en ecriture)"); rsIsCarnetEntry = false; rsSkipCurrent = true; }
  } else if (isValidLogFilename(rsCurName)) {
    String path = "/" + rsCurName;
    if (g_logsFs->exists(path)) {
      rsLogsSkipped++;
      rsNotes.push_back(rsCurName + " (deja present localement, ignore)");
      rsSkipCurrent = true;
    } else {
      rsLogOut = g_logsFs->open(path, "w");
      if (!rsLogOut) { rsNotes.push_back(rsCurName + " (echec de creation -- flash pleine ?)"); rsSkipCurrent = true; }
      else rsIsLogEntry = true;
    }
  } else {
    rsNotes.push_back(rsCurName + " (fichier non reconnu, ignore)");
    rsSkipCurrent = true;
  }

  rsRemaining = rsUncompSize;
  rsState = RS_DATA;
  if (rsRemaining == 0) { // fichier vide -- aucun octet de donnees a attendre, on cloture tout de suite
    rsFinishCurrentEntry();
    rsState = RS_SIG;
  }
}

// Ferme/valide le fichier de l'entree ZIP qui vient de se terminer
// (tous ses octets de donnees consommes) -- verifie le CRC32 pour les
// entrees reellement ecrites, simple garde-fou (vibrations/coupure
// pendant le transfert) : le fichier reste garde meme en cas de
// mismatch (mieux vaut une session suspecte que perdue), juste signale.
static void rsFinishCurrentEntry() {
  uint32_t actualCrc = rsCrcRunning ^ 0xFFFFFFFFUL;
  if (rsIsCarnetEntry) {
    if (rsLineBuf.length() > 0) { importCarnetProcessLine(rsLineBuf); rsLineBuf = ""; }
    if (importCarnetOut) importCarnetOut.close();
    if (importCarnetHasError) {
      // Espace insuffisant en cours de fusion du carnet (detecte par
      // importCarnetProcessLine(), meme verification que /import/carnet) --
      // fatal pour toute la restauration, pas seulement cette entree :
      // pas de raison de continuer a restaurer des logs GPS individuels
      // si le carnet lui-meme n'a pas pu etre fusionne en entier.
      rsHasFatalError = true;
      rsFatalErrorMsg = importCarnetErrorMsg;
    } else if (actualCrc != rsExpectedCrc) {
      rsNotes.push_back(rsCurName + " (CRC invalide -- transfert peut-etre corrompu, verifie le carnet)");
    }
  } else if (rsIsLogEntry) {
    if (rsLogOut) rsLogOut.close();
    if (actualCrc != rsExpectedCrc) rsNotes.push_back(rsCurName + " (CRC invalide -- transfert peut-etre corrompu)");
    else rsLogsImported++;
  }
  rsIsCarnetEntry = false;
  rsIsLogEntry = false;
}

// Coeur de la machine a etats -- un octet a la fois, appelable
// quel que soit le decoupage des paquets recus (meme principe que
// pollGps()/GpsManager.cpp).
static void rsFeedByte(uint8_t b) {
  if (rsHasFatalError || rsState == RS_STOP) return;

  switch (rsState) {
    case RS_SIG:
      rsAccum[rsAccumGot++] = b;
      if (rsAccumGot == 4) {
        rsAccumGot = 0;
        if (rsAccum[0] == 0x50 && rsAccum[1] == 0x4b && rsAccum[2] == 0x03 && rsAccum[3] == 0x04) {
          rsState = RS_HDR;
        } else if (rsAccum[0] == 0x50 && rsAccum[1] == 0x4b && rsAccum[2] == 0x01 && rsAccum[3] == 0x02) {
          rsState = RS_STOP; // repertoire central atteint -- plus rien d'utile ensuite
        } else {
          rsHasFatalError = true;
          rsFatalErrorMsg = "Archive .zip illisible ou non reconnue -- ce champ attend une sauvegarde generee par ce boitier (bouton \"Sauvegarde complete\" sur la page Sessions), pas une archive quelconque.";
        }
      }
      break;

    case RS_HDR:
      rsAccum[rsAccumGot++] = b;
      if (rsAccumGot == 26) {
        uint16_t flag = rsAccum[2] | ((uint16_t)rsAccum[3] << 8);
        rsMethod      = rsAccum[4] | ((uint16_t)rsAccum[5] << 8);
        rsExpectedCrc = (uint32_t)rsAccum[10] | ((uint32_t)rsAccum[11] << 8) | ((uint32_t)rsAccum[12] << 16) | ((uint32_t)rsAccum[13] << 24);
        rsUncompSize  = (uint32_t)rsAccum[18] | ((uint32_t)rsAccum[19] << 8) | ((uint32_t)rsAccum[20] << 16) | ((uint32_t)rsAccum[21] << 24);
        rsNameLen     = rsAccum[22] | ((uint16_t)rsAccum[23] << 8);
        rsExtraLen    = rsAccum[24] | ((uint16_t)rsAccum[25] << 8);
        if (flag & 0x0008) {
          rsHasFatalError = true;
          rsFatalErrorMsg = "Archive generee par une version anterieure de ce firmware (format non supporte par cette version) -- regenere une nouvelle sauvegarde avec le bouton \"Sauvegarde complete\" puis reessaie.";
          break;
        }
        rsCurName = "";
        rsCurName.reserve(rsNameLen);
        rsAccumGot = 0;
        rsState = (rsNameLen > 0) ? RS_NAME : (rsExtraLen > 0 ? RS_EXTRA : RS_DATA);
        if (rsState != RS_NAME && rsState != RS_EXTRA) rsStartData();
      }
      break;

    case RS_NAME:
      rsCurName += (char)b;
      if ((uint16_t)rsCurName.length() == rsNameLen) {
        rsState = (rsExtraLen > 0) ? RS_EXTRA : RS_DATA;
        rsAccumGot = 0;
        if (rsState == RS_DATA) rsStartData();
      }
      break;

    case RS_EXTRA:
      rsAccumGot++;
      if (rsAccumGot == (int)rsExtraLen) rsStartData();
      break;

    case RS_DATA:
      rsCrcRunning = crc32Update(rsCrcRunning, &b, 1);
      if (!rsSkipCurrent) {
        if (rsIsCarnetEntry) {
          char c = (char)b;
          if (c == '\n') {
            importCarnetProcessLine(rsLineBuf);
            rsLineBuf = "";
            if (importCarnetHasError) {
              // Espace insuffisant en plein milieu du carnet -- inutile de
              // continuer a consommer le reste de l'archive octet par
              // octet, rsFinishCurrentEntry() (jamais atteinte ici, cf.
              // rsHasFatalError verifie en tete de rsFeedByte) se chargera
              // de fermer proprement importCarnetOut au prochain appel.
              rsHasFatalError = true;
              rsFatalErrorMsg = importCarnetErrorMsg;
              if (importCarnetOut) importCarnetOut.close();
              return;
            }
          } else if (c != '\r') {
            rsLineBuf += c;
          }
        } else if (rsIsLogEntry && rsLogOut) {
          if (rsLogOut.write(b) != 1) {
            // Meme raisonnement que /import/logs : un log_*.csv tronque
            // n'est de toute facon pas exploitable, autant liberer
            // immediatement la place qu'il a deja prise plutot que de le
            // laisser trainer, et arreter la restauration net plutot que
            // de continuer a tenter d'ecrire (chaque octet en echec
            // spammait "No more free space" sur le port serie).
            rsHasFatalError = true;
            rsFatalErrorMsg = "Plus d'espace disponible sur la LittleFS -- restauration interrompue en cours d'ecriture de " + rsCurName + ".";
            rsLogOut.close();
            g_logsFs->remove("/" + rsCurName);
            return;
          }
        }
      }
      rsRemaining--;
      if (rsRemaining == 0) {
        rsFinishCurrentEntry();
        rsState = RS_SIG;
        rsAccumGot = 0;
      }
      break;

    case RS_STOP:
      break;
  }
}

static void handleRestoreUploadUpload() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    rsState = RS_SIG;
    rsAccumGot = 0;
    rsHasFatalError = false;
    rsFatalErrorMsg = "";
    rsLogsImported = 0;
    rsLogsSkipped = 0;
    rsNotes.clear();
    rsCarnetEntryName = g_sessionLogPath.substring(1);

    // Cles deja presentes localement -- meme lecture preparatoire que
    // handleImportCarnetUpload(), necessaire ici aussi puisque
    // importCarnetProcessLine() s'appuie dessus.
    importCarnetExistingKeys.clear();
    File local = LittleFS.open(g_sessionLogPath, "r");
    if (local) {
      while (local.available()) {
        String l = local.readStringUntil('\n');
        l.trim();
        if (l.startsWith("# session demarree")) {
          String rest = l.substring(l.indexOf("demarree") + 9);
          importCarnetExistingKeys.push_back(stripSeparators(rest));
        }
      }
      local.close();
    }
    importCarnetAddedBlocks = 0;
    importCarnetSkippedBlocks = 0;
    importCarnetSkippingBlock = false;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (rsHasFatalError) return;
    for (size_t i = 0; i < upload.currentSize; i++) rsFeedByte(upload.buf[i]);
  } else if (upload.status == UPLOAD_FILE_END) {
    // Rien a cloturer explicitement : soit RS_STOP a ete atteint (fin
    // normale), soit rsFeedByte() a deja tout ferme/nettoye lui-meme en
    // posant rsHasFatalError (espace insuffisant, cf. RS_DATA), soit un
    // fichier etait encore en cours sans raison connue (RS_DATA sans
    // erreur fatale -- cas improbable, archive tronquee en transit) et
    // on referme alors proprement les handles pour ne pas les laisser
    // trainer.
    if (rsState == RS_DATA && !rsHasFatalError) {
      if (importCarnetOut) importCarnetOut.close();
      if (rsLogOut) rsLogOut.close();
      rsNotes.push_back("Archive tronquee -- dernier fichier (" + rsCurName + ") incomplet, ignore.");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (importCarnetOut) importCarnetOut.close();
    if (rsLogOut) rsLogOut.close();
    rsHasFatalError = true;
    rsFatalErrorMsg = "Transfert annule.";
  }
}

static void handleRestoreResult() {
  String html = pageHeader("import");

  if (rsHasFatalError) {
    html += "<h2>Echec de la restauration</h2><div class='card' style='display:block'>" + rsFatalErrorMsg + "</div>";
    html += "<p><a class='file' href='/import'>Retour</a></p>" + String(PAGE_FOOTER);
    httpServer.send(200, "text/html", html);
    return;
  }

  // sessions.csv modifie -- redemarrage necessaire (main.cpp le garde
  // ouvert en continu, meme raison que partout ailleurs dans ce fichier
  // des qu'on touche au carnet). Les logs GPS restaures n'ont pas besoin
  // de ce redemarrage (pas de handle garde ouvert dessus), mais on ne
  // perd rien a le faire au meme moment.
  if (importCarnetAddedBlocks > 0) {
    String message = String(importCarnetAddedBlocks) + " session(s) restauree(s) au carnet";
    if (rsLogsImported > 0) message += ", " + String(rsLogsImported) + " log(s) GPS restaure(s)";
    if (importCarnetSkippedBlocks > 0 || rsLogsSkipped > 0) {
      message += " (" + String(importCarnetSkippedBlocks + rsLogsSkipped) + " deja present(s), ignore(s))";
    }
    restartToApplyChange("import", message);
    return;
  }

  html += "<h2>Restauration terminee</h2>";
  html += "<div class='card' style='display:block'>";
  html += String(rsLogsImported) + " log(s) GPS detaille(s) restaure(s)";
  if (rsLogsSkipped > 0) html += ", " + String(rsLogsSkipped) + " deja present(s) (ignores)";
  html += ".<br>Aucune nouvelle session dans le carnet (deja toutes presentes localement).</div>";
  if (!rsNotes.empty()) {
    html += "<div class='card' style='display:block'><b>Details :</b><ul style='margin:6px 0 0;padding-left:18px'>";
    for (const String& s : rsNotes) html += "<li>" + s + "</li>";
    html += "</ul></div>";
  }
  html += "<p><a class='file' href='/import'>Restaurer une autre sauvegarde</a> -- <a class='file' href='/'>Voir les sessions</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}


// ----- /import/racechrono : conversion d'un export RaceChrono (.csv) -----
//
// Format observe (RaceChrono Pro v10.2.3, export "v1", cf. echantillon
// fourni) : quelques lignes de metadonnees ("Session title,...", "Track
// name,...", "Created,DD/MM/AAAA,HH:MM", ...), une ligne vide, puis
// l'entete CSV proprement dit ("Lap #,Timestamp (s),...,Trap name") suivi
// des lignes de donnees. RaceChrono a plusieurs variantes de CSV selon la
// version/les options d'export (v1/v2/v3, RaceChrono2AVI...) -- celle-ci
// n'est PAS garantie identique a une autre variante. Les colonnes sont
// donc retrouvees PAR NOM dans l'entete (rcParseHeader()) plutot que par
// position fixe, et l'import est refuse explicitement si une colonne
// necessaire manque -- mieux vaut un echec net qu'un tour silencieusement
// mal converti sur un outil de chrono.
//
// Point cle du format : le "Trap name" vaut "Start/Finish" EXACTEMENT sur
// la ligne ou un tour numerote commence (meme ligne que le changement de
// "Lap #") -- ca donne directement l'instant de chaque passage ligne, pas
// besoin de le deviner comme on doit parfois le faire ailleurs. "Lap #"
// vaut "N/A" avant le premier passage (tour de sortie/echauffement, pas
// compte) -- traite comme "0 tour valide", meme convention que le reste
// du firmware. Un tour en cours qui n'a jamais recroise la ligne (session
// arretee en cours de tour) n'est simplement jamais ecrit dans le carnet
// -- comme un tour non termine ne l'est jamais par checkLapCompletion()
// cote enregistrement normal.
//
// Reconstruction de current_lap_ms : recalcule depuis Timestamp(s) (delta
// depuis le dernier passage ligne, ou depuis le debut du fichier avant le
// 1er passage) -- pas une simple copie, RaceChrono n'a pas cette colonne
// telle quelle. C'est ce qui permet a extractLap() (page /compare) de
// retrouver le bon decoupage par tour sur un fichier importe exactement
// comme sur un fichier enregistre normalement, sans code specifique.
//
// Heure de la session (pour le nom log_AAAAMMJJ_HHMMSS.csv et le carnet) :
// la DATE vient de la ligne "Created" (fiable, precision au jour), mais
// son HEURE n'est precise qu'a la minute -- l'heure complete (avec les
// secondes) est donc recalculee a partir du Timestamp de la toute
// premiere ligne de donnees (secondes depuis minuit, confirme par
// recoupement avec l'echantillon fourni : Timestamp=36342.3s = 10:05:42,
// coherent avec "Created,...,10:05").

struct RcColumns {
  int lap = -1, timestamp = -1, sats = -1, lat = -1, lng = -1, speedKmh = -1, trap = -1;
  bool complete() const { return lap >= 0 && timestamp >= 0 && sats >= 0 && lat >= 0 && lng >= 0 && speedKmh >= 0 && trap >= 0; }
};

// Valeur d'une ligne de metadonnees "Cle,Valeur" (RaceChrono entoure
// certaines valeurs de guillemets, ex: Track name,"Croix-en-Ternois") --
// simple decoupage sur la 1ere virgule, pas de gestion de virgule DANS
// des guillemets (pas observe sur les metadonnees RaceChrono, seulement
// les lignes de donnees en sont totalement depourvues de toute facon).
static String rcMetaValue(const String& line) {
  int comma = line.indexOf(',');
  if (comma < 0) return "";
  String v = line.substring(comma + 1);
  v.trim();
  if (v.length() >= 2 && v[0] == '"' && v[v.length() - 1] == '"') v = v.substring(1, v.length() - 1);
  return v;
}

// Ligne "Created,DD/MM/AAAA,HH:MM" -- ne garde que la date, reformatee
// "AAAA-MM-JJ" (meme format que getLocalDateTime() cote main.cpp).
static String rcCreatedDateFrom(const String& line) {
  int c1 = line.indexOf(',');
  if (c1 < 0) return "";
  int c2 = line.indexOf(',', c1 + 1);
  String datePart = (c2 < 0) ? line.substring(c1 + 1) : line.substring(c1 + 1, c2);
  datePart.trim();
  int s1 = datePart.indexOf('/'), s2 = datePart.indexOf('/', s1 + 1);
  if (s1 < 0 || s2 < 0) return "";
  String dd = datePart.substring(0, s1), mm = datePart.substring(s1 + 1, s2), yyyy = datePart.substring(s2 + 1);
  if (dd.length() != 2 || mm.length() != 2 || yyyy.length() != 4) return "";
  return yyyy + "-" + mm + "-" + dd;
}

static RcColumns rcParseHeader(const String& headerLine) {
  RcColumns c;
  int idx = 0, start = 0;
  for (int i = 0; i <= (int)headerLine.length(); i++) {
    if (i == (int)headerLine.length() || headerLine[i] == ',') {
      String field = headerLine.substring(start, i);
      field.trim();
      if (field == "Lap #") c.lap = idx;
      else if (field == "Timestamp (s)") c.timestamp = idx;
      else if (field == "Locked satellites") c.sats = idx;
      else if (field == "Latitude (deg)") c.lat = idx;
      else if (field == "Longitude (deg)") c.lng = idx;
      else if (field == "Speed (km/h)") c.speedKmh = idx;
      else if (field == "Trap name") c.trap = idx;
      idx++;
      start = i + 1;
    }
  }
  return c;
}

// Etat de la conversion en cours -- un seul import a la fois (mode
// telechargement WiFi de toute facon mono-client en pratique).
static bool rcHasError = false;
static String rcErrorMsg;
static bool rcInDataPhase = false;
static RcColumns rcCols;
static String rcTrackName;
static String rcCreatedDate;
static bool rcHaveCompactKey = false;
static String rcCompactKey;         // "AAAAMMJJ_HHMMSS"
static File rcDetailOut;            // log_*.csv (g_logsFs)
static File rcCarnetOut;            // sessions.csv (LittleFS, mode "a")
static float rcCurLapStartTs = -1; // Timestamp(s) du debut du segment en cours (tour ou pre-tour 1)
static int rcLastNumericLap = 0;    // dernier "Lap #" numerique vu (0 = encore avant le 1er tour)
static int rcPendingLapNumber = -1; // tour dont on attend la fin pour ecrire sa ligne carnet
static float rcPendingLapStartTs = -1;
static long rcBestLapMs = -1;
static int rcLapsWritten = 0;

static void rcAbort(const String& msg) {
  rcHasError = true;
  rcErrorMsg = msg;
  if (rcDetailOut) { rcDetailOut.close(); if (rcHaveCompactKey) g_logsFs->remove("/log_" + rcCompactKey + ".csv"); }
  if (rcCarnetOut) rcCarnetOut.close(); // deja "a" -- ce qui est ecrit avant l'erreur reste (comme pour une coupure secteur), sans consequence a la relecture (blocs bien delimites)
}

static void rcWriteCompletedLap(int lapNumber, long lapMs, const String& heureHHMMSS) {
  if (lapMs < 0) return;
  if (rcBestLapMs < 0 || lapMs < rcBestLapMs) rcBestLapMs = lapMs;
  String line = rcCreatedDate + "," + heureHHMMSS + "," + String(lapNumber) + "," +
                msToLapTime(lapMs) + "," + msToLapTime(rcBestLapMs) + "," + rcTrackName;
  rcCarnetOut.println(line);
  rcLapsWritten++;
}

static void rcProcessDataLine(const String& line) {
  if (rcHasError || line.length() == 0) return;

  // Decoupage simple par virgule -- les lignes de donnees RaceChrono
  // n'ont jamais de guillemets (contrairement aux metadonnees).
  std::vector<String> cols;
  int start = 0;
  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == ',') { cols.push_back(line.substring(start, i)); start = i + 1; }
  }
  int neededCols = 1 + std::max({ rcCols.lap, rcCols.timestamp, rcCols.sats, rcCols.lat, rcCols.lng, rcCols.speedKmh, rcCols.trap });
  if ((int)cols.size() < neededCols) return; // ligne tronquee/vide en fin de fichier -- ignoree plutot que plantee

  float timestamp = cols[rcCols.timestamp].toFloat();
  String lapField = cols[rcCols.lap]; lapField.trim();
  String trapName = cols[rcCols.trap]; trapName.trim();

  if (!rcHaveCompactKey) {
    if (rcCreatedDate.length() == 0) { rcAbort("Date de session introuvable (ligne 'Created' absente ou illisible)."); return; }
    long totalSec = (long)timestamp;
    char timeCompact[8];
    snprintf(timeCompact, sizeof(timeCompact), "%02ld%02ld%02ld", (totalSec / 3600) % 24, (totalSec / 60) % 60, totalSec % 60);
    String dateCompact = rcCreatedDate; dateCompact.replace("-", "");
    rcCompactKey = dateCompact + "_" + String(timeCompact);

    String logPath = "/log_" + rcCompactKey + ".csv";
    if (g_logsFs->exists(logPath)) { rcAbort("Une session existe deja a ce nom (" + rcCompactKey + ") -- deja importee ?"); return; }

    rcDetailOut = g_logsFs->open(logPath, "w");
    if (!rcDetailOut) { rcAbort("Impossible de creer " + logPath + " (flash pleine ?)"); return; }
    rcDetailOut.println("local_time,millis_boot,lat,lng,speed_kmh,fix,sats,laps,circuit,current_lap_ms");

    rcCarnetOut = LittleFS.open(g_sessionLogPath, "a");
    if (!rcCarnetOut) { rcAbort("Impossible d'ouvrir le carnet local en ecriture."); return; }
    rcCarnetOut.println("# session demarree " + rcCompactKey);

    rcHaveCompactKey = true;
    rcCurLapStartTs = timestamp;
  }

  bool isCrossing = (trapName == "Start/Finish");
  bool lapIsNumeric = (lapField.length() > 0 && lapField != "N/A");

  if (isCrossing) {
    if (rcPendingLapNumber >= 0) {
      long lapMs = (long)((timestamp - rcPendingLapStartTs) * 1000.0 + 0.5);
      long totalSec = (long)timestamp;
      char heureBuf[10];
      snprintf(heureBuf, sizeof(heureBuf), "%02ld:%02ld:%02ld", (totalSec / 3600) % 24, (totalSec / 60) % 60, totalSec % 60);
      rcWriteCompletedLap(rcPendingLapNumber, lapMs, String(heureBuf));
    }
    rcCurLapStartTs = timestamp;
    if (lapIsNumeric) { rcPendingLapNumber = lapField.toInt(); rcPendingLapStartTs = timestamp; }
  }

  if (lapIsNumeric) rcLastNumericLap = lapField.toInt();
  int ourLaps = (rcLastNumericLap > 0) ? (rcLastNumericLap - 1) : 0;
  long curLapMs = (long)((timestamp - rcCurLapStartTs) * 1000.0 + 0.5);
  if (curLapMs < 0) curLapMs = 0;

  long totalSecRow = (long)timestamp;
  char localTimeBuf[10];
  snprintf(localTimeBuf, sizeof(localTimeBuf), "%02ld:%02ld:%02ld", (totalSecRow / 3600) % 24, (totalSecRow / 60) % 60, totalSecRow % 60);

  rcDetailOut.printf("%s,0,%s,%s,%s,3,%s,%d,%s,%ld\n",
                      localTimeBuf, cols[rcCols.lat].c_str(), cols[rcCols.lng].c_str(),
                      cols[rcCols.speedKmh].c_str(), cols[rcCols.sats].c_str(), ourLaps, rcTrackName.c_str(), curLapMs);
}

static void rcProcessMetaLine(const String& line) {
  if (line.startsWith("Track name,")) rcTrackName = rcMetaValue(line);
  else if (line.startsWith("Created,")) rcCreatedDate = rcCreatedDateFrom(line);
  else if (line.startsWith("Lap #,")) {
    rcCols = rcParseHeader(line);
    if (!rcCols.complete()) {
      rcAbort("Format RaceChrono non reconnu -- colonnes attendues introuvables dans l'entete (Lap #/Timestamp/Locked satellites/Latitude/Longitude/Speed (km/h)/Trap name).");
      return;
    }
    if (rcTrackName.length() == 0) rcTrackName = "Import RaceChrono"; // nom de circuit absent du fichier -- reste utilisable, juste moins parlant
    rcInDataPhase = true;
  }
}

static String rcLineBuf;

static void handleImportRaceChronoUpload() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    rcHasError = false;
    rcErrorMsg = "";
    rcInDataPhase = false;
    rcCols = RcColumns();
    rcTrackName = "";
    rcCreatedDate = "";
    rcHaveCompactKey = false;
    rcCompactKey = "";
    rcCurLapStartTs = -1;
    rcLastNumericLap = 0;
    rcPendingLapNumber = -1;
    rcPendingLapStartTs = -1;
    rcBestLapMs = -1;
    rcLapsWritten = 0;
    rcLineBuf = "";
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (rcHasError) return;
    for (size_t i = 0; i < upload.currentSize; i++) {
      char c = (char)upload.buf[i];
      if (c == '\n') {
        if (rcInDataPhase) rcProcessDataLine(rcLineBuf); else rcProcessMetaLine(rcLineBuf);
        rcLineBuf = "";
        if (rcHasError) return;
      } else if (c != '\r') {
        rcLineBuf += c;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!rcHasError && rcLineBuf.length() > 0) {
      if (rcInDataPhase) rcProcessDataLine(rcLineBuf); else rcProcessMetaLine(rcLineBuf);
      rcLineBuf = "";
    }
    if (!rcHasError && !rcHaveCompactKey) rcAbort("Fichier vide ou format non reconnu -- aucune ligne de donnees trouvee.");
    if (!rcHasError) {
      rcCarnetOut.println("# session arretee");
      rcCarnetOut.close();
      rcDetailOut.close();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    rcAbort("Transfert annule.");
  }
}

static void handleImportRaceChronoResult() {
  String html = pageHeader("import");
  if (rcHasError) {
    html += "<h2>Echec de l'import RaceChrono</h2>";
    html += "<div class='card' style='display:block'>" + rcErrorMsg + "</div>";
    html += "<p><a class='file' href='/import'>Retour</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }
  String message = "Session importee (" + String(rcLapsWritten) + " tour(s), circuit \"" + rcTrackName + "\")";
  restartToApplyChange("import", message);
}

// ===================== Page "/update" -- mise a jour du firmware (OTA) =====================
//
// Reutilise le mode WiFi existant (AP, cf.
// startDownloadMode()) -- pas de mode dedie, l'OTA est juste une page de
// plus sur le meme serveur. Upload multipart/form-data classique gere par
// WebServer::upload() (callback separe du handler de reponse, cf. begin()
// dans startDownloadMode() : le 3e argument de httpServer.on() pour POST).
//
// Update.h (bibliotheque ESP32 core) ecrit directement dans la partition
// OTA inactive (ota_0/ota_1, cf. partitions_ota.csv) au fur et a mesure de
// la reception, puis bascule dessus et redemarre si tout s'est bien passe
// -- pas besoin de stocker le .bin entier en RAM ou sur la LittleFS.
//
// Necessite une table de partitions avec deux emplacements d'app
// (board_build.partitions dans platformio.ini) -- le schema par defaut
// (une seule grosse partition app) ne laisse pas de place pour l'OTA.

static bool otaHasError = false;
static String otaErrorMsg;

static void handleUpdatePage() {
  String html = pageHeader("firmware");
  html += "<h2>Mise a jour du firmware</h2>";
  html += "<div class='card' style='display:block'>";
  html += "Version actuellement lancee depuis : <b>" + String(esp_ota_get_running_partition()->label) + "</b>";
  html += "</div>";
  html += "<p>Selectionne le fichier <code>firmware.bin</code> genere par PlatformIO ";
  html += "(<code>.pio/build/&lt;env&gt;/firmware.bin</code> apres compilation).</p>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required style='display:block;margin-bottom:12px;color:#e8eaed'>";
  html += "<button type='submit' style='background:#3a7bd5'>Envoyer et flasher</button>";
  html += "</form>";
  html += "<p style='opacity:0.7;font-size:13px;margin-top:14px'>Le boitier redemarre automatiquement une fois le flash termine (quelques secondes). Ne coupe pas l'alimentation pendant le transfert.</p>";
  html += "<p style='margin-top:20px;opacity:0.4;font-size:12px'><a class='file' style='opacity:0.7' href='/debug'>Mode debug</a></p>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);
}

// Callback d'upload -- appele plusieurs fois par WebServer au fil de la
// reception (START une fois, WRITE plusieurs fois par paquets recus, puis
// END ou ABORTED). Aucune reponse HTTP envoyee ici : c'est le role du
// handler passe en 2e position sur httpServer.on() (handleUpdateResult).
static void handleUpdateUpload() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: reception de %s...\n", upload.filename.c_str());
    otaHasError = false;
    otaErrorMsg = "";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaHasError = true;
      otaErrorMsg = "Impossible de demarrer l'ecriture (partition OTA absente ou trop petite ?)";
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaHasError && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaHasError = true;
      otaErrorMsg = "Erreur d'ecriture pendant le transfert -- fichier corrompu ou coupure WiFi";
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaHasError) {
      if (Update.end(true)) {
        Serial.printf("OTA: %u octets ecrits, flash OK.\n", (unsigned)upload.totalSize);
      } else {
        otaHasError = true;
        otaErrorMsg = "Erreur a la finalisation du flash -- .bin incomplet ou invalide";
        Update.printError(Serial);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end(false); // annule l'ecriture en cours -- la partition inactive reste incomplete, sans consequence (jamais selectionnee au boot)
    otaHasError = true;
    otaErrorMsg = "Transfert annule";
    Serial.println("OTA: transfert annule.");
  }
}

// Handler de reponse -- appele une seule fois, apres la fin complete de
// l'upload (status END/ABORTED deja traite par handleUpdateUpload juste
// avant). Redemarre automatiquement en cas de succes.
static void handleUpdateResult() {
  if (otaHasError) {
    String html = pageHeader("firmware");
    html += "<h2>Echec de la mise a jour</h2>";
    html += "<div class='card' style='display:block'>" + otaErrorMsg + "</div>";
    html += "<p><a class='file' href='/update'>Reessayer</a></p>";
    html += PAGE_FOOTER;
    httpServer.send(200, "text/html", html);
    return;
  }

  String html = pageHeader("firmware");
  html += "<h2>Mise a jour reussie</h2>";
  html += "<div class='card' style='display:block'>Redemarrage en cours...</div>";
  html += PAGE_FOOTER;
  httpServer.send(200, "text/html", html);

  httpServer.client().setNoDelay(true); // pousse la reponse avant de couper -- sinon le navigateur voit juste une connexion perdue
  delay(500);
  ESP.restart();
}

// ===================== Cycle de vie =====================

void WebServerManager::begin(const char* apSsid, const char* sessionLogPath, const char* circuitsFilePath, fs::FS& logsFs,
                              WebServerBleStopFn bleStop, WebServerBleRestartFn bleRestart,
                              WebServerFlushLogsFn flushLogs, WebServerGetStatusFn getStatus) {
  _apSsid = apSsid;
  _sessionLogPath = sessionLogPath;
  _bleStop = bleStop;
  _bleRestart = bleRestart;
  _flushLogs = flushLogs;
  g_sessionLogPath = sessionLogPath;
  g_circuitsFilePath = circuitsFilePath;
  g_logsFs = &logsFs;
  g_flushLogs = flushLogs;
  g_getStatus = getStatus;
}

void WebServerManager::startDownloadMode() {
  _active = true; // active la garde tout de suite

  Serial.println("Ouverture du point d'acces WiFi...");
  Serial.flush(); delay(50);

  if (_bleStop) _bleStop();

  Serial.printf("Heap libre avant WiFi : %u octets\n", ESP.getFreeHeap());
  Serial.flush(); delay(50);

  WiFi.softAP(_apSsid);

  Serial.printf("Heap libre apres WiFi.softAP() : %u octets\n", ESP.getFreeHeap());
  Serial.flush(); delay(50);

  httpServer.on("/", HTTP_GET, handleHomePage);
  httpServer.on("/lap", HTTP_GET, handleLapTracePage);
  httpServer.on("/download", HTTP_GET, handleDownloadRequest);
  httpServer.on("/backup", HTTP_GET, handleBackupDownload);
  httpServer.on("/delete", HTTP_GET, handleDeleteRequest);
  httpServer.on("/circuits", HTTP_GET, handleCircuitsListPage);
  httpServer.on("/circuits/edit", HTTP_GET, handleCircuitEditPage);
  httpServer.on("/circuits/save", HTTP_POST, handleCircuitSave);
  httpServer.on("/circuits/toggle", HTTP_GET, handleCircuitToggle);
  httpServer.on("/circuits/delete", HTTP_GET, handleCircuitDelete);
  httpServer.on("/gps-position", HTTP_GET, handleGpsPositionRequest);
  httpServer.on("/status", HTTP_GET, handleStatusPage);
  httpServer.on("/debug", HTTP_GET, handleDebugPage);
  httpServer.on("/debug/clear-sessions", HTTP_GET, handleDebugClearSessions);
  httpServer.on("/debug/delete-sessions", HTTP_GET, handleDebugDeleteSessions);
  httpServer.on("/debug/strip-orphans", HTTP_GET, handleDebugStripOrphans);
  httpServer.on("/debug/delete-file", HTTP_GET, handleDebugDeleteFile);
  httpServer.on("/import", HTTP_GET, handleImportPage);
  httpServer.on("/import/logs", HTTP_POST, handleImportLogsResult, handleImportLogsUpload);
  httpServer.on("/import/carnet", HTTP_POST, handleImportCarnetResult, handleImportCarnetUpload);
  httpServer.on("/import/racechrono", HTTP_POST, handleImportRaceChronoResult, handleImportRaceChronoUpload);
  httpServer.on("/import/restore", HTTP_POST, handleRestoreResult, handleRestoreUploadUpload);
  httpServer.on("/update", HTTP_GET, handleUpdatePage);
  httpServer.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  httpServer.begin();
  Serial.printf("WiFi actif (reseau ouvert) -- connecte-toi a \"%s\" puis va sur http://%s/\n",
                _apSsid, WiFi.softAPIP().toString().c_str());
}

void WebServerManager::stopDownloadMode() {
  httpServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  _active = false;
  Serial.println("WiFi coupe.");
  if (_bleRestart) _bleRestart();
}

void WebServerManager::loop() {
  if (_active) httpServer.handleClient();
}

String WebServerManager::getIp() const {
  return WiFi.softAPIP().toString();
}
