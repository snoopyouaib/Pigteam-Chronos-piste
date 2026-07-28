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
| WebServerManager (WiFi, sessions, sauvegarde/restauration, OTA) | ✅ réel |

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
- **Écran** : LVGL (tactile + PUSH/BACK) au lieu d'Adafruit_GFX/
  encodeur seul -- toute la logique GPS/CourseManager/sessions reprise
  à l'identique, seul l'affichage change. Rotatif EC11 physiquement
  retiré le 27/07, remplacé par un bouton poussoir simple sur la même
  broche (cf. section dédiée plus bas) -- BACK reste un bouton séparé.
- **Navigation** : anneau Circuit → Nouveau circuit → Connexion →
  Session → Réglages (swipe tactile pour tourner l'anneau, tap direct
  sur les listes -- plus de rotation physique depuis le retrait de
  l'encodeur), pas le menu à 5 entrées du TFT -- cf. commentaire
  d'en-tête de `main.cpp` pour le détail exact.

## Build

```
pio run -t uploadfs   # envoie data/circuits.csv sur la LittleFS -- OBLIGATOIRE
                       # avant le premier boot (sinon 0 circuit connu,
                       # mode proximite "Lap Anything" uniquement)
pio run -t upload      # flashe le firmware
```

`data/circuits.csv` = copie directe du fichier réel fourni (8 circuits
PIGTEAM), utilisée pour le tout premier boot. Modifications ultérieures
possibles à chaud via la page web `/circuits` (liste, ajout, édition,
suppression, activation/désactivation -- persisté sur LittleFS) --
`uploadfs` ne reste utile que pour repartir d'un jeu de circuits vierge
sur une nouvelle carte.

## Contrôles REC / arrêt d'enregistrement (27-28/07)

Un même symptôme ("l'enregistrement redémarre tout seul") a nécessité
plusieurs corrections successives, en réalité **deux bugs distincts et
indépendants** :

**1. Circuit jamais désarmé après un stop.** Une fois le circuit détecté
une première fois (`courseManager` → `DETECT_STATE_DETECTED`), rien ne
le réinitialisait à un simple `stopRecording()` -- le bouton REC restait
donc actif indéfiniment tant qu'on restait dans le rayon de géofencing
(15 km, `GEOFENCE_MAX_DISTANCE_M`), y compris des heures plus tard et
loin de tout roulage. Corrigé par un écran de confirmation à deux
temps, inspiré de RaceChrono :
- **PUSH** pendant l'enregistrement → pause (fichier fermé, circuit
  toujours armé), ouvre l'écran "Enregistrement en pause".
- **PUSH** sur cet écran → reprend immédiatement.
- **BACK** sur cet écran → arrêt définitif, désarme le circuit
  (`activateAutoMode()`, reset complet du `courseManager`).
- **Timeout de sécurité** (`CONFIRM_STOP_TIMEOUT_MS`, 5 min) : arrêt
  définitif automatique si on ne choisit pas -- évite qu'un écran
  oublié reste armé indéfiniment.
- BACK sur l'écran Statut reste un simple retour au menu Circuit,
  qu'un enregistrement soit en cours ou non (identique à TFT/OLED).

**2. Faux contact tactile capacitif, confirmé au Serial** (traces du
type `REC via tactile` déclenchées sans aucun contact humain, boîtier
immobile). Reproductible à volonté, y compris après correction du bug
n°1 -- donc bien une cause matérielle distincte, pas juste un symptôme
du même bug. **Le rotatif EC11 a été physiquement retiré et remplacé
par un simple bouton poussoir** (même broche, GPIO10) ; le tactile a
été retiré de tout le chemin REC/pause/reprise (plus aucun widget
cliquable sur l'écran Statut ni sur l'écran de confirmation -- juste du
texte, "PRESS REC" / "PUSH pour reprendre" / "BACK pour arreter", ces
deux derniers dans la même taille -- cf. section "Ajustements
d'affichage" plus bas). Seuls PUSH et BACK
(contacts mécaniques, insensibles au bruit électrique) contrôlent
désormais l'enregistrement. Le tactile reste actif pour tout le reste
(navigation anneau par swipe, sélection dans les listes par tap).

**Piège annexe rencontré en cours de route** : un bug classique
d'arithmétique non signée faisait échouer le timeout de l'écran de
confirmation en quelques millisecondes au lieu de 5 minutes --
`nowMs` était capturé une seule fois en haut de `loop()`, avant que
`confirmStopEnteredMs` ne soit réglé plus tard dans la même itération
(dans `handlePush()`), rendant `nowMs - confirmStopEnteredMs`
techniquement négatif -- et donc, en `unsigned long`, un nombre énorme
qui dépassait aussitôt le seuil. Fix : capturer un `millis()` frais au
moment précis de la vérification plutôt que de réutiliser le `nowMs` du
haut de la boucle. TFT/OLED n'ont jamais eu ce bug (ils utilisaient déjà
un `millis()` frais à cet endroit).

## Ajustements d'affichage (28/07)

Petite série de retouches suite à la calibration sur le banc
(`display_only_191`, testées là-bas avant report ici) :

- **Police agrandie** sur l'écran Statut : "Dernier", "Best" et "Tours"
  passent de `medium_34` à `bold_38` (un cran au-dessus, jugé trop
  petit à l'usage).
- **Écran "Enregistrement en pause"** : le texte "PRESS BACK pour stop
  definitif" devient "BACK pour arreter", raccourci et remonté à la
  même taille (`bold_38`) que "PUSH pour reprendre" (était `medium_26`,
  nettement plus petit que sa contrepartie).
- **Vitesse max par tour** : `checkLapCompletion()` suit désormais la
  vitesse GPS max atteinte pendant chaque tour (`currentLapMaxSpeedKmh`,
  mis à jour à chaque trame dans `logGpsRow()`), écrite comme 7e champ
  dans `/sessions.csv` et affichée dans le détail d'une session
  (`Tour 3 : 1:23.456  (best)  Vmax 87`). Rétrocompatible : les
  anciennes sessions à 6 champs (sans vitesse max) s'affichent sans
  planter, juste sans le "Vmax".
- **Hints de navigation supprimés** : les petits messages en haut à
  droite de chaque écran de l'anneau (Circuit, Connexion, Sessions,
  Réglages, WiFi, Nouveau circuit -- ex. "Tap: choisir BACK: statut")
  ont été retirés, ainsi que la fonction `createHint()` devenue
  inutile. Le "PRESS REC" clignotant en bas à droite de l'écran Statut
  est conservé (ce n'est pas un hint de navigation mais l'invite
  fonctionnelle qui indique quand appuyer sur REC).

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
3. **Bug résiduel de corruption de tas -- cause exacte non trouvée,
   contourné par simplification de `/lap`** : plantage reproductible
   (`assert failed: ... tlsf.c`, détecteurs internes de l'ESP-IDF) sur
   les pages construisant une grosse réponse HTML/JS en une seule fois
   (détail d'un tour, avec son graphique de tracé), tôt après le
   démarrage WiFi. Investigation très approfondie, plusieurs pistes
   testées :
   - `addr2line` (décodage `Guru Meditation`) a localisé successivement
     plusieurs points d'appel fautifs différents selon les essais
     (`lapTimeToMs()`, `String::changeBuffer()`, le constructeur de
     copie de `LapDetail`...) -- signe que la corruption ne vient pas
     d'un point de code precis, mais se manifeste où que ce soit une
     fois le tas déjà abîmé.
   - **JTAG/GDB configuré et utilisé avec succès** (débogueur natif de
     l'ESP32-S3 via USB, driver WinUSB installé via Zadig) -- mais le
     bug **disparaît complètement** une fois le débogueur branché
     ("Heisenbug" : problème de minutage entre tâches FreeRTOS
     concurrentes, perturbé juste assez par le JTAG pour l'éviter). Le
     pas-à-pas classique ne peut donc pas l'attraper "sur le fait".
   - **Streaming (chunked) des réponses HTTP** (`/`, `/lap`, `/compare`)
     -- réduit nettement la fréquence du plantage (est passé de "plante
     systématiquement" à "tient 2-3 requêtes avant de planter"), sans
     l'éliminer complètement.
   - **Théories testées et infirmées** : mélange de fins de ligne
     CRLF/LF dans `sessions.csv` (fichier normalisé en LF pur, même
     plantage) ; `result.reserve()` sur les `std::vector` concernés
     (a **aggravé** le problème -- une grosse allocation d'un coup
     échoue plus vite qu'une accumulation progressive, signe que le tas
     est probablement déjà corrompu avant même que notre code ne
     s'exécute) ; désactivation de la PSRAM pour le `malloc()` par
     défaut (`heap_caps_malloc_extmem_enable(0)`, piste documentée sur
     d'autres projets ESP32-S3 -- testée puis retirée, aucun effet).
   - Recherche documentaire : ce type d'assertion (`tlsf.c`,
     `block_locate_free`/`block_trim_free`) est un problème **connu et
     répandu sur toute la plateforme ESP32/ESP32-S3**, pas spécifique à
     ce projet -- de nombreux rapports similaires sur le dépôt
     `espressif/arduino-esp32` et le forum ESP32, sans solution
     universelle.
   **Contournement final retenu** : la page `/lap` (détail d'un tour)
   a été **simplifiée** -- son gros bloc JS/SVG (graphique de vitesse
   colorée, détection freinages/relances) a été retiré, elle affiche
   désormais juste le temps du tour, la navigation précédent/suivant et
   un lien de téléchargement du CSV complet pour analyse externe.
   `/compare` garde son graphique (pas encore simplifiée, même risque
   potentiel si le problème s'y reproduit un jour).

## Prochaines étapes

- Page web `/circuits` pour éditer `circuits.csv` à chaud : ✅ fait,
  cf. section Build ci-dessus.
- Isoler formellement la cause du bug mémoire de `WebServerManager.cpp`
  (`loadLapsForSession()`, cf. section "Pièges de compilation" point 3)
  via JTAG (`debug_tool`/`debug_init_break` déjà en place dans
  `platformio.ini`) -- contourné pour l'instant (tableau détail-par-tour
  retiré de la page d'accueil), jamais réellement corrigé.
- Tester en conditions réelles (roulage) : géofencing, détection de
  ligne, capture de nouveau circuit -- tout ça n'a été porté que par
  lecture de code, jamais testé sur GPS réel en mouvement à ce stade.
