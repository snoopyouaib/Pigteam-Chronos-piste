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
par un simple bouton poussoir** (même broche à l'origine, GPIO10 --
déplacé depuis, cf. section "Brochage boutons" plus bas) ; le tactile a
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
- **Vitesse max/min/moy, distance et heure de depart par tour** : `logGpsRow()` accumule desormais ces 5 valeurs a chaque trame GPS pendant l'enregistrement (vitesse mini/maxi/moyenne, distance cumulee par haversine, heure de la 1ere trame du tour), et `checkLapCompletion()` les ecrit en fin de ligne dans `/sessions.csv` (11 champs au total, contre 6 a l'origine puis 7 avec le seul Vmax) avant de tout reinitialiser pour le tour suivant. Affiche sur l'ecran physique (`Tour 3 : 1:23.456  (best)  Vmax 87`) ET sur la page web `/lap`, qui lit ces 5 colonnes directement depuis le carnet -- plus de calcul ni de requete au log GPS detaille a l'affichage. Retrocompatible pour la lecture : les anciennes sessions a 6/7 champs continuent de s'afficher sans planter (juste "--" pour ces 5 colonnes sur la page web -- l'ancien repli par fetch+parsing JS du log detaille a ete retire entierement pour alleger la page, plus aucun `<script>` sur `/lap`).
- **Correction "attente au paddock" comptee dans le tour 1** (28/07, valide sur log reel) : `current_lap_ms` peut rester bloque a 0 plusieurs secondes (observe : 27s) le temps que le geofence s'arme, avant le vrai depart du 1er tour -- ce temps d'attente immobile/lent etait a tort inclus dans la distance et l'heure de depart du tour 1 (Vmax/Vmin/Vavg non affectes, la vitesse pendant l'attente restant dans une plage coherente par coincidence). Fix dans `logGpsRow()` : tant qu'aucun tour n'est encore valide (`lapsCount == 0`) ET que `current_lap_ms` ne progresse pas d'une trame a l'autre, les accumulateurs (distance/vitesses/depart) sont glisses en avant plutot que de compter ce temps mort. **Piege evite en cours de route** : une 1ere version comparait `current_lap_ms` sans cette garde `lapsCount == 0` -- elle se redeclenchait a CHAQUE transition entre tours (le compteur du tour suivant peut deja apparaitre dans `getDisplayState()` avant que `checkLapCompletion()` ait pu lire les stats du tour qui vient de se terminer, confirme sur log reel : le tour 2 demarre direct a 1182ms sans jamais repasser par 0), ce qui aurait efface les stats de TOUS les tours suivants au lieu de corriger juste le 1er -- attrape uniquement grace a une simulation Python du log reel avant tout flash, pas en theorie.
- **Regression corrigee le lendemain (29/07)** : la version ci-dessus limitait la CAPTURE du depart au seul cas `lapsCount == 0` (plateau), en supprimant au passage l'ancienne capture generale -- consequence : le depart du tour 1 restait bon, mais celui des tours 2, 3... restait bloque sur le placeholder `--:--:--` (confirme sur un test terrain reel de 2 tours le 29/07, `sessions.csv` a l'appui). Fix : capture generale du depart retablie (`if (currentLapSpeedSamples == 0) ...`, fire sur la 1ere trame accumulee apres N'IMPORTE quel reset, y compris celui de fin de `checkLapCompletion()`), EN PLUS de la logique de plateau specifique au tour 1 -- les deux mecanismes cohabitent sans se marcher dessus. Revalide par simulation Python sur le log reel du 29/07 : tour 1 retombe pile sur les valeurs deja enregistrees (07:28:53, 0.86 km, 53/26/43), tour 2 obtient enfin un depart coherent (07:30:07).
- **Hints de navigation supprimés** : les petits messages en haut à
  droite de chaque écran de l'anneau (Circuit, Connexion, Sessions,
  Réglages, WiFi, Nouveau circuit -- ex. "Tap: choisir BACK: statut")
  ont été retirés, ainsi que la fonction `createHint()` devenue
  inutile. Le "PRESS REC" clignotant en bas à droite de l'écran Statut
  est conservé (ce n'est pas un hint de navigation mais l'invite
  fonctionnelle qui indique quand appuyer sur REC).

## IMU (QMI8658) -- angle d'inclinaison en virage, log seul (29/07)

**Calibration du biais gyro ajoutée (30/07)** suite à un test réel :
sur PigTeam Track (circuit qui ne tourne qu'à droite), l'angle max
"à gauche" ressortait significatif (26-32°) sur les 3 tours d'une
session -- signature classique d'une dérive de gyroscope non corrigée
(le filtre complémentaire fait confiance à 98% à l'intégration gyro ;
même un petit biais au repos, quelques dixièmes de °/s, s'accumule
linéairement et donne des dizaines de degrés de dérive sur un tour de
~50s). Fix : `initImu()` calibre maintenant le biais des 3 axes gyro
au démarrage (50 échantillons sur ~700ms, carte supposée immobile à la
mise sous tension), et le retire de l'axe roulis (gyroZ) avant
intégration. **Pas encore revalidé sur piste** -- prochain test à
confirmer.

Le board 1.91 a un accelerometre+gyroscope 6 axes (QMI8658) deja
partage sur le meme bus I2C que le tactile FT3168 -- valide au
bring-up (`README_AMOLED_bringup.md`, etape "02_I2C_QMI8658") mais
jamais integre au firmware jusqu'ici. Ajout demande : **angle
d'inclinaison en virage (lean angle), journalise uniquement -- aucun
affichage ecran** (pas le temps de regarder un ecran en pleine
inclinaison).

- **Pas de lib Arduino/Wire** : une lib QMI8658 classique
  (`Wire`/`TwoWire`) ferait son propre `i2c_driver_install()` sur le
  meme port I2C_NUM_0 deja pris par `I2C_master_Init()` (driver ESP-IDF
  natif) -- exactement le conflit que `touch_bsp.c` evite deja pour le
  FT3168 (meme bus physique). Nouveau module `lib/imu/ImuManager.*`
  ecrit en registres bas niveau via `I2C_write_buff`/`I2C_read_buff`
  (memes fonctions partagees que le tactile), pas de dependance
  externe ajoutee a `platformio.ini`.
- **Angle par filtre complementaire** (98% integration gyro / 2%
  recalage accelerometre) plutot qu'un simple `atan2(accel)` : sous
  acceleration laterale en virage, l'accelerometre seul confond
  gravite et force centripete et donnerait un angle faux. Le gyro
  integre est precis a court terme mais derive lentement -- le
  melange des deux reste correct dans la duree sans etre perturbe par
  un virage de quelques secondes.
- **7 champs ajoutes en fin de ligne** de `log_*.csv`
  (`lean_angle_deg,accel_x_mg,accel_y_mg,accel_z_mg,gyro_x_dps,gyro_y_dps,gyro_z_dps`),
  meme principe de retrocompatibilite que les ajouts precedents a
  `/sessions.csv` -- les valeurs brutes sont loggees en plus de l'angle
  calcule, pour pouvoir recalculer/corriger en post-traitement si le
  mapping d'axe s'avere faux une fois monte definitivement.
- **⚠️ A valider physiquement avant de faire confiance aux logs**,
  exactement comme au bring-up d'origine : le mapping de registres
  (WHO_AM_I/CTRL/donnees) vient de la datasheet publique QMI8658C et de
  plusieurs implementations open-source croisees, mais n'a pas ete
  verifie sur ce firmware precis (contrairement au reste du projet,
  aucun test au banc n'est possible ici -- pas de vrai capteur sur le
  banc `display_only_191`). A la premiere mise en route, verifier au
  Serial : `WHO_AM_I` doit valoir `0x05`, et les valeurs au repos
  (carte immobile) doivent se stabiliser comme au bring-up (~1g sur un
  seul axe accelero, gyro quasi nul). **L'axe qui correspond au roulis
  une fois monte dans le carenage reste aussi a confirmer** (hypothese
  de travail actuelle : gyro X + `atan2(accelY, accelZ)`, a ajuster
  dans `ImuManager.cpp` si le montage final donne un axe different --
  cf. commentaire dedie dans le fichier).

## Wheelie / stoppie -- detection par seuil, log seul (29/07)

Suite logique de l'IMU : detecter les levages de roue (avant =
wheelie, arriere = stoppie). **Pas de calcul d'angle de tangage continu
ici**, contrairement au roulis -- sous forte acceleration/freinage,
l'accelerometre confond acceleration lineaire et gravite bien plus
violemment qu'en virage, un angle calcule serait denue de sens pendant
exactement les evenements qu'on veut detecter. Approche plus robuste et
standard en telemetrie moto : **detection par seuil sur la vitesse de
tangage brute** (gyro Y, cf. mapping d'axe ci-dessous), avec hysteresis
pour ne compter qu'une fois un evenement soutenu.

- **Mapping d'axe complet** (deduit par symetrie geometrique a partir du
  test roulis validé sur le vrai board) : roulis = gyroZ (corrige le
  29/07 -- gyroY avait ete suppose par erreur au depart), tangage =
  gyroY, lacet = gyroX. Seul le roulis a ete teste physiquement (carte
  penchee a gauche/droite sur l'etabli) -- tangage/lacet n'ont pas pu
  l'etre de la meme facon, un wheelie/stoppie ne se simule pas a la
  main.
- **2 champs ajoutes en fin de ligne** de `/sessions.csv`
  (`wheelie_count`, `stoppie_count` -- 13 champs desormais), meme
  principe de retrocompatibilite que les ajouts precedents.
- **⚠️ Seuil/durée NON validés sur piste** : `WHEELIE_GYRO_THRESHOLD_DPS`
  (45°/s) et `WHEELIE_MIN_DURATION_MS` (250ms) sont des valeurs de
  depart a affiner apres un premier roulage reel selon les faux
  positifs/negatifs constates (vibrations moteur/route vs vrai levage
  de roue). Le **sens est confirme et fiable** : une premiere
  validation par instantanes ponctuels (commande Serial `i`) avait
  donne des signes contradictoires d'un essai a l'autre (impossible de
  viser a la main le pic exact d'un mouvement aussi rapide) -- resolu
  par l'ajout d'une commande `w` qui imprime gyroY en flux continu
  pendant 3s pendant le geste. Le flux complet confirme sans ambiguite :
  gyroY negatif = roue avant qui se leve (wheelie, pic a -203°/s
  observe), gyroY positif = roue arriere qui se leve (stoppie, pic a
  +292°/s observe), les deux mouvements et leurs transitions bien
  visibles dans une seule capture continue.
- **Angle max a droite/a gauche par tour ajoute** (meme jour) : suite
  logique du roulis deja logge point par point -- `logGpsRow()`
  maintient desormais `currentLapMaxAngleRightDeg` (max simple) et
  `currentLapMaxAngleLeftDeg` (min simple, le plus negatif) a chaque
  trame, ecrits par `checkLapCompletion()` comme 2 champs
  supplementaires en fin de ligne de `/sessions.csv` (`%.0f,%.0f` en
  valeur absolue -- 15 champs desormais). Affiche sur la page web
  `/lap` (colonnes "Angle D"/"Angle G", `--` si absent sur une session
  plus ancienne). Toujours pas d'affichage ecran physique (meme
  principe que l'angle en continu : pas le temps de regarder un ecran
  en pleine inclinaison).

## Mode Route -- parcours libre sans détection de tour (29/07)

Nouvelle ligne "Mode Route" dans Réglages (toggle par tap, ON/OFF) pour
enregistrer n'importe quel trajet sans notion de circuit -- balade à
vélo, trajet en voiture. GPS/vitesse/distance/IMU (angle, wheelie/
stoppie) toujours enregistrés en continu, mais **aucune détection de
tour** : pas de ligne de départ-arrivée, pas de geofencing, pas de
`CourseManager` engagé.

- **PAS PERSISTÉ** (contrairement à "Pause chrono") : revient toujours
  à OFF au démarrage -- mode explicite à réactiver à chaque usage, par
  sécurité (pas de risque d'oublier le chrono "bloqué" en mode Route
  après une balade).
- **"Route" affiché à la place du nom de circuit** partout où celui-ci
  apparaît (écran Statut, Connexion, logs).
- **Écran Statut adapté** : le gros chrono devient un simple
  chronomètre (durée écoulée depuis le départ, ne repart jamais à
  zéro), "Dernier/Best/Tours" masqués (aucun sens sans tours).
- **`/sessions.csv`** : une seule ligne "Route" écrite à l'arrêt
  définitif (pas à chaque pause), avec les mêmes statistiques que pour
  un tour de circuit (Vmax/Vmin/Vmoy/distance/départ/wheelie-stoppie/
  angle max) accumulées sur tout le trajet -- `lap_number` fixé à 1,
  `best_lap_time` = `lap_time` (pas de notion de "meilleur" sur un
  parcours unique).
- **Toggle bloqué pendant un enregistrement** : changer de mode en
  plein REC laisserait les accumulateurs de stats dans un état
  incohérent (déjà utilisés pour un circuit, plus pour une route, ou
  l'inverse) -- il faut arrêter avant de basculer.
- Réutilise entièrement les contrôles REC/pause/arrêt existants (PUSH/
  BACK, écran de confirmation) -- aucune nouvelle interaction à
  apprendre, seule la détection de tour est désactivée.
- **Bug corrige : session Route invisible sur le webserver** (meme
  jour, trouve en testant l'ajout des noms de circuit) :
  `finalizeRouteSessionIfNeeded()` n'etait appelee qu'a la confirmation
  d'arret definitif (`SCR_CONFIRM_STOP`), qui intervient APRES que
  `stopRecording()` ait deja ecrit le marqueur `# session arretee` --
  la ligne "Route" atterrissait donc EN DEHORS du bloc demarree/arretee
  que `loadSessionSummaries()` utilise pour rattacher les tours a une
  session, et etait completement ignoree (`lapCount` reste a 0) : ni
  "Route", ni nom de circuit, ni lien de detail n'apparaissaient nulle
  part sur le webserver. Fix : appel deplace au tout debut de
  `stopRecording()` lui-meme, avant l'ecriture du marqueur. Consequence
  du deplacement (`stopRecording()` sert aussi de pause) : si Route est
  mis en pause puis repris plusieurs fois avant l'arret definitif,
  chaque segment ecrit desormais sa propre ligne "Route" au lieu d'une
  seule pour tout le trajet -- pas une perte par rapport a avant (les
  donnees etaient deja perdues/invisibles), juste rendu visible.
  `SessionSummaryLite` retient desormais aussi le nom de circuit de la
  session (`isRouteSummary()`, detection par `circuit == "Route"`) --
  la carte d'accueil et la liste `/debug` affichent "Route -- duree
  XX:XX.XXX" au lieu de "N tour(s) -- meilleur : ...". Sur la page
  `/lap`, meme detection en 1ere passe (scalaire, avant le titre) :
  titre "Detail du parcours (mode Route)", colonnes Tour/Diff/Circuit
  masquees (sans objet -- une seule ligne, pas de classement, circuit
  toujours "Route"), seule la Duree reste en 1ere colonne suivie de
  Depart/Distance/V.max/V.min/V.moy/Wheelie/Stoppie/Angle D/Angle G,
  identiques a un tour de circuit.
- **Nom de circuit affiche sur chaque session** (accueil + `/debug`,
  meme jour) : la carte d'une session circuit affiche desormais
  "N tour(s) -- meilleur : TIME -- NomDuCircuit". Pour une session
  Route, le resume s'enrichit de la distance et de la V.moy totales
  ("Route -- duree TIME -- X.XX km -- V.moy XX km/h") -- possible
  directement car une session Route n'a qu'UNE seule ligne dans
  `/sessions.csv` (le total du trajet). Pour une session multi-tours,
  distance/V.moy ne sont PAS affichees a ce niveau resume : la derniere
  ligne lue par `loadSessionSummaries()` ne serait que le dernier tour,
  pas un cumul sur toute la session -- afficher ca aurait ete trompeur.
  Toujours accessible via le detail complet tour par tour (`/lap`).

- **Timeout automatique de l'écran de pause supprimé** : "Enregistrement
  en pause" (BACK pendant le REC) restait auparavant armé au maximum
  5 minutes avant de confirmer l'arrêt tout seul (filet de sécurité en
  cas d'oubli). Retiré -- l'écran reste maintenant armé indéfiniment
  tant que PUSH (reprendre) ou BACK (arrêter) n'est pas pressé. Devient
  vraiment utile avec le **mode Route** (pause volontaire en cours de
  trajet, ex. un arrêt café, sans limite de temps à respecter). La
  garde anti-faux-contact de 600ms à l'entrée de l'écran est conservée
  (mécanisme distinct, protège contre un rebond tactile/mécanique
  immédiat, pas contre un oubli prolongé).

- **Timeout automatique de l'écran de pause supprimé** : "Enregistrement
  en pause" (BACK pendant le REC) restait auparavant armé au maximum
  5 minutes avant de confirmer l'arrêt tout seul (filet de sécurité en
  cas d'oubli). Retiré -- l'écran reste maintenant armé indéfiniment
  tant que PUSH (reprendre) ou BACK (arrêter) n'est pas pressé. Devient
  vraiment utile avec le **mode Route** (pause volontaire en cours de
  trajet, ex. un arrêt café, sans limite de temps à respecter). La
  garde anti-faux-contact de 600ms à l'entrée de l'écran est conservée
  (mécanisme distinct, protège contre un rebond tactile/mécanique
  immédiat, pas contre un oubli prolongé).

## Brochage boutons -- déplacé du GPIO10/14, standardisé GPIO2 (29-30/07)

PUSH et BACK ont été déplacés sur d'autres broches pour des raisons
pratiques de soudure (accès plus facile en bord de carte). Un 2e
chrono identique est en cours de montage (même firmware, même
matériel) -- **standardisé sur GPIO2** pour BACK sur les deux
exemplaires (le 1er, d'abord soudé par erreur sur GPIO3, a été
resoudé) :

| Bouton | Broche d'origine | Broche actuelle |
|---|---|---|
| PUSH | GPIO10 | **GPIO16** |
| BACK | GPIO14 | **GPIO2** |

Vérifiés sans conflit avec le reste du brochage du firmware (écran
QSPI : 5/6/7/17/18/47/48, I2C partagé tactile+IMU : 39/40, GPS UART :
12/13 (déplacé depuis 43/44, cf. section dédiée plus bas -- 04/08), SD SDMMC : 8/9/42, batterie ADC : 1), et sans être des broches
de strapping sur l'ESP32-S3 (celles-ci sont 0/3/45/46 -- GPIO3 avait
été utilisé un temps par erreur de soudure sur le 1er chrono, sans
conséquence pratique constatée mais écarté par principe au profit de
GPIO2, qui n'a aucune fonction de strapping). GPIO17 avait été
envisagé pour PUSH mais écarté : déjà utilisé comme reset de l'écran
(`EXAMPLE_PIN_NUM_LCD_RST`).

Alimentation : VBUS (5V USB, présent uniquement câble branché) écarté
au profit de VSYS (alimentation système, toujours présente batterie ou
USB) pour un point de soudure mieux placé en bord de carte -- sans
lien avec les boutons, notée ici pour mémoire.

**Piège annexe rencontré en assemblant le 2e chrono** : mêmes module
GPS et fournisseur que le 1er, mais fils TX/RX inversés par rapport au
premier exemplaire (câblage/connecteur pré-serti différent d'un lot à
l'autre, le module lui-même n'a pas changé de convention). Réflexe à
avoir sur tout futur exemplaire : si aucune trame NMEA ne sort au
premier branchement, essayer l'inverse avant de chercher plus loin --
ne pas supposer que le câblage du 1er exemplaire est transposable tel
quel.

## Seuil de détection de ligne augmenté 7m -> 10m (01/08)

Franchissement raté constaté sur Carole (session du 01/08, tour 1 =
3:43 au lieu de ~1:11 -- en fait 3 vrais tours fusionnés, faute de
détection sur les 2 premiers franchissements). Distance réelle
mesurée à la ligne au moment des passages manqués : ~17-19m puis
~10m -- au-dessus du seuil de 7m (`crossingThresholdMeters`,
`main.cpp`, `new CourseManager(...)`) alors que la ligne de Carole est
déjà large (12,9m, cf. tableau plus bas). **Cause identifiée : la
vitesse**, pas la largeur de ligne -- à 130-200 km/h (36-55 m/s),
même à 10Hz GPS le véhicule avance de 3,6 à 5,5m entre deux mesures,
et sur un passage tangentiel (pas perpendiculaire à la ligne) le point
le plus proche réel peut tomber entre deux échantillons sans jamais
être mesuré sous le seuil. Même cause probable derrière le
franchissement raté de PigTeam Track fin juillet (ligne élargie
8m->10m à l'époque, cf. section dédiée plus bas -- corrige la
symptomatologie sur ce circuit lent, mais le seuil lui-même restait
sous-dimensionné pour un circuit rapide comme Carole). Seuil global
passé à 10m -- s'applique à tous les circuits, pas seulement Carole.

## Nouveautés du 29/07

- **Écran Connexion enrichi** : en plus de GPS et Circuit, affiche
  désormais le **stockage** (`SD OK  x.x/y.y GB` en blanc, ou
  `SD --  repli LittleFS` en orange si la carte est absente/en panne --
  via `gpsLogsOnSd`/`sdUsedBytes()`/`sdTotalBytes()`, déjà exposés par
  `SdLogStorage.h`) et la **batterie** (`Batt XX%  (Y.YYV)`, rouge si
  ≤15%) -- un coup d'œil suffit pour vérifier que tous les modules
  répondent avant de partir rouler.
- **Sécurité batterie faible ("NO BAT")** : sous 5%
  (`LOW_BATT_NO_REC_PERCENT`), le "PRESS REC" clignotant de l'écran
  Statut devient "NO BAT" et PUSH ne démarre plus d'enregistrement --
  évite de couper l'alimentation en plein tour (perte du log en cours)
  et d'user une batterie déjà quasi vide. Ne bloque que le
  **démarrage** : un enregistrement déjà en cours n'est pas interrompu,
  et la reprise depuis l'écran "Enregistrement en pause" n'est pas
  (encore) protégée par ce même seuil -- l'utilité réelle de cet écran
  de pause/reprise est encore en réflexion, pas la peine d'y ajouter la
  garde tant que ce n'est pas tranché.
- **Chrono figé au passage de ligne, désormais réglable ("Pause
  chrono")** : au lieu de repartir instantanément à 0 après un tour, le
  gros chrono reste affiché sur le temps du tour qui vient de se
  terminer pendant `lapFreezeS` secondes (`checkLapCompletion()` arme
  `lapFreezeUntilMs` à chaque tour -- un tour plus rapide que ce délai
  écourte naturellement le gel précédent au profit du nouveau temps).
  N'affecte que l'affichage, rien n'est retardé côté
  enregistrement/logs. Réglable depuis l'écran **Réglages** -- une
  nouvelle ligne "Pause chrono" sous "WiFi téléchargement", un tap
  cycle parmi 0 (désactivé) / 5 / 10 / 15 / 20 / 30s -- persisté sur
  LittleFS (`/settings.csv`) pour survivre à un redémarrage, 10s par
  défaut si le fichier est absent/corrompu.

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

## Diagnostic GPS bloqué à 1Hz + déplacement GPIO 43/44 -> 12/13 (04/08)

Suite aux sessions ratées à Carole du 01/08 (cf. section seuil ci-dessus)
et au retour terrain "vitesse affichée qui plafonne bizarrement au fil
des tours" : analyse des logs GPS avec `millis_boot` (précision ms) a
révélé que le GPS tournait en réalité à **1Hz, pas 10Hz** malgré la
config `PAIR050,100` envoyée au boot -- delta constant de ~1000ms entre
fixes traités sur toutes les sessions du 01/08. À 190km/h ça représente
~53m entre deux mesures, largement au-dessus du seuil de détection de
ligne (même à 10m) -- explique à la fois les tours fusionnés et les
sessions à 0 tour détecté du 01/08.

**Cause : aucun ACK vérifié.** `configureFixRate10Hz()` envoyait la
commande sans jamais lire la réponse du module. Ajout de
`confirmFixRateAck()` (lit `$PAIR001,050,x`) et `measureActualRmcRate()`
(mesure le débit RMC réel sur 2s, indépendamment de l'ACK) au boot --
les deux sont maintenant systématiques et visibles au boot, et le débit
mesuré (`gpsMeasuredRmcHz`) est aussi affiché sur l'écran Connexion
(`GPS OK Fix:x Sat:y NHz`, en rouge si <8Hz) puisqu'on n'a jamais le
serial sur la moto.

**Diagnostic** : aucun ACK reçu, et plus révélateur, les trames
GLL/GSV/VTG (censées être coupées par `PAIR062`) continuaient de sortir
-- donc **aucune** commande `$PAIR` n'atteignait le module, pas
seulement `PAIR050`. Éliminé par élimination : pas un souci de firmware
LC76G (10Hz déjà validé sur le montage TFT avec le même module et la
même commande, cf. conversation du 27/07), pas une broche ESP32 morte
(même symptôme identique après déplacement complet 43/44 -> 12/13, sur
deux GPIO indépendantes). Conclusion : **sertissage TX/RXD insuffisant**
sur le connecteur JST-PH 2.5mm reliant le module au connecteur --
tenait mécaniquement (le loquet clipsait) sans contact électrique
fiable en émission, alors que la réception passait par une autre paire
de broches sur le même connecteur, d'où "réception GPS parfaite, module
totalement muet aux commandes" comme signature du problème.

**Fix retenu** : point de soudure par-dessus le sertissage existant
(crimp + soudure, pas soudure seule -- le crimp garde la tenue
mécanique). 10Hz confirmé au flash suivant (20 trames RMC/2000ms).

**Broches** : passées de GPIO43/44 à **GPIO12/13** pendant le
diagnostic (pour écarter l'hypothèse broche ESP32 morte) -- conservées
telles quelles après coup, aucune raison technique de revenir en
arrière une fois le vrai problème (sertissage) identifié et corrigé.
Le 2e exemplaire en cours de montage reste sur son brochage propre sauf
si son fil GPS présente le même symptôme.

Seuil de détection de ligne laissé à 10.0m (pas de retour à 7.0m
envisagé un temps pendant ce diagnostic, cf. section ci-dessus) : à
10Hz réel l'espacement entre mesures tombe à ~5m à 190km/h, largement
dans la marge du seuil actuel.

## Anti-faux-contact BACK/PUSH pendant l'enregistrement (04/08)

Constaté sur le montage SV (bicylindre en V, vibrations plus marquées
qu'un 4-cylindres) : retour d'un petit tour avec le chrono affichant
l'écran Circuit au lieu de l'écran Statut, sans action volontaire.

**Cause** : `handleBack()` sur `SCR_STATUS` va directement vers
`SCR_CIRCUIT` pendant un enregistrement, sans écran de confirmation
(contrairement à PUSH qui passe par `SCR_CONFIRM_STOP`) -- une seule
impulsion parasite sur le bouton suffit donc à naviguer hors de l'écran
Statut. Le debounce logiciel existant (`BACK_DEBOUNCE_MS`/
`PUSH_DEBOUNCE_MS`, cf. section "Brochage boutons") ne protège pas
contre ça : il filtre le rebond d'un vrai appui (fronts rapprochés),
pas une impulsion isolée qui ressemble en tout point à un vrai front
descendant côté ISR. Vraisemblablement mécanique (bouton-poussoir
excité par les vibrations du bicylindre), pas un souci logiciel --
l'enregistrement lui-même n'est pas coupé dans ce cas précis (pas
d'appel à `stopRecording()` dans ce branchement), seul l'affichage
change, donc pas de perte de données constatée.

**Fix retenu** : délai de confirmation par maintien, appliqué
uniquement au cas à risque (`SCR_STATUS` + enregistrement en cours),
pour BACK et PUSH -- ailleurs (menus, écran de confirmation, hors REC)
les deux boutons restent instantanés. Le bouton doit rester
physiquement appuyé (`digitalRead(...) == LOW`) pendant
`BACK_HOLD_FOR_REC_MS` / `PUSH_HOLD_FOR_REC_MS` (300ms chacun, doublé
depuis 150ms initial) avant que l'action ne s'exécute réellement ;
relâché avant ce délai, l'appui est ignoré silencieusement. Implémenté
via deux variables d'état (`backHoldPendingSinceMs`/
`pushHoldPendingSinceMs`) vérifiées à chaque tour de `loop()`,
indépendamment du flag d'ISR qui ne reste vrai qu'au moment du front
descendant lui-même.

Piste matérielle envisagée mais pas encore retenue : condensateur de
découplage (~0,22µF, calculé pour rester très en dessous des 300ms de
la protection logicielle avec le pull-up interne ~45kΩ de l'ESP32 --
un 1µF testerait la limite, cf. calculs faits en conversation) en
parallèle des contacts du bouton. Laissé de côté pour l'instant, la
correction logicielle suffit -- à réévaluer si le symptôme persiste
malgré tout sur circuit.

**Complément** : la protection BACK/PUSH réduit le risque de quitter
l'écran Statut par erreur, mais ne l'annule pas complètement (un
maintien parasite de 300ms+ reste possible). Si ça arrive quand même,
l'écran Circuit a le tactile pleinement actif, contrairement à Statut
(swipe + tap non câblés du tout dessus, cf. `enableRingSwipe()` jamais
appelé sur `scrStatus` -- vérifié, pas juste par absence). Or un tap
sur une ligne de circuit valide **immédiatement** le changement, sans
confirmation. Bloqué directement dans `circuitRowTappedCb()` : tap
ignoré (juste un log Serial) si `recordingEnabled` est vrai, pour
qu'un tap parasite en pleine session ne puisse plus changer le circuit
actif sous `CourseManager`. Même garde-fou ajouté sur les autres tap
directs identifiés : nouvelle capture de circuit
(`startCaptureTappedCb`), réglage de pause (`freezeRowTappedCb`) et
ouverture WiFi (`settingsRowTappedCb` -- celui-ci cumulait en plus le
risque déjà documenté de contention mutex LVGL/WiFi si le softAP
démarrait pendant un enregistrement). `routeRowTappedCb` (Mode Route)
avait déjà ce garde-fou de son côté, mis en place lors de son
implémentation initiale.

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
