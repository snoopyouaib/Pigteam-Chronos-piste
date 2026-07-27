# Chrono GPS moto piste -- variante TFT ST7789 (ESP32-S3-Tiny N8R8)

Chrono GPS piste PigTeam avec affichage TFT couleur en continu (tour en
cours qui défile en direct), navigation à l'encodeur EC11 + un seul
bouton. Même logique `CourseManager` (détection auto/proximité, 8
circuits) / logs par session / WiFi que la variante OLED (`lap_timer_firmware`)
-- seuls l'affichage et l'entrée changent. Si tu modifies un comportement
commun (`WebServerManager`, `GpsManager`, `SdLogStorage`, format
`circuits.csv`/`sessions.csv`...), pense à reporter le changement dans
les deux projets.

## Matériel

- **ESP32-S3-Tiny N8R8** (8MB flash / 8MB PSRAM octale, module USB
  amovible via nappe) -- montage final, ESP soudé directement sur le
  module TFT. Migré successivement depuis un ESP32-S3FH4R2 (4MB/2MB
  quad) puis un ESP32-S3-Zero N8R8 (mêmes 13 GPIO 1-13 + TX/RX) --
  le Tiny expose davantage de broches (cf. câblage ci-dessous), plus de
  changement de carte prévu.
- **Écran TFT ST7789 320x240** -- piloté par **Adafruit_GFX +
  Adafruit_ST7789**, pas TFT_eSPI (cf. section dédiée plus bas).
- **EC11** (encodeur rotatif + clic axe) -- navigation + validation.
- **1 bouton BACK** -- retour/menu (pas de CONFIRM dédié comme côté OLED :
  seulement 2 entrées physiques ici, PUSH et BACK).
- **GPS u-blox NEO-M9N ou NEO-M8N** en UART direct (UBX binaire,
  NAV-PVT à 10Hz) -- les deux fonctionnent avec ce firmware, le débit
  initial de connexion est confirmé par balayage au premier essai (cf.
  `debugBaudScan()`, désactivé par défaut une fois le bon débit connu).
- **Carte SD** -- réintroduite sur ce montage (bus SPI dédié, cf.
  `SdLogStorage.h`/section dédiée plus bas) pour les logs GPS détaillés,
  avec repli automatique sur LittleFS si absente/en panne.
- **Batterie** -- Lipo Rider Plus (cf. section Alimentation), tension
  monitorée par ADC.

### Câblage (ESP32-S3-Tiny N8R8 -- GPIO 1-18 + broches TX/RX dédiées)

| Signal | GPIO |
|---|---|
| Batterie (ADC1_CH0, pont diviseur 10k/10k depuis BAT) | 1 |
| GPS RX (← TX module) | 6 |
| GPS TX (→ RX module) | 5 |
| SD MOSI | 7 |
| SD MISO | 8 |
| SD SCLK | 9 |
| SD CS | 10 |
| TFT SCLK (SCL) | 11 |
| TFT MOSI (SDA) | 12 |
| TFT RST (RES) | 13 |
| TFT DC | 14 |
| TFT CS | 15 |
| TFT BL/BLK (rétroéclairage) | 16 |
| Encodeur CLK (A) | 17 |
| Encodeur DT (B) | 18 |
| BACK (K0) | TX (GPIO43, U0TXD -- chip pin 49 sur le boîtier QFN56) |
| Encodeur PUSH | RX (GPIO44, U0RXD -- chip pin 50) |

Les broches TX/RX (UART0) sont de simples GPIO comme les autres --
libres ici puisque le monitoring Série passe par l'USB natif
(`ARDUINO_USB_CDC_ON_BOOT=1`), récupérées pour BACK/PUSH plutôt que pour
le GPS (contrairement au plan précédent sur S3-Zero). Le GPS et la SD
occupaient initialement les mêmes GPIO (5 partagé) dans une première
itération de ce plan -- corrigé en donnant un bus SPI entièrement dédié
à la SD (7-10) plutôt que de partager MOSI/SCLK avec l'écran comme sur
le S3-Zero, la carte Tiny ayant assez de broches libres pour se le
permettre.

**Piège rencontré au câblage** : GPS totalement silencieux (aucun octet
reçu, à aucun débit testé) malgré une continuité électrique confirmée
bonne sur GPIO5/6 et le module actif (LED clignotante) -- cause :
**RX/TX inversés** au connecteur. Contrairement à un mauvais débit (qui
produit toujours des octets, juste illisibles), un croisement RX/TX
produit un silence total quel que soit le débit testé -- c'est le signe
distinctif à chercher dans ce cas. Résolu en intervertissant simplement
les deux fils, sans toucher au code.


## Écran : pourquoi Adafruit_ST7789 et pas TFT_eSPI

TFT_eSPI plante systématiquement (`Guru Meditation StoreProhibited` dans
`begin_tft_write()`) sur ESP32-S3 avec les cœurs Arduino-ESP32 récents
(>2.0.14) -- bug connu et non résolu à ce jour (plusieurs issues
ouvertes sur le dépôt Bodmer/TFT_eSPI, mêmes symptômes sur différents
drivers/pins/cartes). Adafruit_ST7789 fonctionne correctement sur ces
mêmes cœurs.

**Points durs rencontrés et corrigés :**
- **Inversion matérielle** : ce panneau ST7789 démarre en mode inversé
  par défaut chez Adafruit_ST7789 (courant sur les dalles IPS) --
  `tft.invertDisplay(false)` corrige ça une fois pour toutes dans
  `setup()`. Identifié via un test de bandes de couleurs pures (blanc→noir,
  rouge→cyan, vert→magenta, bleu→jaune = signature exacte d'une
  inversion bit à bit).
- **`startWrite()`/`endWrite()` imbriqués** cassent l'affichage (écran
  blanc figé) avec cette bibliothèque -- un seul niveau (non imbriqué)
  reste sans risque, cf. `pushCanvasToDisplay()`.
- **Double buffering** (`GFXcanvas16`, 320x240x2 octets = 150KB, alloué
  automatiquement en PSRAM) : tous les écrans dessinent dans ce canvas
  hors-écran, envoyé en un seul bloc SPI à la fin de chaque rendu
  (`pushCanvasToDisplay()`). Élimine le clignotement -- les tentatives
  précédentes (largeur de texte fixe, redessin conditionnel `uiDirty`)
  réduisaient la fréquence des dessins visibles mais ne l'éliminaient
  pas complètement, contrairement au vrai double buffering utilisé côté
  OLED/Pendule Paddock.

## Splash de démarrage

`splash_pigteam.h` : export brut Piskel en **ARGB8888** (tableau de
`uint32_t`, format réel `0xAABBGGRR` -- Piskel stocke R,G,B,A en
mémoire, ce qui inverse R et B une fois relu comme un entier 32 bits sur
ce processeur little-endian). Déjà aux bonnes dimensions/orientation
paysage (284x240), affiché directement sans rotation logicielle. Pour
regénérer ce fichier : exporter depuis Piskel au format C/ARGB8888,
284x240, déjà dans le bon sens.

## Alimentation et monitoring batterie

**Chaîne d'alimentation** : Lipo Rider Plus (Seeed, chip ETA9740 --
charge switching 3A + boost synchrone 5V/2.4A + fuel gauge 4 LEDs) --
sortie 5V header (pin 4) vers l'ESP32-S3, GND commun.

- **Coupure marche/arrêt** : un poussoir étanche à verrouillage
  mécanique en série sur le fil **+** entre le connecteur JST 2.0 et la
  batterie -- coupe tout en amont (charge + boost), plus robuste et
  plus simple à actionner avec des gants que le petit switch on-board
  du module (laissé en position ON en permanence). Attention à ne
  couper que le **+**, jamais le **-**.
- **Batterie** : le Lipo Rider Plus charge à **3A fixe** (résistance ISET
  soudée, non ajustable sans dessouder). Toutes les batteries du stock
  PigTeam (1000, 2000, 3500, 4000mAh) sont des **1C** -- le taux de
  charge réel à 3A dépend donc fortement de la capacité choisie :

  | Capacité | Taux de charge réel à 3A | Compatible avec ce chargeur ? |
  |---|---|---|
  | 1000mAh | 3C | ❌ trois fois la limite officielle |
  | 2000mAh (EEMB LP103454 testée) | 1.5C | ❌ 1.5x la limite |
  | 3500mAh | ~0.86C | ✅ dans les clous |
  | 4000mAh | 0.75C | ✅ confortable |

  **3700-4000mAh recommandé** pour cette raison (en plus d'être
  nécessaire pour l'autonomie avec le TFT, plus gourmand qu'un OLED) --
  les 1000/2000mAh se rechargent trop vite pour leur spec avec ce
  chargeur précis (pas forcément dangereux à l'usage ponctuel, les
  protections internes de la cellule interviennent généralement avant
  un incident, mais ça sort du cadre garanti par le fabricant et use la
  batterie prématurément) ; à réserver à un usage annexe avec un
  chargeur adapté à leur capacité, pas sur ce montage.

**Monitoring tension (GPIO1, ADC1_CH0)** : pont diviseur **10k/10k**
entre le pin BAT du Lipo Rider Plus et GND, jonction sur GPIO1 (ramène
3.0-4.2V vers 1.5-2.1V, large marge sous les 3.3V de l'ADC). **10k/10k
et pas 100k/100k** : en 100k, l'impédance de source est trop élevée
pour le convertisseur ADC de l'ESP32-S3 (le condensateur d'échantillonnage
interne n'a pas le temps de se charger), ~3.4% d'erreur constatée au
banc -- corrigé en passant à 10k/10k (~0.05% d'écart résiduel, jugé
suffisant pour un simple indicateur, sans facteur de calibration
logiciel). Courant de fuite du diviseur : ~380µA sous 3.8V, négligeable
sur une 3700/4000mAh.

Tension et pourcentage (courbe Li-Ion approximative par paliers,
`batteryVoltageToPercent()`) affichés en haut à droite de l'écran
statut (vert >50%, orange 20-50%, rouge ≤20%) et via la commande Serial
**`b`**.

## GPS

Repris à l'identique de la variante OLED : UBX binaire, NAV-PVT à 10Hz.
`GPS_INITIAL_BAUD` est réglé sur le débit confirmé au banc pour le
module actuellement câblé -- si le module change et que "GPS --
(recherche)" reste bloqué, réactiver `debugBaudScan()` dans `initGps()`
pour rebalayer les débits courants (9600/19200/38400/57600/115200/4800)
et lire ce qui ressort en clair (`$GPGGA`, `$GNRMC`...). Si le balayage
ne reçoit **rien du tout** à aucun débit (pas même du charabia), pense
au croisement RX/TX avant d'incriminer le débit -- cf. câblage plus haut.

## SD -- stockage des logs GPS détaillés

Module `SdLogStorage.h`/`.cpp` (partagé avec la variante OLED, cf.
WebServerManager plus bas) -- bus SPI **dédié** (pas partagé avec
l'écran, cf. câblage). `initSdLogStorage(SD_CS, sdSPI)` tente
`SD.begin()` au démarrage ; en cas d'échec (carte absente, mal câblée,
en panne), repli automatique et silencieux sur LittleFS pour les logs
GPS détaillés -- la SD reste entièrement facultative, rien ne bloque au
démarrage si elle n'est pas présente. Le carnet cumulatif
(`sessions.csv`) et `circuits.csv` restent **toujours** sur LittleFS,
quel que soit l'état de la SD.

**Piège de compilation rencontré** : `<SD.h>` amène PlatformIO à
installer par défaut un paquet tiers du registre (`arduino-libraries/SD`
ou `adafruit/SD`, homonymes) incompatible avec l'ESP32-S3 (`#error
Architecture or board not supported`), au lieu de la version intégrée
au framework Arduino-ESP32 (compatible, hérite de `fs::FS`). `lib_ignore
= SD` n'aide pas (exclut les deux versions indifféremment, y compris la
bonne). Fix retenu dans `platformio.ini` : déclarer explicitement la lib
en pointant sur le dossier framework via
`file://${platformio.packages_dir}/framework-arduinoespressif32/libraries/SD`
-- plus d'ambiguïté, plus de téléchargement pour ce nom.

## CourseManager -- détection, géofencing, capture de circuit

- **Détection auto** : `CourseManager` (lib DovesLapTimer) compare la
  position à `circuits.csv` (jusqu'à 8 circuits actifs) et bascule en
  mode proximité ("Lap Anything") si rien ne correspond après quelques
  tours.
- **Géofencing** : au tout premier fix GPS valide, si un circuit actif
  est à moins de 15km (`GEOFENCE_MAX_DISTANCE_M`), il est activé
  directement par distance à vol d'oiseau -- pas besoin de boucler un
  tour pour lever l'ambiguïté, contrairement à l'algorithme natif de la
  lib qui ne connaît pas la position réelle des circuits.
- **Capture de nouveau circuit** ("New track (capture)", racine du menu
  principal) : capture en deux temps -- le cap est estimé dès que le
  waypoint interne est posé (historique GPS court terme, ~4s), puis la
  ligne A/B perpendiculaire est écrite dans `circuits.csv`
  (`active=0`, nom `Nouveau_AAAAMMJJ_HHMMSS`) seulement au premier tour
  validé. Suspend le géofencing pendant la capture (sinon il réactiverait
  un circuit connu proche avant que le tour ne se termine). Le nouveau
  circuit reste **inactif** -- à activer depuis `/circuits` (page web).

## Logs et enregistrement

- `/log_AAAAMMJJ_HHMMSS.csv` : détail GPS complet de la session (1 ligne
  par fix), créé/fermé à chaque REC.
- `/sessions.csv` : carnet cumulatif, 1 ligne par tour terminé, délimité
  par des marqueurs `# session demarree/arretee`.
- **PUSH** (clic encodeur) démarre/arrête l'enregistrement -- manuel
  dans les deux sens (pas d'auto-démarrage à la détection d'un circuit).
- Commandes Serial en complément du web : `l` (lister les logs), `d`
  (dump le dernier), `s` (dump le carnet), `c` (effacer les logs GPS),
  `b` (tension/pourcentage batterie -- déjà repris sur l'écran statut,
  pratique pour vérifier sans allumer l'écran).

## Écran et navigation

**Écran statut** (par défaut) : GPS (fix/satellites), pourcentage
batterie (haut droite, vert/orange/rouge), nom du circuit/état de
détection (jaune + "CAPTURE NEW TRACK" pendant une capture armée), tour
en cours en gros, dernier tour, meilleur tour, tours + vitesse, REC.

**BACK** ouvre le **menu principal** (5 entrées, rotation + PUSH pour
valider, BACK pour remonter d'un niveau) :

- **Circuit** -- Auto (détection) + liste des circuits actifs.
- **Connexion** -- état GPS (fix/satellites) + circuit actif.
- **Session** -- liste des sessions enregistrées (8 plus récentes pour
  l'instant, pas de défilement au-delà -- à revoir si ça devient
  limitant à l'usage), sélection puis détail tour par tour (dernier
  tour affiché en premier, meilleur temps recalculé sur toute la
  session -- pas une simple colonne du CSV, qui n'est qu'un
  best-so-far progressif).
- **Réglages** -- raccourci WiFi téléchargement.
- **New track (capture)** -- arme la capture de nouveau circuit (cf.
  section dédiée plus haut).

Pas de mode Démo (jamais porté ici, contrairement à l'OLED -- jugé
inutile, économise de la place).

## WebServerManager -- module partagé avec la variante OLED

`WebServerManager.h`/`.cpp` sont **strictement identiques** entre les
deux projets (copie directe, pas d'adaptation) -- toute évolution future
d'un côté se reporte à l'identique de l'autre en recopiant les deux
fichiers. Point d'accès WiFi (réseau ouvert, `ChronoMotoTFT`) + serveur
HTTP avec les pages : Sessions (téléchargement + espace disque),
Comparer (deux tours superposés en SVG), **Circuits** (activer/éditer/
supprimer -- c'est ici qu'on finalise un circuit capturé sur le terrain),
Statut, `/debug` (nettoyage du carnet, non listée dans la nav), Firmware
(mise à jour OTA).

Callbacks fournis par `main.cpp` : `flushLogsCallback` (flush le log
GPS en cours avant téléchargement), `getStatusCallback` (état GPS/
circuit/REC/tours pour la page Statut). `bleStop`/`bleRestart` passés à
`nullptr` (pas de BLE sur ce projet).

**SD réintroduite** sur ce montage (cf. section dédiée plus haut) --
`begin()` reçoit `*gpsLogFs` (SD si détectée au démarrage, LittleFS en
repli sinon), fourni par `SdLogStorage.h`. `WebServerStatusInfo` inclut
désormais `hasSeparateLogsFs`/`logsFsLabel`/`logsFsUsedBytes`/
`logsFsTotalBytes` pour afficher une barre d'espace disque SD distincte
de LittleFS sur la page Sessions, quand elle est active.

### Mise à jour OTA

`/update` (page Firmware) : upload du `firmware.bin` généré par
`pio run`, flash direct sur la partition OTA inactive, redémarrage
automatique. Nécessite le schéma de partitions dédié (déjà en place,
cf. `partitions_ota.csv`) -- **un premier flash complet par USB est
obligatoire après tout changement de ce fichier** (l'OTA ne peut prendre
le relais qu'une fois le nouveau schéma en place sur la carte), et ça
réinitialise la LittleFS au passage (donc `circuits.csv` aussi --
`uploadfs` à refaire).

## Mémoire (8MB flash du N8R8)

```
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,   (3MB)
app1,     app,  ota_1,   0x310000, 0x300000,   (3MB)
spiffs,   data, spiffs,  0x610000, 0x1F0000,   (~1.94MB, LittleFS)
```

Large marge par rapport à la taille actuelle du firmware et aux besoins
de logs -- pas de pression mémoire connue à ce jour. À revoir si les
sessions/logs s'accumulent sans purge régulière.

## Build

```
pio run                 # compiler
pio run -t upload       # flasher par USB (obligatoire au premier flash, ou après changement de partitions)
pio run -t uploadfs     # envoyer data/circuits.csv sur la LittleFS
```

`data/circuits.csv` doit exister avant `uploadfs` -- c'est la source de
vérité pour les circuits actifs au démarrage (`loadActiveCircuitsIntoTracks()`
dans `main.cpp`, s'arrête à 17 champs et ignore la 18e colonne `locked`,
gérée uniquement côté `WebServerManager`).

## Historique -- ce qui a changé en route

- Départ sur ESP32-S3FH4R2 (4MB/2MB quad), migré vers ESP32-S3-Zero N8R8
  (8MB/8MB octale) en cours de projet -- `platformio.ini` et
  `partitions_ota.csv` mis à jour en conséquence.
- TFT_eSPI abandonné au profit d'Adafruit_ST7789 (bug de plantage non
  résolu sur ESP32-S3).
- Plusieurs itérations sur le splash (rotation logicielle abandonnée au
  profit d'une image déjà générée dans le bon sens/format).
- Menu circuit simplifié : WiFi et capture de circuit déplacés vers le
  menu principal (Réglages et racine respectivement), le sous-menu
  Circuit ne contient plus que Auto + la liste des circuits.
- `WebServerManager` complet importé depuis la variante OLED (remplace
  la version réduite maison), synchronisé avec le nettoyage SD fait des
  deux côtés suite à la migration N8R8.
- Clignotement d'écran éliminé via double buffering (`GFXcanvas16`),
  après plusieurs tentatives partielles (largeur de texte fixe,
  redessin conditionnel) qui réduisaient sans éliminer le problème.
- Monitoring batterie ajouté (GPIO1, dernier GPIO libre du plan
  S3-Zero) : pont diviseur d'abord en 100k/100k, puis corrigé en
  10k/10k suite à une erreur de mesure liée à l'impédance de source
  trop élevée pour l'ADC ESP32-S3. Affiché sur l'écran statut +
  commande Serial `b`.
- `GpsManager.h`/`.cpp` extrait de `main.cpp` en module partagé avec
  l'OLED (même principe que `WebServerManager`) -- au passage, portage
  du correctif de validité date/heure (bits `validDate`/`validTime` de
  la trame NAV-PVT) découvert côté OLED sur le terrain à
  Croix-en-Ternois (log parfois nommé avec une date provisoire fausse
  malgré un fix 3D déjà bon).
- Migration finale vers l'**ESP32-S3-Tiny N8R8** (module USB amovible,
  ESP soudé directement sur le module TFT) -- nouveau plan de brochage
  complet (GPIO1, 5-18, TX/RX), SD réintroduite avec bus SPI dédié
  (plutôt que partagé avec l'écran comme envisagé un temps sur
  S3-Zero), `platformio.ini` corrigé pour forcer la lib SD du framework
  plutôt qu'un paquet tiers incompatible ESP32-S3 installé par défaut.
  GPS temporairement désinstallé/réinstallé sur GPIO5/6 en cours de
  route (conflit initial avec la SD, résolu en donnant un bus dédié à
  cette dernière) -- silence radio total au premier essai, résolu par
  inversion RX/TX au connecteur (pas un problème de débit).

## Prochain chantier -- affichage lisible en roulant

L'écran statut actuel convient pour le développement/debug sur banc,
mais pas encore pensé pour une lecture rapide en conditions réelles
(vibrations, gants, casque, plein soleil). À retravailler : taille de
police mini lisible d'un coup d'œil, priorisation de l'info utile
pendant un tour (temps/delta) par rapport à l'info de diagnostic
(GPS/satellites, plus utile dans le menu Connexion que sur l'écran
principal), contraste/lisibilité en plein soleil, éventuellement un
mode nuit. Le double buffering déjà en place permet d'itérer sur la
mise en page sans se soucier du clignotement.
