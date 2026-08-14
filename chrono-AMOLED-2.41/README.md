# chrono-AMOLED-2.41 -- portage complet (14/08)

Portage du firmware réel `chrono-AMOLED` (1.91") vers le module
`ESP32-S3-Touch-AMOLED-2.41` (SKU 30589, RM690B0 600x450, FT6336,
révision matérielle **V2** -- cf. section dédiée plus bas). Distinct de
`display_only_241`, qui reste un banc de test écran pur sans
GPS/SD/batterie/WiFi réels.

Base : bas-niveau écran/tactile/expandeur validé sur `display_only_241`
(bring-up des 12-13/08) + moteur réel de `chrono-AMOLED` (GPS,
CourseManager, SD, sessions, IMU, WebServer -- inchangé, éprouvé sur le
1.91). Toolchain unifié : `platform = 53.03.13` (confirmé fonctionnel
sur la V2 le 14/08, cf. plus bas), LVGL 8.4.0.

## État au 14/08 : portage complet, tout validé au banc

Écran, tactile, expandeur, IMU, GPS, SD, batterie, chargement des
circuits, et le `main.cpp` fusionné (moteur réel + écrans 600x450) --
tous testés avec le vrai matériel (pas de simulation) le 14/08.

## Piège V2 -- pinout officiel Waveshare pas fiable sur ce montage

Le pinout publié sur le wiki Waveshare correspond à la révision V1 de
ce module. Les deux exemplaires de ce projet sont en **V2** (silkscreen
PCB "Rev2.0", confirmé), qui diverge sur au moins deux points déjà
rencontrés :

- Reset écran/tactile : passe par un IO-expander **TCA9554** (I2C
  0x20, EXIO0=reset écran/EXIO1=reset tactile) au lieu d'une connexion
  GPIO directe -- cf. `display_only_241/README_portage_241.md` pour le
  détail du diagnostic (écran noir persistant tant que ce n'est pas
  initialisé avant `displayInit()`).
- GPIO21 non connecté sur la V2 (utilisé comme reset direct sur le
  schéma V1).

**Leçon reprise plusieurs fois pendant ce portage** : ne pas supposer
qu'une broche du pinout officiel correspond forcément au montage réel
sans le vérifier au banc -- cf. la section GPS ci-dessous, où même une
étiquette imprimée au dos de la carte ("GND 3V3 43 44") s'est avérée
correcte niveau identité de broche (confirmé par test `digitalRead`)
mais où la fonction UART sur ces pins précis restait inutilisable pour
une raison encore non élucidée.

## GPS -- migré de GPIO43/44 vers GPIO1/GPIO2

Le petit connecteur JST 4 broches du 2.41 (étiqueté GND/3V3/43/44 au
dos de la carte) était le choix naturel de départ pour le GPS -- ce
sont les pins UART0 natives de l'ESP32-S3. Diagnostic du 14/08 :

- Continuité et identité physique des broches confirmées bonnes (test
  `digitalRead` + pull-down interne : réponse nette au 3V3 sur 43 et
  44 indépendamment).
- Module GPS confirmé sain (testé avec succès sur le chrono 1.91).
- Malgré ça : **zéro trame reçue**, même dans un sketch isolé
  n'utilisant que `HardwareSerial(1)` sur ces pins, sans écran/WiFi/
  IMU/rien d'autre. Testé aux deux baudrates (9600/115200) et dans les
  deux sens de câblage TX/RX.
- Root cause non identifiée avec certitude -- hypothèse la plus
  probable : liaison matérielle par défaut de ces broches à l'UART0
  (console ROM/bootloader) qui empêcherait leur réattribution complète
  à l'UART1 sur ce core, sans que ça se voie au niveau GPIO simple.
- **Migré vers GPIO1/GPIO2** : confirmé fonctionnel (fix 3D 10Hz, 20+
  satellites) aussi bien en isolé que dans le firmware complet.

**Conséquence matérielle** : GPIO1/2 ne sont **pas sortis** sur le
connecteur JST GPS du 2.41. Le module doit être câblé en filaire
direct sur le header 34 broches (GPIO1 = TX module, GPIO2 = RX
module), le connecteur JST intégré reste inutilisable pour le GPS sur
ce montage tant qu'aucune révision matérielle ne sort GPIO1/2 dessus.

## SD -- repinée GPIO4/5/6 (SDMMC 1-fil)

Le montage 1.91 (GPIO8/42/9 en SD_MMC) était en conflit direct sur le
2.41 : GPIO8 = BACK_BUTTON, GPIO9 = QSPI_CS de l'écran. Le 2.41 a un
connecteur SD dédié au pinout officiel (CS=GPIO2, SCLK=GPIO4,
MOSI/CMD=GPIO5, MISO/D0=GPIO6). Repris en SDMMC 1-fil comme le 1.91
(CLK/CMD/D0 uniquement, la broche CS du pinout n'est pas utilisée dans
ce mode) : **montée du premier coup**, aucun souci constaté.

## Batterie -- ADC repiné + activation GPIO16 + calibration

Trois problèmes distincts rencontrés et corrigés, dans l'ordre :

1. **Mauvaise broche ADC.** Le 1.91 lit la batterie sur
   `ADC_UNIT_1`/`ADC_CHANNEL_0` (GPIO1) -- qui sur le 2.41 est une
   broche libre non connectée à la batterie (et maintenant utilisée
   pour le GPS, cf. plus haut). Le pinout officiel place `BAT_ADC` sur
   **GPIO17 = ADC_UNIT_2 / ADC_CHANNEL_6**. Repiné en conséquence
   (`lib/adc/adc_bsp.cpp`).
2. **Rail batterie non alimenté sans activation explicite.** Même
   après le repinage ADC, tension lue proche de 0V (`NO BAT`) alors
   qu'une vraie batterie était branchée. Cause : `GPIO16` (`BAT_Control`,
   documenté par Waveshare via `BAT_GPIO_Init()`/`BAT_ON()`/`BAT_OFF()`)
   doit être piloté en HAUT pour connecter effectivement le rail
   batterie au diviseur lu par l'ADC. Sans ça, l'ADC lit une entrée
   flottante même batterie physiquement présente.
3. **Facteur du pont diviseur.** Repris à x2 du 1.91 initialement (par
   défaut, non vérifié) -- s'est avéré faux sur le 2.41 (circuit
   différent). Calibré au multimètre sur plusieurs points de charge
   (3.85V, 3.94V, 4.15V réels) : légère non-linéarité de l'ADC
   observée (facteur idéal ~2.90 en bas de plage, ~2.98 en haut).
   Retenu **x2.94**, un compromis qui priorise la précision près de la
   pleine charge (partie la plus raide de la courbe
   `batteryVoltageToPercent()` -- 15 points de % pour seulement 0.15V
   d'écart entre 4.0V et 4.15V, donc c'est la zone où un petit écart de
   tension se voit le plus sur le pourcentage affiché).

## Démarrage et extinction sur batterie

Le 2.41 n'a pas de puce de gestion d'énergie dédiée comme l'AXP2101 du
1.91 -- juste un circuit discret bouton PWR (GPIO15) + latch piloté par
logiciel (`BAT_ON()`/`BAT_OFF()`, GPIO16).

- **`BAT_ON()` doit être appelé tout au début de `setup()`**, avant
  même `Serial.begin()`. Le bouton PWR n'alimente le micro que le
  temps qu'il démarre ; c'est au firmware de reprendre la main très
  vite en pilotant GPIO16, sinon l'alimentation retombe au relâchement
  du bouton avant que le reste du code n'ait eu le temps de s'exécuter
  (symptôme observé avant le fix : LED du module GPS qui s'allume
  pendant l'appui puis s'éteint au relâchement, la carte ne finissait
  jamais son boot).
- Comportement final, voulu : **sur USB**, démarrage direct sans
  bouton (alimentation permanente). **Sur batterie**, appui sur PWR
  nécessaire pour démarrer -- normal, comportement voulu par le
  circuit d'alimentation Waveshare, pas un bug.
- **Extinction propre ajoutée** : appui long de 2s sur PWR
  (`PWR_HOLD_OFF_MS`) arrête un enregistrement en cours puis coupe
  l'alimentation (`BAT_OFF()`). Sans effet sur USB (l'alimentation USB
  maintient la carte indépendamment de GPIO16).
- Validé fonctionnel avec les vrais boîtiers batterie 2000mAh/TP4056
  du projet.

## Boutons

| Bouton | GPIO | Rôle |
|---|---|---|
| PUSH | 18 | Navigation (avance / démarre l'enregistrement) |
| BACK | 8 | Navigation (retour) / maintien 700ms = arrêt définitif de l'enregistrement |
| PWR | 15 | Maintien 2s = extinction propre sur batterie (sans effet sur USB) |

## `main.cpp` -- fusion moteur réel + écrans 600x450 (14/08)

Construit à partir de deux sources, pas d'une simple copie d'un seul
fichier :

- **Moteur réel + Navigation** : repris du fichier déjà testé sur ce
  hardware (GPS, SD, IMU, WebServer, CourseManager, sessions,
  wheelie/stoppie, hold-to-stop BACK, extinction PWR) -- inchangé.
- **Écrans** : repris de `display_only_241` (déjà pensés pour
  600x450) -- polices agrandies (chrono principal en 110px, `bold_56`
  pour dernier tour/meilleur tour, `bold_38` pour le compteur de
  tours), positions recalculées, **plus le clignotement du chrono
  figé** pendant le temps de pose (fonctionnalité née sur le banc,
  jamais reportée sur le 1.91 avant ce merge).

Trois points de couture identifiés et corrigés pendant le merge (le
banc `display_only_241` simulait certains éléments qui ont une vraie
implémentation côté firmware réel) :

- `handlePush()`/`handleBack()` remis sur la vraie logique -- le banc
  simulait la détection de circuit via un appui PUSH artificiel
  (`simDetected = true`), inutile en réel où le geofencing GPS
  s'occupe de la détection automatiquement.
- Écran WiFi remis sur les vraies SSID/IP (`webServerManager.getSsid()`/
  `getIp()`) au lieu des constantes simulées du banc.
- Écran Connexion gardé dans sa version réelle complète (Hz GPS avec
  seuil rouge <8Hz, état SD, batterie + température CPU) plutôt que la
  version simplifiée du banc, qui aurait fait perdre ces diagnostics.

**Non fait, cosmétique uniquement** : l'écran Connexion n'a pas été
réagencé pour exploiter toute la hauteur 600x450 (reste calé en haut,
espace vide en dessous) -- s'affiche correctement, juste pas optimisé
visuellement. À reprendre si besoin dans une passe dédiée.

## Reste à faire

- Nouveau splash 600x450 (asset graphique, l'ancien 536x240 ne
  correspond pas aux dimensions du nouvel écran).
- Réagencement cosmétique de l'écran Connexion pour la hauteur 600x450.
- Boîtier/enclosure dédié au 2.41 (batterie 2000mAh + TP4056 +
  connecteurs JWPF, cf. le reste du projet PigTeam Chronos).
- Test complet en conditions réelles (piste), le portage n'a pour
  l'instant été validé qu'au banc.
