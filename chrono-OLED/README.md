# Chrono GPS moto piste -- variante OLED + encodeur (ESP32-S3-Tiny N8R8)

Pour la moto du fils. Pas d'affichage live -- il reste concentré sur le
pilotage. L'écran OLED n'affiche que **dernier tour / meilleur tour**,
mis à jour uniquement quand un tour se termine.

Même logique `CourseManager` (8 circuits) / logs par session / WiFi que
le firmware TFT principal (`lap_timer_firmware`) -- seuls l'affichage et
l'entrée changent. Si tu modifies un comportement commun (logs, WiFi...),
pense à reporter le changement dans les deux projets.

**GPS** : module u-blox NEO-M8N en UART direct (UBX binaire, NAV-PVT à
10Hz) -- remplace le RaceBox/BLE des premières versions (cf. section
dédiée "Migration RaceBox → NEO-M8N" plus bas pour le pourquoi et le
détail technique). Un NEO-M9N a été testé en cours de route sur ce
montage (cf. "Migration NEO-M8N → NEO-M9N → retour au NEO-M8N" plus bas)
mais écarté suite à des soucis de négociation de débit -- le NEO-M8N
reste le module de référence sur cette variante.

## Matériel

- **ESP32-S3-Tiny N8R8** (Waveshare, 23.5×18mm, 8MB flash / 8MB PSRAM octale)
  -- migration depuis l'ESP32-S3-Zero (4MB/2MB), même firmware sinon.
  Programmation via l'adaptateur USB-C/FPC du kit (réutilisable, pas
  besoin d'en racheter un par carte Tiny nue).
- **OLED 1.3" + encodeur EC11** combinés sur un même module (SH1106/SSD1315,
  128×64, I2C) -- celui avec `GND VCC SCL SDA RES DC CS BLK A B PUSH K0`

### Câblage (à adapter si différent du tien)

Numérotation choisie dans l'ordre physique du connecteur du module
(CONFIRM, SDA, SCL, PUSH, A, B, BACK) -- les fils se suivent sans
croisement au soudage. Décalée de +5 par rapport à l'ESP32-S3-Zero
(7-13 → 12-18) pour la migration vers le Tiny ; GPS inchangé.

| Signal | GPIO |
|---|---|
| CONFIRM | 12 |
| OLED SDA | 13 |
| OLED SCL | 14 |
| Encodeur PUSH (clic axe) | 15 |
| Encodeur CLK (A) | 16 |
| Encodeur DT (B) | 17 |
| BACK | 18 |
| GPS RX (← TX module) | 4 |
| GPS TX (→ RX module) | 5 |
| SD CS | 7 |
| SD MOSI | 8 |
| SD MISO | 9 |
| SD SCLK | 10 |

GPIO choisis pour éviter 19/20 (USB natif) et 33-37 (PSRAM octale du N8R8,
réservés en interne, indisponibles en externe). **Carte SD réintégrée**
sur ce montage (cf. section dédiée plus bas) -- bus SPI entièrement dédié
(7/8/9/10), aucun partage possible avec l'écran comme sur la variante TFT
(celui-ci est en I2C ici, pas de bus SPI existant à réutiliser). GPIO
1, 2, 3, 6, 11 restent libres pour un usage futur (ex. la variante TFT
ST7789 avec encodeur intégré, 2 entrées seulement -- cf. notes projet).

## Particularités de l'ESP32-S3-Tiny (vs le DevKit N16R8 du firmware TFT)

- **Pas de puce USB-UART** (CH343) -- port USB natif uniquement (adaptateur
  FPC/USB-C séparé). D'où `ARDUINO_USB_CDC_ON_BOOT=1` dans `platformio.ini`
  (l'inverse du firmware TFT).
- **Bouton BOOT** : pas nécessaire à tenir en pratique pour flasher sur ce
  setup (confirmé par le test) -- contrairement à ce que la doc générale
  laissait craindre. Si jamais le flash échoue un jour, c'est le premier
  réflexe à essayer.
- **8MB flash / 8MB PSRAM** (N8R8) -- PSRAM octale, nécessite
  `board_build.arduino.memory_type = qio_opi` explicite dans
  `platformio.ini` (confirmé au banc : sans ce flag, `ESP.getPsramSize()`
  renvoie 0 malgré le chip N8R8). Table de partitions dédiée
  (`partitions_ota_8mb.csv`) : app0/app1 gardées à la même taille que sur
  le S3-Zero, tout l'espace libéré par le passage 4MB→8MB va dans la
  partition `spiffs` (LittleFS) -- ~1.19MB avant, ~5.19MB maintenant.
  C'est le but de cette carte : plus de marge pour les logs GPS détaillés
  et le carnet de session cumulatif, sans purge aussi fréquente.

## Interaction (écran statut + menu principal)

**CONFIRM et BACK sont lus par interruption matérielle** (comme
l'encodeur), pas par sondage du GPIO à chaque tour de boucle -- le
sondage peut manquer un appui court si la boucle principale est
momentanément ralentie (typiquement `webServerManager.loop()` pendant
l'usage WiFi).

**Écran statut** (par défaut) : indicateur REC discret (petit point fixe,
pas de clignotement), dernier tour, meilleur tour. Avant le premier tour
bouclé, la zone centrale affiche l'état -- distingue maintenant "GPS ?"
(pas de fix), "Detection..." (recherche en cours), "Nv. circuit" (mode
proximité actif, circuit inconnu) **et le nom du circuit** dès qu'un
circuit connu est sélectionné (geofencing, menu, ou détection normale
aboutie) mais pas encore de tour complet -- corrige un flou constaté sur
le terrain où tout tombait dans un "En attente..." générique, sans
aucune indication de quel circuit avait été reconnu (le point REC
confirmait que l'enregistrement tournait, mais pas sur quoi).

- **PUSH** (clic de l'axe encodeur) : démarre/arrête l'enregistrement.
- **CONFIRM** : force le mode proximité immédiatement (cf. section
  "Contournement" plus bas) -- bouton dédié, plus un appui long à
  chronométrer. Sans effet si un circuit est déjà résolu (connu ou
  proximité déjà active) -- juste une trace Serial dans ce cas.
- **BACK** : ouvre le menu principal.

**CONFIRM et PUSH restent redondants partout ailleurs** (menus) --
seul l'écran statut leur donne un rôle distinct, pour ne pas gaspiller
un bouton physique dédié sur une fonction déjà couverte par le clic de
l'encodeur.

### Enregistrement automatique (comme RaceChrono)

L'enregistrement démarre **tout seul** dès qu'un VRAI circuit est actif
(auto-détecté par `CourseManager`, ou forcé depuis le sous-menu Circuit)
-- le repli "Lap Anything" en proximité (circuit inconnu) ne compte pas
comme une détection, pas de démarrage auto dans ce cas. L'arrêt, lui,
reste **toujours manuel** (PUSH) : pas de coupure surprise pendant
que le pilote roule encore.

Un arrêt manuel pendant que le circuit reste actif (le pilote rentre aux
stands sans changer de circuit) **n'est pas réactivé tout seul** au tour
de boucle suivant -- l'auto-démarrage ne se redéclenche qu'après un
véritable changement de circuit (nouvelle sélection dans le sous-menu
Circuit, ou retour en "Auto (détection)").

### Contournement maison du fallback "Lap Anything" (bug constaté sur le terrain)

Constaté lors d'un test réel (portion de rocade avec 2 ronds-points,
5,45km, 5 passages confirmés à moins de 10m du point de départ sur
5min26 -- log GPS à l'appui, fix 3D constant 12-18 satellites tout du
long) : le mécanisme intégré de `CourseDetector` (lib `DovesLapTimer`)
peut rester bloqué en détection **indéfiniment**, même après plusieurs
vrais bouclages, sans jamais atteindre les 3 rejets nécessaires pour
basculer tout seul en mode proximité. Raison exacte pas identifiée avec
certitude (pas de logs Serial en direct sur ce test, usage autonome) --
piste la plus probable : le point de référence interne ("waypoint") est
posé dès le premier dépassement de 32km/h de la sortie, pas forcément au
début de l'enregistrement, ce qui peut fausser l'analyse après coup.

Plutôt que de patcher la lib (fork), `main.cpp` la contourne directement
: le chrono proximité (`WaypointLapTimer`, exposé via
`getLapAnythingTimer()`) tourne de toute façon en permanence en tâche de
fond, indépendamment du flag interne de la lib. `main.cpp` bascule
lui-même l'affichage/l'enregistrement sur ce chrono (`forcedLapAnything`)
dès que l'une de ces deux conditions est vraie, sans dépendre du
compteur de rejets buggé de la lib :

- sa distance parcourue dépasse **4500m** (`DETECTION_FALLBACK_DISTANCE_M`,
  nettement au-dessus du plus long circuit connu -- Le Mans Bugatti,
  4283m) -- le filet de sécurité d'origine ;
- **ou il a déjà bouclé un tour tout seul** (`getLaps() > 0`) --
  ajouté après un test terrain sur une petite boucle de quartier
  (~1,8km/tour) où la session s'est terminée avant d'atteindre 4500m
  parcourus, alors qu'un tour avait bien été compté en interne (jamais
  affiché/loggé, `lapAnythingEffective()` étant resté faux). Un vrai tour
  complété est une preuve directe qu'on est en mode proximité -- pas
  besoin d'attendre une distance arbitraire en plus dans ce cas.

Réinitialisé à chaque retour en "Auto (détection)" (`activateAutoMode()`).

**Bouton CONFIRM dédié** (écran statut, appui court) : force le mode
proximité immédiatement, sans attendre la distance de secours -- pour
le pilote qui sait déjà que la détection ne peut pas aboutir (test sur
une route sans rapport avec un circuit calibré, par exemple) et ne veut
pas patienter. Auparavant un appui long (≥800ms) sur CONFIRM/PUSH
redondants -- remplacé par un bouton dédié début juillet 2026 : plus
robuste avec des gants (pas de durée à chronométrer), et surtout plus
découvrable (un vrai bouton visible plutôt qu'un geste caché). PUSH
garde seul le démarrage/arrêt d'enregistrement sur cet écran -- CONFIRM
et PUSH restent redondants ailleurs (menus), où un seul choix ("valider")
est possible. Sans effet si un circuit est déjà forcé manuellement ou
si la détection a déjà abouti (évite d'écraser une vraie détection par
erreur) -- juste une trace Serial dans ce cas. **C'est aussi le seul
déclencheur qui arme la capture automatique de nouveau circuit** (cf.
section dédiée plus bas) -- un repli automatique en proximité (3 rejets
natifs ou le contournement ci-dessus) ne déclenche jamais d'écriture
dans `circuits.csv`, volontairement.

**BACK annule un CONFIRM presse par erreur** (`cancelForcedLapAnything()`)
: si le mode proximité a été forcé et rien n'a encore été écrit dans
`circuits.csv` (capture encore en attente), BACK sur l'écran statut
remet tout à zéro (retour à la détection normale) -- en plus d'ouvrir le
menu comme d'habitude, comportement inchangé sur ce point. Si une
capture a déjà été écrite (un tour a eu le temps de se valider), BACK ne
la supprime pas automatiquement -- juste une trace Serial invitant à
aller la supprimer sur `/circuits` si besoin, pour éviter de perdre une
vraie capture par un BACK réflexe.

**Menu principal** (BACK depuis l'écran statut) : tourne pour naviguer
entre 4 entrées, CONFIRM/clic pour valider, BACK pour revenir à l'écran
statut.

- **Circuit** -- sous-menu "Auto (détection)" + les circuits actifs (cf.
  section dédiée "Circuits persistants" plus bas -- jusqu'à 8, chargés
  depuis `circuits.csv`) + "WiFi téléchargement" (en dernier). Choisir un
  circuit (ou Auto) revient directement à l'écran statut ; choisir WiFi
  bascule sur l'écran dédié (cf. plus bas). BACK depuis ce sous-menu
  revient au menu principal.
- **Connexion** -- GPS (fix/satellites) + nom du circuit actif (l'ancien
  "écran info"). BACK revient au menu principal.
- **Session** -- liste des sessions enregistrées (les plus récentes en
  premier), regroupées d'après les marqueurs `# session demarree/arretee`
  du carnet `/sessions.csv`. CONFIRM sur une session ouvre le détail tour
  par tour (numéro, temps, meilleur temps courant, circuit), navigable à
  l'encodeur, dernier tour affiché en premier. BACK remonte d'un niveau à
  chaque fois (détail → liste → menu principal).
- **Réglages** -- une seule entrée pour l'instant : "WiFi téléchargement"
  (raccourci vers la même action que dans le sous-menu Circuit).

Choisir "WiFi téléchargement" dans le sous-menu Circuit active le point
d'accès et bascule sur un écran dédié affichant le SSID et l'adresse IP
-- pratique une fois le boîtier monté sur la moto, sans câble Serial à
portée de main. Le GPS continue de tourner normalement pendant tout
l'usage WiFi (câblé, pas de radio à ménager -- cf. section migration plus
bas). BACK depuis cet écran coupe le WiFi et revient à l'écran statut.

### Le mode manuel, en détail

`CourseManager` ne propose pas de méthode pour forcer directement un
circuit (vérifié dans son API) -- seule la détection automatique existe.
Choisir un circuit dans le sous-menu Circuit active donc un second
chronomètre (`DovesLapTimer`) indépendant, configuré avec la ligne de ce
circuit précis. Quand actif, ce timer manuel remplace entièrement le
`CourseManager` dans l'affichage et les logs -- les deux sont mutuellement
exclusifs (jamais les deux en même temps).

Choisir "Auto (détection)" désactive le mode manuel et relance la
détection automatique depuis zéro.

### Geofencing (reconnaissance quasi instantanée sur circuit connu)

L'algorithme natif de la lib (`CourseDetector`) ne connaît pas la
position réelle des circuits, seulement leur longueur de tour -- il
faut donc boucler au moins un tour avant de confirmer un circuit (cf.
"Contournement maison" ci-dessus). Inutile dans ce cas précis : les
circuits utilisés sont espacés de dizaines de km, donc une simple
distance à vol d'oiseau au premier fix GPS suffit à lever toute
ambiguïté, avant même d'avoir bougé -- comme RaceChrono.

**Vérifié une seule fois**, au premier fix GPS valide (ou après un
retour explicite en "Auto (détection)", qui remet `geofenceCheckDone` à
`false`) : distance à vol d'oiseau entre la position courante et le
point milieu de la ligne départ/arrivée de chaque circuit **actif**. Un
seul en dessous de `GEOFENCE_MAX_DISTANCE_M` (15km, large marge
délibérée vu l'espacement réel) → activé directement, en réutilisant
`activateManualCourse()` (même mécanisme que le choix manuel au menu,
juste déclenché automatiquement). Aucun match → aucun effet, l'algorithme
de détection normal prend le relais sans rien savoir de cette
vérification -- zéro régression sur le comportement existant.

**Si tu ajoutes un nouveau circuit dans ce rayon de 15km d'un circuit
existant** (peu probable vu l'usage actuel, mais prévu) : désactive
temporairement l'un des deux depuis `/circuits` pour éviter toute
ambiguïté -- le geofencing prend simplement le plus proche parmi les
circuits actifs, pas de gestion de cas multi-candidats pour l'instant.

### Capture automatique de nouveau circuit

Déclenchée **uniquement** par le bouton CONFIRM (cf.
`newCircuitCaptureArmed`, armé dans `forceLapAnythingManually()`) --
jamais par un repli automatique en proximité (3 rejets natifs, ou le
contournement 4500m/tour ci-dessus). Sans ce garde, une balade improvisée
où rien n'est reconnu finirait par écrire un circuit parasite dans
`circuits.csv` sans que ce soit demandé -- le mode proximité/comptage de
tours continue de fonctionner pareil dans tous les cas, seule
l'**écriture** du fichier est réservée au geste intentionnel.

Capture en deux temps, volontairement découplés :

1. **Dès que le waypoint interne est posé** (`getWaypointLat()`/
   `getWaypointLng()`, transition de `(0,0)` à une position réelle -- on
   est encore physiquement dessus) : le cap de la route est estimé à
   partir d'un petit historique GPS embarqué (buffer circulaire, ~4s de
   recul, `GPS_HISTORY_SIZE`), puis une ligne A/B perpendiculaire est
   calculée (mêmes formules que `generate_line.py`, cf. plus bas,
   réimplémentées en C++ -- `geoBearingDeg()`/`geoDestinationPoint()`,
   aucune dépendance externe). Attendre le tour validé pour ça donnerait
   un cap totalement faux, calculé à un endroit potentiellement loin de
   la ligne réelle.
2. **Écriture effective seulement au premier tour validé**
   (`getRaceStarted()`) *et* seulement si toujours en mode proximité à
   cet instant (`lapAnythingEffective()`) -- si un vrai circuit connu a
   fini par être détecté entre-temps, la capture est abandonnée
   silencieusement plutôt que de créer une entrée parasite.

Écrit directement dans `circuits.csv` : `active=0`, non protégé, nom
générique `Nouveau_AAAAMMJJ_HHMMSS` (`getLocalDateTimeCompact()`),
longueur déduite du premier tour réel (`getLastLapDistance()`, converti
en pieds). Toujours **aucune saisie de texte depuis le chrono** -- la
finalisation (nom définitif, activation) se fait sur `/circuits`, comme
prévu depuis le début (cf. discussion ergonomie encodeur vs page web).

**Script compagnon, `generate_line.py`** (hors dépôt, sur le PC) : permet
de générer une ligne A/B à la main depuis n'importe quel `log_*.csv`
existant, en donnant juste une heure cible -- utile pour un circuit
enregistré *avant* l'ajout de cette fonctionnalité (comme `PigTeam_track`,
capturé après-coup depuis un vieux log), ou pour ajuster une ligne sans
retourner sur place. Même principe de calcul (cap lissé sur une fenêtre
de quelques secondes + décalage perpendiculaire) que la capture embarquée.

## Logs, GPS, WiFi

Log GPS par session (`/log_AAAAMMJJ_HHMMSS.csv`), carnet de session
cumulatif (`/sessions.csv`), commandes Serial `l`/`d`/`c`/`s`/`x`/`w`/`g`,
téléchargement WiFi à la demande (réseau ouvert). **Le GPS n'est plus
coupé pendant l'usage WiFi** -- étant câblé (UART), il n'y a plus de
radio à partager entre GPS et WiFi comme du temps du BLE, donc plus
besoin d'arbitrer quoi que ce soit autour du cycle WiFi. Pas de gestion
de sprites à libérer non plus -- le tampon OLED est minuscule (~1 Ko),
aucune pression mémoire à gérer contrairement à la version TFT.

### `WebServerManager` -- module séparé, 6 pages (+ 1 page cachée)

Le point d'accès WiFi et le serveur HTTP vivent dans
`WebServerManager.h`/`.cpp`, pas dans `main.cpp` -- même logique de
séparation que sur la Pendule Paddock. Le module ne connaît rien du GPS
ni du détail des logs : `main.cpp` lui fournit ses callbacks à
l'initialisation (`wifiCallback_flushLogs` pour le flush avant
téléchargement, `webServerCallback_getStatus` pour la page Statut), sans
qu'il ait besoin de connaître les objets internes du reste du firmware.
Les callbacks `bleStop`/`bleRestart` existent toujours dans la signature
de `begin()` (compatibilité) mais sont passés à `nullptr` -- plus
d'usage depuis le retrait du BLE.

**Pas de bouton "démarrer l'enregistrement à distance"** : absent depuis
l'origine du projet (à l'époque du BLE, ça n'aurait rien écrit puisque
le BLE était coupé pendant le WiFi). Ce n'est plus vrai avec le GPS
câblé -- ajouter ce callback deviendrait possible si le besoin se
présente, pas fait pour l'instant.

- **Sessions** (`/`) -- gestion des sessions GPS enregistrées, groupées
  par date (la plus récente en premier), chacune avec son résumé (nombre
  de tours, meilleur temps -- recoupé automatiquement avec le carnet de
  session cumulatif via l'horodatage commun aux deux fichiers), un lien
  de téléchargement, et un bouton de suppression individuelle (avec
  confirmation navigateur). Le carnet cumulatif (`/sessions.csv`) reste
  téléchargeable en haut de page, mais volontairement **non
  supprimable** depuis cette page (perte de tout l'historique sinon) --
  utilise `x` en Serial si tu veux vraiment le réinitialiser. Lien discret
  "Sauvegarde complète (.zip)" juste en dessous -- cf. section dédiée
  plus bas.
- **Comparer** (`/compare`) -- superpose deux sessions déjà enregistrées
  sur l'ESP (menus déroulants, pas besoin de fichier externe), tracé SVG
  calculé côté navigateur, sans fond de carte réel (pas de connexion
  Internet une fois connecté au point d'accès de l'ESP) -- la forme du
  tracé seule suffit à reconnaître un circuit. Utilisable au bord de la
  piste, sur le téléphone, entièrement hors-ligne. cf. section dédiée
  plus bas pour le détail technique.
- **Circuits** (`/circuits`) -- gestion complète des circuits (liste,
  ajout, édition, suppression, activation/désactivation) persistée dans
  `circuits.csv` sur LittleFS. Bouton "+ Nouveau circuit" mis en avant
  (bouton plein), lien d'export "Exporter circuits.csv" volontairement
  discret juste en dessous (pensé pour migrer vers une nouvelle carte,
  pas l'action principale de la page -- les deux étaient visuellement
  confondus avant, corrigé). cf. section dédiée plus bas.
- **Statut** (`/status`) -- état GPS (fix/satellites), circuit actif,
  état de l'enregistrement (tours/dernier/meilleur), mémoire libre (heap)
  et uptime. Lien "Rafraîchir" pour une nouvelle lecture (pas de
  rafraîchissement auto, pour rester simple).
- **Import** (`/import`) -- quatre formulaires indépendants pour
  récupérer des sessions depuis un autre appareil ou une sauvegarde :
  log GPS détaillé seul, fusion du carnet (`sessions.csv`), conversion
  d'un export RaceChrono, et restauration d'une sauvegarde complète
  (`.zip`, cf. section dédiée). cf. section dédiée plus bas pour le
  détail de chacun.
- **Firmware** (`/update`) -- mise à jour OTA du firmware (upload de
  `firmware.bin`). cf. section dédiée plus bas.
- **Debug** (`/debug`, page cachée -- pas de lien dans le menu du haut,
  à taper directement dans le navigateur) -- vidage complet ou sélectif
  du carnet de session, réservé à la phase de test/réglage.

### `/compare` -- comparaison de tracés hors-ligne

Pas de fond de carte réel (OpenStreetMap etc.) : ça demanderait une
connexion Internet que le téléphone n'a plus une fois connecté au point
d'accès ChronoMoto. La trajectoire seule suffit largement à reconnaître
un circuit, donc tout reste utilisable sans réseau, au bord de la piste.

Fonctionnement : la page sert un formulaire de sélection (deux menus
déroulants parmi les sessions déjà sur l'ESP), puis une fois validé, sert
une page contenant un peu de JS vanilla (aucune dépendance externe/CDN)
qui va chercher les deux CSV via la route `/download` déjà existante,
les parse, projette les coordonnées lat/lon sur un plan local en mètres
(projection équirectangulaire centrée sur le premier point -- l'erreur de
courbure terrestre est négligeable à l'échelle d'un circuit), et dessine
les deux tracés superposés en SVG (orange / cyan), avec les stats de
chaque session (points, distance, vitesse max, durée) côté navigateur.
Tout le calcul se fait côté client -- l'ESP se contente de servir les
deux fichiers bruts, aucune charge CPU/mémoire supplémentaire pour lui.

## `/circuits` -- circuits persistants (LittleFS, éditables sans reflasher)

Les circuits ne sont plus codés en dur dans `main.cpp` -- ils vivent
dans `/circuits.csv` (LittleFS), éditable depuis cette page web. Motivation :
il arrive que des tracés changent (chicane ajoutée, piste modifiée), et
ça évite de recompiler/reflasher à chaque fois. Ça permet aussi
d'ajouter un circuit inconnu sur le terrain (ex: un tour de quartier
pour tester) sans toucher au firmware.

**Actif / inactif** : tous les circuits stockés ne sont pas forcément
chargés dans `CourseManager` en même temps -- seuls ceux cochés **actif**
le sont, au démarrage. Ça colle à l'usage réel (on ne roule jamais sur
deux circuits le même jour) et ça contourne une limite fixe de la lib
`DovesLapTimer` : `MAX_COURSES = 8`, codé en dur côté lib (pas
modifiable sans forker). Le reste des circuits (inactifs) dort dans le
fichier, réactivable à tout moment sans perte -- rien n'est jamais
supprimé automatiquement.

**Limite de 8 actifs, bloquée côté web** : si tu coches "Actif" sur un
9e circuit, il est quand même **enregistré** (coordonnées comprises,
rien n'est perdu) mais reste forcé en inactif, avec un message clair --
décoche-en un autre puis reviens l'activer depuis la liste. Volontaire :
un rejet pur et simple du formulaire ferait perdre toute la saisie en
cours (particulièrement pénible pour un nouveau circuit tout juste
capturé au GPS).

**Bouton "Capturer position GPS actuelle"** sur chaque point (ligne
départ/arrivée + secteurs 2/3 optionnels) : remplit les champs lat/lng
avec la position live, via `fetch('/gps-position')` en JS -- pratique
pour saisir un circuit en se plaçant physiquement sur la ligne plutôt
que de chercher les coordonnées sur une carte. Fonctionne parce que le
GPS continue de tourner pendant le WiFi (câblé, cf. section migration
plus bas) -- ça n'aurait pas été possible avec le RaceBox/BLE.

**Longueur affichée/saisie en mètres**, stockage interne en pieds
(`length_ft` dans le CSV -- ce qu'attend `CourseConfig` côté lib).
Conversion faite uniquement aux deux points d'entrée/sortie du
formulaire (`FT_PER_METER = 3.28083989501`, côté `WebServerManager.cpp`)
-- `main.cpp` ne voit jamais la conversion, aucun impact sur
`CourseManager`.

**Case "Protégé contre la suppression accidentelle"** (`locked`, 18e
colonne CSV) : les 8 circuits d'origine sont protégés par défaut (seedés
ainsi au premier démarrage -- ce sont les références calibrées du
projet). Bouton Supprimer grisé côté page **et** bloqué côté serveur
(`handleCircuitDelete()` -- pas juste le bouton HTML, une requête
construite à la main ne contourne pas la protection). Décoche "Protégé"
depuis Modifier pour supprimer un circuit protégé. Colonne
**rétrocompatible** : un `circuits.csv` écrit avant son ajout (17
colonnes) reste lisible, `locked` vaut simplement `false` dans ce cas.

**Sauvegarde → redémarrage automatique** de l'ESP dès qu'un changement
affecte la détection (circuit actif ajouté/modifié/désactivé) -- plus
robuste qu'un rechargement à chaud de `CourseManager` pendant qu'une
session pourrait être en cours, même logique que la fin d'un flash OTA.
Pas de redémarrage si le changement ne touche qu'un circuit resté
inactif (aucun impact sur la détection en cours).

**Format CSV** (`active,name,length_ft,sa_lat,sa_lng,sb_lat,sb_lng,has2,
s2a_lat,s2a_lng,s2b_lat,s2b_lng,has3,s3a_lat,s3a_lng,s3b_lat,s3b_lng,
locked`) -- parsé indépendamment côté `main.cpp` (lecture seule, juste
les circuits actifs, s'arrête aux 17 premiers champs -- `locked` ne le
concerne pas) et côté `WebServerManager.cpp` (lecture/écriture complète,
tolérante à 17 ou 18 colonnes) -- même principe de découplage que pour
`/sessions.csv`, chaque module lit le fichier à sa façon pour son propre
besoin, sans dépendance croisée.

**Premier démarrage après cette fonctionnalité** : si `circuits.csv`
n'existe pas encore, il est recréé automatiquement avec les 8 circuits
d'origine (tous actifs) -- migration transparente, rien à ressaisir.

**Export "Exporter circuits.csv"** (lien discret en haut de page) :
télécharge le fichier CSV brut tel quel, pensé pour migrer vers une
nouvelle carte -- télécharge, place-le dans `data/circuits.csv` du
nouveau firmware, `pio run -t uploadfs`. N'est PAS inclus dans la
sauvegarde complète (`/backup`, cf. section dédiée plus bas) --
volontairement, la définition des circuits n'est pas une "session".

## Migration RaceBox (BLE) → NEO-M8N (UART direct)

Les toutes premières versions utilisaient un RaceBox Micro connecté en
BLE. Motivation du changement : le BLE apportait son lot de complexité
récurrente (scan par fenêtres bornées pour rester réactif, cohabitation
radio avec le WiFi, plantages `BLEDevice::deinit()` documentés sur le
dépôt NimBLE-Arduino -- cf. historique Git pour le détail de ces
contournements) pour un gain limité : le RaceBox est lui-même une puce
u-blox en interne, autant piloter le module directement.

**Module** : GY-GPS6MU2 (puce u-blox NEO-M8N), antenne active déportée,
pile de sauvegarde RTC embarquée (hot-start quasi instantané une fois la
position/almanach en cache -- confirmé par test : fix quasi immédiat
après une coupure/reprise rapide de l'alimentation).

**Protocole** : UBX binaire (pas de NMEA -- coupé explicitement à la
config), trame NAV-PVT à 10Hz. Le module sort en 9600 bauds par défaut,
insuffisant pour tenir 10Hz (une trame NAV-PVT fait ~106 octets, soit
~1060 octets/s nécessaires contre ~960 octets/s de capacité en 9600) --
`initGps()` bascule donc le port du module en 38400 bauds dès le
démarrage via `UBX-CFG-PRT`, avant de configurer le taux (`UBX-CFG-RATE`,
100ms) et d'activer NAV-PVT en sortie (`UBX-CFG-MSG`). Pas de
`UBX-CFG-CFG` (sauvegarde en flash) : la plupart des breakouts NEO-M8N
n'ont pas de flash externe, la pile ne persiste que la RAM de sauvegarde
(position/almanach), pas la config UBX -- elle est donc renvoyée à
chaque démarrage (quelques ms, sans conséquence).

**Compatibilité de champs** : `GpsData` (ex-`RaceBoxData`) garde
exactement les mêmes champs -- `fixType` suit déjà la convention u-blox
(0=aucun, 1=dead-reckoning, 2=2D, 3=3D...) aussi bien côté RaceBox que
côté NEO-M8N brut (même famille de puce), donc tout le code en aval
(`getLocalDateTime`, `logRow`, `CourseManager`...) n'a nécessité aucune
modification.

**Validation** : comparaison RaceBox/M8N sur un même trajet routier
(vitesse cohérente à quelques % près une fois recalculée indépendamment
depuis les positions GPS, distance/durée globales identiques), plus un
test en intérieur qui a bien montré les limites attendues (multipath,
bruit de position/vitesse) par rapport à un test extérieur propre --
rien de spécifique au M8N, comportement GPS générique.

**GPIO** : le GPS UART réutilise deux des broches libérées par le retrait
de la carte SD (RX=4, TX=5) -- cf. tableau de câblage plus haut.

## Migration NEO-M8N → NEO-M9N → retour au NEO-M8N (débit initial différent)

Le S3-Tiny tourne avec un NEO-M9N plutôt que le NEO-M8N du S3-Zero --
même famille de puce u-blox, même protocole UBX, même trame NAV-PVT à
10Hz, mais **un détail d'initialisation diffère selon le breakout**.

**Le NEO-M8N sort en 9600 bauds par défaut** -- insuffisant pour tenir
10Hz (~1060 octets/s nécessaires contre ~960 octets/s de capacité en
9600), d'où la bascule vers 38400 dès le démarrage via `UBX-CFG-PRT`
(cf. section "Migration RaceBox → NEO-M8N" plus haut pour le détail).

**Le breakout NEO-M9N utilisé ici sort déjà en 38400 bauds nativement**,
configuré ainsi en usine par le revendeur (constaté au banc, pas une
caractéristique générale de tous les NEO-M9N -- un autre breakout du
même modèle pourrait très bien démarrer en 9600 comme le M8N). D'où
`GPS_INITIAL_BAUD` fixé directement à 38400 dans `main.cpp`, au lieu de
9600 puis bascule. La séquence de config (`configurePortBaud` /
`updateBaudRate` / `configureNavRate10Hz` / `enableNavPvt`) reste
identique et sans risque à envoyer même si le module y est déjà --
simple redondance, quelques ms perdues au démarrage, rien de plus.

**Si un jour ce montage revient sur un module qui démarre en 9600**
(NEO-M8N, ou un autre breakout NEO-M9N configuré différemment en
usine) : remettre `GPS_INITIAL_BAUD` à 9600 dans `main.cpp`, sinon la
toute première commande de bascule de débit n'est jamais reçue par le
module (elle part au débit cible, que le module n'écoute pas encore).

**Retour effectif au NEO-M8N** : le NEO-M9N testé ci-dessus a fini par
poser un souci de négociation de débit sur ce montage précis (le
breakout ne s'est pas comporté de façon aussi prévisible en usage
prolongé que lors du test initial au banc) -- combiné au fait que les
NEO-M6N/M7N testés en parallèle ne supportent de toute façon pas
NAV-PVT (cf. paragraphe suivant), le NEO-M8N a été retenu comme module
de référence sur cette variante : `GPS_INITIAL_BAUD` est revenu à 9600
dans `main.cpp`/`GpsManager.cpp`, avec la bascule vers 38400 décrite
dans la section "Migration RaceBox → NEO-M8N" ci-dessus. Le NEO-M9N
reste conservé comme module de réserve/comparaison, pas monté en
usage courant sur cette variante.

**Modules testés et écartés en cours de route** : NEO-M6N et NEO-M7N
branchés sur ce montage causent bien en NMEA (câblage/alim/débit
corrects, confirmé au sniffer UART brut), mais **ne supportent pas
NAV-PVT** -- message introduit à partir de la génération M8 (protocole
UBX version 15+), absent des générations antérieures. La commande
`enableNavPvt()` est donc silencieusement ignorée (ou NAK) par ces
modules, aucune trame exploitable en aval. Pas la peine d'insister avec
ces générations sur ce firmware tel qu'il est écrit -- il faudrait
réécrire le parsing pour NAV-POSLLH + NAV-SOL, pas rentable pour de
simples modules de test.

**Méthode de diagnostic en cas de nouveau module GPS douteux** : un
sniffer UART brut temporaire dans `setup()` (écoute passive avant tout
envoi, avec un `delay(1)` dans la boucle pour ne pas déclencher le
watchdog) permet de trancher vite entre plusieurs causes possibles --
silence total (câblage/alim/court-circuit RX-TX), charabia illisible
(mauvais débit d'écoute), ou texte NMEA lisible (module vivant, débit
correct, le souci est alors plus haut dans la pile UBX/NAV-PVT). Pensez
à retirer ce bloc une fois le diagnostic posé -- ne doit pas rester dans
le firmware "propre".

## Sauvegarde complète et restauration (`/backup`, `/import/restore`)

**Bouton "Sauvegarde complète (.zip)"** sur la page Sessions (`/`, à côté
du téléchargement du carnet seul) : génère à la volée une archive `.zip`
regroupant le carnet (`sessions.csv`) et **tous** les logs GPS détaillés
(`log_*.csv`), streamée directement au navigateur -- jamais assemblée en
RAM ni stockée temporairement sur la flash. Format "stored" (pas de
compression -- les CSV compressent mal de toute façon, et ça évite
d'embarquer une lib de compression). N'inclut PAS `circuits.csv`
(export dédié séparé, cf. section `/circuits` plus haut) -- la
définition des circuits n'est pas une "session".

CRC32 et taille de chaque fichier sont **calculés avant** d'écrire son
en-tête ZIP local (une double lecture du fichier source, lecture flash
bon marché) plutôt que via la technique classique du "data descriptor"
(crc/taille écrits après les données) : ce choix n'est pas anodin, cf.
paragraphe suivant.

**Formulaire "Restaurer cette sauvegarde"** sur la page Import (`/import`,
4e formulaire) : réinjecte une archive `.zip` générée par `/backup` --
carnet fusionné (mêmes garanties anti-doublon que la fusion carnet
ci-dessous, réutilise directement `importCarnetProcessLine()`) et logs
GPS restaurés (ignorés s'ils existent déjà localement, jamais écrasés).
Extraction **en un seul passage, octet par octet, directement depuis le
flux HTTP entrant** -- même principe qu'une machine à états déjà utilisée
ailleurs dans ce firmware pour un flux binaire reçu en morceaux de
taille arbitraire (cf. `pollGps()` dans `GpsManager.cpp`, qui fait
exactement ça pour les trames UBX). Ceci n'est possible QUE parce que
l'en-tête local ZIP écrit par `/backup` est complet et fiable dès le
départ (crc/taille connus avant le premier octet de données) -- pas
besoin de repértoire central ni de sauter à la fin de l'archive.

**Pourquoi pas de fichier temporaire** : une première version
écrivait l'archive reçue dans un fichier temporaire sur LittleFS avant
de l'analyser (nécessaire avec la technique du "data descriptor", qui
oblige à atteindre la FIN de l'archive avant de pouvoir extraire quoi
que ce soit). Sur une LittleFS déjà bien remplie, ce besoin de stocker
une copie complète de l'archive **en plus** des fichiers en cours de
restauration faisait typiquement échouer l'opération
(`esp_littlefs: No more free space`) pile au moment où on en a le plus
besoin -- espace nécessaire ~2x la taille de la sauvegarde. La version
actuelle ne consomme que la place des fichiers réellement restaurés,
exactement comme `/import/logs` ou `/import/carnet`.

**Format non rétrocompatible** : une sauvegarde générée par une version
antérieure de ce firmware (avec "data descriptor") n'est plus lisible
par `/import/restore` -- message d'erreur explicite dans ce cas plutôt
qu'une corruption silencieuse. Regénère une nouvelle sauvegarde avec le
firmware à jour si besoin.

## Mode Débogage (rejeu d'un vrai log dans le VRAI pipeline de production)

Ce mode rejoue un fichier **au même format que les vrais
`log_AAAAMMJJ_HHMMSS.csv`** à travers le pipeline de production réel --
`processGpsFix()`, donc geofencing, détection/contournement,
démarrage auto de l'enregistrement, logs compris. Objectif : valider la
logique de détection sur un trajet réellement enregistré, sans avoir à
rouler à chaque modification de code.

**Extraction de `processGpsFix()`** : tout le traitement d'un point GPS
(anciennement dans `loop()` directement) vit maintenant dans une
fonction dédiée, paramétrée par le temps -- appelée aussi bien par le
vrai GPS (`timeMs = millis()` réel) que par le rejeu (`timeMs` =
horodatage simulé du CSV). Garantit que le mode Débogage teste
exactement le même chemin de code que l'usage réel, pas une
approximation à part.

**Activation** : commande Serial **`g`**. Rejoue `/debug_replay.csv` (LittleFS) -- **même format
qu'un `log_*.csv` normal**, renomme simplement le fichier récupéré avant
de l'envoyer via `pio run -t uploadfs`. Rejeu accéléré x10, reboucle
automatiquement en fin de fichier. Repart d'un état totalement vierge à
chaque activation (`courseManager->reset()`, `forcedLapAnything`,
`geofenceCheckDone`, etc. remis à zéro) -- sinon on testerait avec un
état hérité d'un essai précédent sur ce même firmware.

**⚠️ À n'utiliser QUE sur un ESP sans GPS réel branché** (ESP de
rechange, pas de module UART connecté) : `toggleDebugReplay()` refuse de
s'activer si `gpsActive` est vrai (un GPS a répondu). Sur le même
appareil, GPS réel et rejeu écriraient tous les deux dans `liveData`/
`courseManager` en parallèle -- résultat incohérent garanti. Pensé
justement pour tourner sur un stock d'ESP32-S3-Zero de rechange, sans
OLED/GPS si besoin (juste le moniteur Serial suffit pour observer les
logs de détection).

## Stockage des logs (SD + LittleFS)

La carte SD a été **réintégrée** sur ce montage (elle avait été retirée
lors du passage au GPS UART, ses GPIO d'origine servant alors à autre
chose) -- cf. `README_FS.md` (variante TFT) pour l'historique complet de
cette réintégration et les pièges rencontrés (conflit de nom PlatformIO
sur `SD`, choix `<SD.h>` plutôt que `SdFat`...), largement réutilisé tel
quel ici.

**Différence avec le TFT** : cet OLED est en I2C, aucun bus SPI existant
à partager -- la SD utilise donc un bus SPI **entièrement dédié**
(CS=7, MOSI=8, MISO=9, SCLK=10), contrairement au TFT qui partage
MOSI/SCLK avec son écran ST7789 et ne dédie que CS+MISO à la carte.

**Câblage validé au banc** via un sketch isolé (`sd-wiring-test-oled/`,
`<SD.h>` + `SPIClass` dédiée sur les mêmes pins) avant toute intégration
dans le firmware de prod -- écriture/lecture confirmées, carte SDHC
détectée correctement. Piège rencontré lors du premier essai : MOSI/MISO
inversés au câblage (symptôme `sdCommand(): Card Failed!` / `f_mount
failed`) -- résolu en re-vérifiant la continuité fil par fil.

**Module `SdLogStorage.h`/`.cpp`** : la logique SD (bascule `gpsLogFs`,
repli LittleFS, migration, stats pour la barre web) a été extraite dans
un module dédié, dans le même esprit que `GpsManager`/`WebServerManager`
-- pensé pour être mutualisable tel quel avec la variante TFT (pas encore
fait côté TFT à ce jour, celui-ci garde encore son code SD en ligne dans
`main.cpp`). Le module ne possède jamais son propre bus SPI et ne fait
jamais `spi.begin()` lui-même -- cette responsabilité reste dans chaque
`main.cpp`, car elle diffère structurellement entre variantes (bus dédié
ici, partagé côté TFT) :

```cpp
// main.cpp
sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
if (initSdLogStorage(SD_CS, sdSPI)) migrateLittleFsLogsToSd();
```

**Seuls les logs GPS détaillés (`log_*.csv`) suivent la SD** -- le
carnet cumulatif (`/sessions.csv`) et `circuits.csv` restent TOUJOURS
sur LittleFS, quel que soit l'état de la carte (repli automatique si
absente/en panne -- la SD reste facultative).

**Migration des sessions pré-existantes** (`migrateLittleFsLogsToSd()`) :
au premier démarrage avec la SD active, les éventuels `log_*.csv` restés
sur LittleFS (sessions enregistrées avant la réintégration SD) sont
copiés vers la carte -- copie vérifiée par taille avant suppression de
la source, idempotent, sans risque en cas de coupure en plein milieu.

**Testé en conditions réelles** (sortie sur `PigTeam_track`) : écriture
confirmée pendant `REC` (`Nouveau log de session : ... (SD)`), fichiers
exploitables aussi bien après un arrêt propre (PUSH) qu'après une
coupure d'alimentation brutale (test volontaire, en prévision du montage
définitif direct-batterie) -- aucune ligne tronquée ni corrompue dans
les deux cas. Page **Sessions** (`/`) : 2e barre de stockage SD affichée
en plus de celle de LittleFS, comme sur le TFT.

**Espace bien plus confortable qu'avec LittleFS seule** : à 10Hz, chaque
ligne de log fait environ 70-90 octets (~45 Ko/minute d'enregistrement
actif). Avec une carte de plusieurs Go, purger régulièrement n'est plus
une nécessité comme du temps du "LittleFS uniquement" -- le bouton
"Sauvegarde complète (.zip)" sur la page Sessions reste pratique pour
archiver/nettoyer périodiquement, mais sans urgence d'espace.

Une page **Firmware** (`/update`) a ete ajoutee au serveur web existant --
meme AP, pas de mode WiFi dedie en plus (le GPS continue de tourner
normalement pendant l'usage, cf. section Logs/GPS/WiFi plus haut).

**Utilisation** : depuis le sous-menu Circuit, choisir "WiFi
telechargement" comme d'habitude, se connecter au reseau, puis aller sur
`http://<ip affichee>/update` (onglet **Firmware** dans le menu du haut).
Choisir le fichier `firmware.bin` genere par PlatformIO
(`.pio/build/<env>/firmware.bin` apres `pio run`), l'envoyer. Le boitier
flashe puis redemarre tout seul en quelques secondes.

Implementation : `Update.h` (bibliotheque core ESP32) ecrit directement
dans la partition OTA inactive au fil de la reception HTTP (upload
multipart classique via `WebServer::upload()`), puis bascule dessus et
redemarre si le flash est complet et valide -- pas de fichier temporaire
sur la LittleFS, pas de RAM supplementaire requise.

### ⚠️ Prerequis : table de partitions custom

Le schema de partitions par defaut de PlatformIO n'a qu'un seul
emplacement d'application -- pas de place pour l'OTA (qui a besoin de
deux emplacements, `ota_0`/`ota_1`, pour flasher le nouveau firmware
sans ecraser celui qui tourne). Fichier fourni : `partitions_ota_8mb.csv`
(a la racine du projet, deja reference par `board_build.partitions` dans
`platformio.ini` -- cf. table complete et repartition detaillee en debut
de README, section "Particularités de l'ESP32-S3-Tiny").

**Si tu changes un jour ce schema de partitions**, un flash complet par
USB est necessaire une fois (`pio run -t upload`, ou `pio run -t erase`
suivi d'un upload si l'ancien schema pose probleme) -- l'OTA ne peut
prendre le relais qu'une fois le nouveau schema en place sur la carte.
Ca reinitialise aussi la LittleFS (meme consequence que `pio run -t
uploadfs`) : recupere ce qui compte avant si besoin -- le bouton
"Sauvegarde complète (.zip)" sur la page Sessions (cf. section dédiée
plus haut) est le moyen le plus simple de le faire.

**Verification apres compilation** : si `.pio/build/<env>/firmware.bin`
depasse ~1,31Mo (`0x150000`), ca ne rentrera plus dans les
`0x160000` reserves par emplacement (`app0`/`app1`) -- augmenter leur
taille (et reduire `spiffs` d'autant) dans ce cas.


À l'occasion : un petit formulaire sur la page WiFi pour régler des
paramètres à distance (identifiant moto/numéro de course pour
distinguer plusieurs CBR125 dans les noms de fichiers et le SSID,
forcer un circuit sans passer par le menu encodeur, etc.) -- pas
prioritaire pour l'instant, noté pour plus tard.

## À tester

1. ~~Driver OLED (`SH1106` par défaut, `SH1107` en commentaire si l'image
   sort déformée)~~ -- confirmé OK.
2. ~~GPS NEO-M8N + détection automatique sur un vrai circuit~~ -- validé
   via le rejeu de Croix-en-Ternois (ancien mode exemple, depuis retiré
   du firmware) ; reste à confirmer en conditions réelles de piste.
3. Le mode manuel (forcer un circuit via le menu, vérifier qu'un tour se
   détecte bien sur la ligne forcée).
4. ~~Le bouton BOOT à tenir pour flasher~~ -- confirmé non nécessaire sur ce setup.
5. Montage définitif de l'antenne GPS sur la moto (dégagement du ciel,
   tenue aux vibrations) -- testé jusqu'ici uniquement en montage
   provisoire (voiture, table).
6. Validation grandeur nature à Croix-en-Ternois (mi-juillet 2026) --
   l'échéance de référence pour tout le projet.
7. ~~Circuits persistants (`/circuits`) -- créer un "circuit de quartier"
   en conditions réelles~~ -- validé (`PigTeam_track`, ligne calculée
   après-coup via `generate_line.py` sur un log capturé avant l'ajout de
   la capture automatique embarquée).
8. Mode Débogage (`g`) -- sur l'ESP de rechange (`oled-m8n-circuits-debug/`),
   rejouer un vrai log de boucle test (aller-retour paddock → boucle →
   paddock, REC démarré dès le départ) et vérifier en Serial que le
   geofencing/la détection/le contournement se comportent comme attendu.
9. ~~Geofencing -- vérifier en conditions réelles qu'un circuit actif à
   proximité s'active bien dès le premier fix GPS~~ -- validé sur
   `PigTeam_track` (activation à 1,4km, enregistrement auto démarré).
10. Capture automatique de nouveau circuit (bouton CONFIRM) -- tester le
    flux complet en conditions réelles sur un nouveau spot inconnu
    (armement, capture du cap au bon moment, écriture dans
    `circuits.csv`, vérifier la ligne A/B générée une fois sur
    `/circuits`).
11. Affichage du nom de circuit avant le premier tour (écran statut) --
    vérifier que les noms plus longs que 18 caractères restent lisibles
    (tronqués proprement, pas de débordement à l'écran).
12. Séparation CONFIRM/PUSH sur l'écran statut (PUSH = REC, CONFIRM =
    Nouveau circuit) -- valider avec des gants que les deux boutons
    restent bien distincts au toucher, pas de confusion en conditions
    réelles.
13. BACK-annulation (`cancelForcedLapAnything()`) -- presser CONFIRM par
    erreur puis BACK juste après, vérifier en Serial que tout est bien
    remis à zéro et que la détection normale reprend.
14. Sauvegarde complète (`/backup`) et restauration (`/import/restore`)
    -- valider sur le terrain avec une vraie archive multi-sessions :
    taille/temps de génération réels, fusion anti-doublon du carnet en
    cas de réimport, détection CRC en cas de transfert WiFi interrompu.
15. ~~Réintégration carte SD (câblage 7/8/9/10, écriture/lecture réelles,
    migration, barre de stockage web)~~ -- validée en conditions réelles
    sur `PigTeam_track` : logs écrits correctement à l'arrêt (PUSH) comme
    lors d'une coupure d'alimentation brutale (test volontaire), aucune
    corruption constatée dans les deux cas.
16. ~~Affichage du nom de circuit sur l'écran "Détail par tour" -- le bas
    du "g" de certains noms (ex. `PigTeam_track`) était coupé par le bord
    de l'écran~~ -- corrigé (baseline remontée de 2px pour laisser la
    place aux jambages de la police).

