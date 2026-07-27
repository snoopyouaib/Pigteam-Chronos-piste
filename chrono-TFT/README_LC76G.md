# GPS Quectel LC76G -- notes de portage (sous-projet `/gps_LC76G`)

Remplacement du GPS u-blox NEO-M9N (UBX binaire, NAV-PVT) par un module
Quectel LC76G (Waveshare, Multi-GNSS) parlant NMEA 0183 en natif.
Sous-projet isolé pour valider le portage sans toucher au montage
principal validé -- fusionner dans le projet principal une fois le test
terrain sur `PigTeam_track` concluant.

**Seul `GpsManager.cpp` change.** `GpsManager.h` reste strictement
identique (`GpsData`/`liveData`/`newGpsData`/`gpsActive`/`initGps()`/
`pollGps()`) -- c'était l'objectif de l'interface dès sa conception, cf.
commentaire d'en-tête du `.h`. Aucune modif nécessaire côté
`CourseManager`, `main.cpp`, `WebServerManager` ou affichage.

## Câblage

Connecteur module : **JST GH, 1.25mm de pas, 8 broches**, avec
loquet de sécurité. Seuls 4 des 8 fils sont utilisés :

| Fil (module) | GPIO ESP32-S3-Tiny |
|---|---|
| TXD (→ RX ESP32) | 6 |
| RXD (← TX ESP32) | 5 |
| VCC | alim 3.3V ou 5V (module tolère les deux) |
| GND | GND commun |

SDA/SCL (I2C, non utilisé -- on reste en UART), RST et PPS (non câblés
pour l'instant, cf. section PPS plus bas) laissés de côté, isolés
individuellement pour éviter tout court-circuit une fois dans le
boîtier.

**Pièges rencontrés à l'achat/au câblage :**
- **Connecteurs JST GH1.25 "génériques" incompatibles** : plusieurs
  connecteurs achetés séparément comme "JST GH 1.25mm" avec loquet ne
  rentraient pas sur le module (mesurés ~1mm plus larges au pas) --
  pas le même standard malgré l'étiquette. **Solution retenue** :
  utiliser les câbles pré-sertis fournis avec le module plutôt que
  d'en racheter/sertir soi-même, plus fiable pour un usage vibrant
  (moto) qu'un sertissage 1.25mm fait maison.
- **La couleur des fils du câble pré-serti ne suit PAS la convention
  habituelle** (confirmé par la doc Waveshare elle-même : "the red
  cable is not VCC and the black cable is not GND") -- toujours
  vérifier au multimètre en continuité contre la sérigraphie du module
  avant de souder quoi que ce soit sur le support de déport.
- **Déport prévu** : petits fils fins du connecteur GH1.25 soudés sur
  un support avec connecteur JST-XH classique, puis liaison en fil
  plus solide (24-26 AWG) vers l'ESP32. Point de colle chaude/époxy sur
  la jonction fil fin → pad de soudure (le vrai point faible en
  vibration, pas le fil lui-même).
- **Paire TX/RX torsadée** recommandée sur la longueur du câble de
  déport, le module étant monté à l'avant (araignée sous la bulle,
  près du tableau de bord) -- plus proche de la bobine d'allumage/fils
  de bougie que ne l'aurait été un montage sous-cadre arrière, donc
  un peu plus exposé au bruit électrique.

## Protocole : `$PAIR`, pas `$PMTK`

**Piège majeur identifié au banc** : contrairement aux anciens modules
GlobalTop/MTK (protocole legacy `$PMTK...`), le LC76G utilise le jeu de
commandes **propriétaire Quectel `$PAIR...`**. Les commandes PMTK
classiques (`PMTK220`, `PMTK300`, `PMTK314`) sont syntaxiquement
acceptées (checksum valide, le module répond) mais **rejetées** --
réponse du type `$PMTK314,ERROR,3` au lieu de l'accusé de réception
standard `$PMTK001,cmd,code`. Confirmé par un fil du forum Quectel avec
exactement le même symptôme.

Commandes PAIR utilisées (mêmes encodage/checksum que PMTK -- XOR de
tous les caractères entre `$` et `*`) :

- **`PAIR050,<ms>`** (`PAIR_COMMON_SET_FIX_RATE`) -- intervalle de
  calcul du fix, 100-1000ms (100 = 10Hz). **Certaines variantes LC76G
  (PA)/(PB) ne supportent pas cette commande** d'après la doc Quectel
  et restent bloquées à 1Hz quoi qu'on fasse -- si l'accusé PAIR001
  échoue malgré la bonne syntaxe, c'est probablement une limite
  matérielle du module reçu, pas un bug firmware.
- **`PAIR062,<type>,<taux>`** (`PAIR_COMMON_SET_NMEA_OUTPUT_RATE`) --
  active/coupe individuellement chaque trame NMEA (type 0=GGA, 1=GLL,
  2=GSA, 3=GSV, 4=RMC, 5=VTG). On garde GGA/GSA/RMC à `1` (une fois par
  fix), on coupe GLL/GSV/VTG à `0` pour réduire le volume à parser --
  logique équivalente au `outProtoMask` UBX-only côté NEO-M9N.

**Accusé de réception** : `$PAIR001,<cmd>,<résultat>` -- **`0` = succès**
en protocole PAIR (attention, c'est l'inverse de la convention PMTK où
`3` = succès). Loggué en clair quand `GPS_DEBUG_LOG` est actif.

Résultat confirmé au banc : **10Hz effectif** une fois PAIR050+PAIR062
acceptés (plusieurs lignes de log par seconde, timestamp identique
répété).

## Parsing NMEA

Trois trames exploitées, dispatchées par les 3 derniers caractères du
type de trame (`RMC`/`GGA`/`GSA`, peu importe le talker `GN`/`GP`/`GL`/
`GA` qui varie selon les constellations utilisées pour le fix) :

- **RMC** : heure/date UTC, position, vitesse. Ignorée si statut ≠ `A`
  (pas de fix) -- logique équivalente aux bits `validDate`/`validTime`
  de NAV-PVT côté UBX.
- **GGA** : altitude + nombre de satellites (absents de RMC). Ignorée
  si qualité de fix = 0.
- **GSA** : type de fix (2D/3D), mappé directement sur
  `liveData.fixStatus` (même convention que u-blox : 0/2/3, pas de
  valeur "1=dead-reckoning" en NMEA standard).

**Piège de parsing rencontré** : décalage d'un caractère au début des
champs (`fields = line + 6` au lieu de `line + 7`) -- `"$GNRMC,"` fait
7 caractères, pas 6. Symptôme observé : le champ `status` affichait un
chiffre (`'1'`) au lieu de `'A'`/`'V'`, sans plantage ni erreur de
checksum (tous les champs étaient juste décalés d'une position).
Corrigé.

## Debug (`GPS_DEBUG_LOG`)

Flag de compilation dans `GpsManager.cpp`, **désactivé par défaut**
(`0`) une fois le parsing validé -- comportement Serial silencieux
identique au NEO-M9N. Réactiver (`1`) en cas de nouveau souci :

- **Dump `liveData`** à chaque trame RMC valide (date, fix, sats, lat/
  lon, altitude, vitesse).
- **RMC sans fix** loggué explicitement (`statut='V'`) -- confirme que
  le module transmet bien, juste en attente de satellites (normal en
  intérieur/sous abri).
- **Checksum NMEA invalide** ou **ligne non-`$`** -- signe de
  désynchronisation/mauvais débit si ça persiste (occasionnel et rare,
  c'est du bruit normal sur un flux série rapide).
- **Réponses `$PAIR001`/`$PAIR010`/`$PAIR011`** -- accusés de commande
  et messages de statut spontanés du module (changement d'état
  satellite, almanach...), utiles pour diagnostiquer la config mais
  sans action à prendre dessus.

## Comparatif au banc vs NEO-M9N

- **19-20 satellites en intérieur** contre environ la moitié pour le
  NEO-M9N dans les mêmes conditions -- cohérent avec le suivi simultané
  GPS+BeiDou+GLONASS+Galileo+QZSS du LC76G (le M9N supporte aussi ces
  constellations mais limite le nombre de canaux concurrents actifs
  par défaut). Un compteur de sats plus élevé n'est pas une garantie de
  meilleure précision en usage réel -- le vrai test reste la stabilité
  de la trace GPS en mouvement sur circuit.
- **10Hz confirmé** après config PAIR050/PAIR062 (contre 10Hz natif
  côté UBX, donc pas de régression, juste une étape de config
  supplémentaire).
- **Cold start intérieur** : fix 3D obtenu en quelques secondes lors
  des tests (LED PPS clignotant régulièrement dès l'allumage), cohérent
  avec le TTFF ~35s annoncé par Quectel en conditions extérieures
  normales.

## PPS -- reporté, pas câblé pour l'instant

Décision : ne pas câbler le PPS dans cette itération. Le facteur
limitant pour un chrono GPS reste l'incertitude de position (quelques
mètres), pas la précision du timestamp -- le gain du PPS serait marginal
tant que la position elle-même n'est pas le facteur limitant, pour un
coût non négligeable (5e fil fin à souder en plus, interruption GPIO à
gérer, corrélation flanc PPS ↔ trame NMEA à implémenter). Point PPS
disponible sur le connecteur module si le besoin se confirme après le
test terrain. Si câblé un jour : **paire dédiée PPS+GND séparée de la
paire TX/RX**, pas dans le même toron (le front raide du PPS et les
transitions rapides de l'UART se perturbent mutuellement par couplage
si torsadés ensemble).

## Montage physique -- module à l'avant (araignée sous la bulle)

Choix retenu : fixation sur l'araignée qui maintient l'avant du
carénage, sous la bulle, proche du tableau de bord (câblage court).
Points de vigilance identifiés (pas de blocage connu à ce stade) :

- **Visibilité ciel à travers la bulle** : transparent aux fréquences
  GPS pour un plastique/polycarbonate classique -- vérifier au nombre
  de satellites si la bulle a un traitement métallisé ou un film
  chauffant.
- **Proximité bobine d'allumage/fils de bougie** : plus proche à
  l'avant qu'à un montage sous-cadre arrière -- renforce l'intérêt de
  la paire torsadée TX/RX, envisager une ferrite sur le câble si bruit
  constaté à l'usage.
- **Projections d'eau/gravillons** côté roue avant plus directes qu'à
  l'arrière -- boîtier imprimé à étancher correctement (joint ou évents
  orientés vers le bas).
- **Zone plus exposée en cas de chute** que le sous-cadre arrière --
  accepté comme compromis pour le câblage court.
- Orientation module : face composants vers le ciel (diagramme
  hémisphérique de l'antenne céramique interne), l'angle de la bulle
  n'est pas un problème.

## Historique des pièges rencontrés (chronologique)

1. SD `f_mount failed (3)` au premier boot -- sans rapport avec le GPS
   (bus SPI totalement séparé), résolu : mauvais branchement physique
   de la carte SD.
2. Décalage d'un caractère dans le parsing NMEA (`line+6` au lieu de
   `line+7`) -- champ `status` affichait un chiffre au lieu de `A`/`V`.
   Corrigé.
3. Config `$PMTK220`/`$PMTK300`/`$PMTK314` rejetée (`ERROR`) -- le
   LC76G ne parle pas le protocole PMTK legacy, seulement `$PAIR...`.
   Corrigé, cadence 10Hz confirmée une fois basculé sur PAIR050/PAIR062.
4. Connecteurs JST GH1.25 achetés séparément incompatibles (~1mm de
   pas en trop) -- résolu en utilisant les câbles pré-sertis fournis
   avec le module plutôt que d'en racheter/sertir.

## Prochain chantier

Test en mouvement (marche rapide/vélo) pour valider `CourseManager`
(détection de ligne, calcul de tour) avec cette source GPS, avant la
session complète sur `PigTeam_track`. Une fois concluant, fusionner ce
sous-projet dans le montage principal et mettre à jour le README.md
principal (section GPS + tableau câblage) pour refléter le LC76G comme
source GPS retenue.
