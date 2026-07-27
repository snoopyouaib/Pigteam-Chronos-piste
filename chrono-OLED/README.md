# firmware_191 -- Chrono GPS moto piste, ESP32-S3-AMOLED-1.91

Ex "display_only_191" (banc de test d'affichage 100% simulé, cf.
`README_AMOLED_bringup.md` du projet principal pour tout l'historique
de calibration écran/tactile) -- devenu le **vrai firmware**, porté
depuis `pigteam-chrono-tft` (GPS, détection circuit/tour, sessions,
batterie, stockage SD réels). **Compile et tourne sur le vrai board**
(GPS actif, batterie réelle, circuits chargés).

## État actuel

| Brique | Statut |
|---|---|
| GPS (`GpsManager`, Quectel LC76G) | ✅ réel |
| Détection circuit/tour (`CourseManager`) | ✅ réel |
| Géofencing, mode manuel, capture nouveau circuit | ✅ réel |
| Sessions (`/sessions.csv`, LittleFS) | ✅ réel |
| Batterie (ADC interne) | ✅ réel |
| Stockage SD (`SdLogStorage`, SDMMC) | ✅ réel |
| WebServerManager (WiFi, sessions, sauvegarde/restauration) | ✅ réel |

## CourseManager -- vendoré depuis DovesLapTimer

`lib/DovesLapTimer/` contient la vraie lib publique
(github.com/TheAngryRaven/DovesLapTimer, branche `master`) -- pas un
module custom. `CourseManager` fait partie de cette lib (orchestre
plusieurs `DovesLapTimer` + `CourseDetector` + `WaypointLapTimer`,
cf. son header pour l'API complète). Dépendance requise :
`hideakitai/ArxTypeTraits` (traits de type C++, cf. `platformio.ini`).

## Différences avec le firmware TFT d'origine

- **SD en SDMMC**, pas SPI (`SdLogStorage.cpp` adapté -- interface
  externe identique, `initSdLogStorage()` sans `csPin`/`SPIClass&`,
  inutiles en SDMMC ici).
- **Batterie** : ADC interne du board (`adc_bsp`), pas de pont diviseur
  externe -- `readBatteryPercent()` réutilise la même courbe
  tension→pourcentage que le TFT, juste une source de tension
  différente.
- **Écran** : LVGL (tactile + encodeur) au lieu d'Adafruit_GFX/
  encodeur seul -- toute la logique GPS/CourseManager/sessions reprise
  à l'identique, seul l'affichage change.
- **Navigation** : anneau Circuit → Nouveau circuit → Connexion →
  Session → Réglages (encodeur + swipe tactile + tap direct sur les
  listes), pas le menu à 5 entrées du TFT -- cf. commentaire d'en-tête
  de `main.cpp` pour le détail exact.

## Build

```
pio run -t uploadfs   # envoie data/circuits.csv sur la LittleFS -- OBLIGATOIRE
                       # avant le premier boot (sinon 0 circuit connu,
                       # mode proximite "Lap Anything" uniquement)
pio run -t upload      # flashe le firmware
```

`data/circuits.csv` = copie directe du fichier réel fourni (8 circuits
PIGTEAM). Pour en ajouter/modifier, éditer ce fichier puis refaire
`uploadfs` (pas encore de page web `/circuits` pour l'éditer à chaud --
cf. Prochaines étapes).

## Pièges de compilation rencontrés

- **`hideakitai/ArxTypeTraits`** (dépendance de DovesLapTimer) : la
  version épinglée initialement (`^0.3.5`) n'existe pas au registre
  PlatformIO (dernière réelle : `0.3.2`) -- retirer toute contrainte de
  version dans `lib_deps` (juste `hideakitai/ArxTypeTraits`) resout.
- Variables/fonction GPS (`gpsFixStatus`/`gpsNumSVs`/`gpsSpeedKmh`/
  `gpsUpdateFromLiveData()`) supprimées par erreur lors du remplacement
  en bloc de l'ancienne couche de simulation -- restaurées juste après
  les includes.

## Piège rencontré : crash au démarrage WiFi après changement de partitions

Après passage à `partitions_ota_16mb.csv` (support OTA), premier essai
du point d'accès WiFi (`WebServerManager`) -> crash (`Guru Meditation
Error: LoadProhibited`) juste après le "WiFi actif..." affiché en
Serial. Deux causes distinctes traitées :

1. **Race condition LVGL** (corrigée par prudence, même si pas
   confirmée comme la cause principale) : `WiFi.softAP()`/
   `stopDownloadMode()` étaient appelés directement depuis des
   callbacks tactiles LVGL (déjà exécutés avec le mutex LVGL tenu par
   la tâche de rendu) -- une opération radio lente sous ce mutex gèle
   tout l'affichage/tactile pendant sa durée. `handlePush()`/
   `handleBack()`/rotation encodeur (appelés depuis `loop()`, hors
   mutex) appelaient aussi `lv_scr_load()` sans jamais le prendre.
   **Fix** : démarrage/arrêt WiFi différés via un simple flag, traités
   dans `loop()` hors de tout verrou ; `handlePush()`/`handleBack()`/
   rotation désormais protégés par `lvglLock()`.
2. **Fuite de descripteur de fichier confirmée et corrigée** :
   `forEachGpsLogFile()` (WebServerManager.cpp) ouvrait le répertoire
   racine (`root.openNextFile()`) sans jamais le refermer, ni fermer
   chaque fichier individuel avant de passer au suivant -- corrigé
   (`root.close()` + `f.close()` a chaque iteration).
3. **Bug de corruption de tas -- IDENTIFIÉ ET RÉSOLU** : plantage
   reproductible (`assert failed: ... tlsf.c`, détecteurs internes de
   l'ESP-IDF) tôt après le démarrage WiFi, avec de vraies sessions
   réelles (restaurées depuis une sauvegarde `/backup`, y compris des
   sessions importées via `/import/racechrono`). Investigation très
   approfondie avant d'isoler la vraie cause :
   - Premier symptôme : `handleHomePage()` (page d'accueil du serveur
     web) plantait dans `String::indexOf()` (via `lapTimeToMs()`) avec
     un vrai volume de sessions -- accumuler toute la page HTML
     (plusieurs dizaines de Ko, carte + tableau de tours + graphique
     SVG par session) dans une seule `String` faisait exploser le tas,
     déjà réduit par `WiFi.softAP()` (~70Ko libres seulement). **Fix** :
     page envoyée **en streaming** (`httpServer.sendContent()`, un
     morceau par session traitée) plutôt qu'accumulée puis envoyée
     d'un coup.
   - Le plantage a persisté ensuite ailleurs (`/lap`), ce qui a
     d'abord fait accusé à tort le gros bloc JS/SVG du graphique de
     tracé -- retiré une première fois sans effet, preuve que ce
     n'était pas lui.
   - **JTAG/GDB configuré et utilisé avec succès** (débogueur natif de
     l'ESP32-S3 via USB, driver WinUSB installé via Zadig) -- le bug
     disparaissait sous débogueur (signature de "Heisenbug" liée au
     minutage entre tâches FreeRTOS concurrentes), rendant le pas-à-pas
     classique inutilisable pour l'attraper "sur le fait".
   - Théories testées et infirmées en cours de route : fins de ligne
     CRLF/LF dans `sessions.csv`, `result.reserve()` sur les
     `std::vector` concernés (a même **aggravé** le symptôme),
     désactivation de la PSRAM pour `malloc()` par défaut.
   - **Cause réelle, confirmée par lecture précise du backtrace
     `addr2line`** : l'appel à `loadLapsForSession()` dans
     `handleLapTracePage()` (page `/lap`) -- utilisé uniquement pour
     calculer la navigation précédent/suivant entre tours, sans
     rapport avec le graphique JS accusé à tort au départ. La
     construction du `std::vector<LapDetail>` (copie d'éléments
     contenant des `String`) déclenchait la corruption, systématiquement,
     dès le premier appel après le démarrage WiFi.
   **Fix retenu** : `handleLapTracePage()` n'appelle plus
   `loadLapsForSession()` -- navigation précédent/suivant simplifiée en
   numéro-1/numéro+1 (sans vérification d'existence -- un lien vers un
   tour inexistant affiche une page vide, sans planter). Le graphique
   JS/SVG reste retiré (simplification jugée raisonnable au passage,
   remplacé par un lien de téléchargement du CSV complet pour analyse
   externe). **Confirmé stable au banc** après plusieurs tours consultés
   d'affilée. **`/compare` utilise toujours `loadLapsForSession()`** --
   même risque potentiel si le problème s'y reproduit, pas encore
   traité (mais le vrai correctif est désormais connu si besoin).

## Prochaines étapes

- `/compare` utilise toujours `loadLapsForSession()` (cf. bug de
  corruption de tas ci-dessus) -- même risque potentiel si le problème
  s'y reproduit ; le correctif (retirer l'appel, simplifier la
  navigation) est connu et prêt à appliquer si besoin.
- Page web `/circuits` pour éditer `circuits.csv` à chaud (actuellement
  uniquement modifiable via `uploadfs`).
- Tester en conditions réelles (roulage) : géofencing, détection de
  ligne, capture de nouveau circuit -- tout ça n'a été porté que par
  lecture de code, jamais testé sur GPS réel en mouvement à ce stade.
