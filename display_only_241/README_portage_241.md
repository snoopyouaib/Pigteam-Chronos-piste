# Portage display_only_191 -> display_only_241 (12/08)

Clone de `display_only_191` (banc de test écran pur, aucune dépendance
GPS/SD/batterie/WiFi réelle -- cf. `README.md`), adapté pour le module
`ESP32-S3-Touch-AMOLED-2.41` (SKU 30589, RM690B0 600x450, FT6336).

## Ce qui a été changé

- **`lib/display/esp_lcd_sh8601.c/h`** renommés en `esp_lcd_rm690b0.c/h`.
  Le driver lui-même n'a pas changé de logique : le framing QSPI
  (opcodes 0x02/0x32/0x03, CASET/RASET/RAMWR) est générique à toute la
  famille SH8601/CO5300/RM67162/RM690B0 -- seule la table d'init
  diffère vraiment d'une puce à l'autre.
- **`lib/display/display_bsp.cpp`** : pins QSPI (CS9/CLK10/D0-D3=11-14/
  RST21, plus aucun recouvrement avec le 1.91), résolution 600x450,
  table d'init RM690B0 **première passe non testée** (CASET/RASET
  recalculés pour 600x450, MADCTL remis à 0x00 par défaut -- le 0xF0
  du 1.91 était spécifique à son montage physique).
- **`lib/touch/touch_bsp.c`** : renommé FT3168 -> FT6336 (même famille
  Focaltech, mêmes registres 0x00/0x02/0x03-0x06), bornes 600x450.
  Adresse I2C 0x38 reprise du 1.91, à confirmer sur le schéma 2.41.
- **`src/main.cpp`** : PUSH_BUTTON et BACK_BUTTON déplacés de GPIO10/14
  (désormais QSPI_CLK/QSPI_D3 de l'écran) vers GPIO18/8 (libres, sans
  fonction de strapping d'après le pinout Waveshare).
- **`platformio.ini`** : env renommé `display_only241`.

## Pas touché (repris tel quel du 1.91)

- `lib/i2c_shared` (structure du module, pas les pins -- cf. mise à
  jour du 12/08 plus bas) -- bus I2C partagé touch/IMU/RTC/expandeur.
- `lib/fonts_teko`, `lib/splash` -- polices/splash, indépendants de
  l'écran physique.
- `lib/gps`, `lib/adc`, `lib/sdlog`, `lib/DovesLapTimer` -- présents
  dans `lib/` (compilés par PlatformIO) mais non appelés depuis
  `main.cpp`, comme sur le 1.91. `GpsManager.cpp` y référence encore
  GPIO12/13 (qui sont maintenant QSPI_D1/D2 côté écran) mais comme rien
  n'appelle `GpsManager::begin()` ici, ça ne crée pas de conflit
  matériel réel -- juste du code mort à garder en tête si un jour ce
  fichier est réactivé sur le 2.41 (il faudra le repasser sur
  GPIO43/44, libres et déjà en UART_TXD/RXD natif sur ce board).

## Mise à jour du 12/08 (retour banc : écran noir, LVGL ok)

Deux bugs trouvés après premier flash réel (écran restait noir malgré
un `displayInit()` qui ne plantait plus après le fix PSRAM) :

1. **Pins I2C jamais mises à jour** -- `i2c_bsp.cpp` était resté sur
   GPIO39/40 (pins du 1.91), copiées telles quelles depuis
   `display_only_191`. Le message `[2/5] Tactile ok` est trompeur : il
   s'affiche inconditionnellement après l'appel, sans vérifier le
   retour I2C, donc il ne prouvait rien. Corrigé vers GPIO47/48
   (TP_SDA/SCL, IMU_SDA/SCL, RTC_SDA/SCL du pinout Waveshare 2.41).
2. **IO-expander TCA9554 absent** -- nouveau sur le 2.41 (n'existe pas
   sur le 1.91) : `AMOLED_EN` (EXIO1) passe par un TCA9554 I2C (adresse
   0x20, même bus partagé) et doit être mis à 1 pour alimenter le
   panneau. Sans ça, le RM690B0 peut rester électriquement éteint même
   si toutes les commandes QSPI sont correctement envoyées -- ce qui
   expliquait l'écran noir malgré une table d'init a priori correcte.
   Ajouté `lib/expander/expander_bsp.cpp/h`, appelé dans `setup()`
   entre `I2C_master_Init()` et `Touch_Init()`.

Toujours à confirmer au banc : adresse TCA9554 (0x20 par défaut,
A0/A1/A2 câblés à la masse sur la plupart des boards Waveshare mais
pas vérifié sur le schéma du 2.41), et si l'écran reste noir après ce
fix, la table d'init RM690B0 elle-même (point 1 de la section
précédente) reste la suspecte suivante.


## Mise à jour du 12/08 (bis) -- toujours noir après le fix EXIO_EN, ajout diagnostic

Le fix TCA9554 n'a pas suffi (écran toujours noir). Plutôt que de
continuer à deviner pin par pin, ajout d'un vrai diagnostic au boot :

- **`I2C_scan()`** (`lib/i2c_shared`) -- scanne 0x08-0x77 sur le bus
  configuré et logge chaque adresse qui ACKe. Appelé juste après
  `I2C_master_Init()`, avant `expanderInit()`/`Touch_Init()`. Ça dira
  sans ambiguïté si GPIO47/48 est le bon bus (rien trouvé = mauvaises
  pins) et à quelle adresse répond effectivement l'expandeur/le
  tactile (peut-être pas 0x20/0x38).
- **`expanderInit()`** et **`Touch_Init()`** impriment maintenant le
  code retour I2C réel de chaque écriture (0 = ACK reçu, non-nul =
  rien à cette adresse) au lieu du `Serial.println` inconditionnel
  précédent qui ne prouvait rien.

Prochaine étape : relancer, coller le nouveau log (surtout la sortie
de `I2C_scan`), et on ajuste pins/adresses sur des faits plutôt que
des hypothèses issues du pinout wiki (qui peut être inexact ou
correspondre à une autre révision de carte).

## Mise à jour du 12/08 (ter) -- I2C confirmé bon, diagnostic écran ajouté

`I2C_scan()` a tout confirmé côté bus partagé : GPIO47/48 corrects,
TCA9554 (0x20), FT6336 (0x38), RTC PCF85063 (0x51), IMU QMI8658 (0x6B)
répondent tous. L'écran noir n'est donc **pas** un problème I2C/EXIO --
il faut chercher côté QSPI (pins, table d'init RM690B0, ou LVGL).

Ajouté dans `display_bsp.cpp`, juste après `esp_lcd_panel_disp_on_off()`
et avant toute init LVGL : un remplissage plein écran brut en trois
couleurs (rouge/vert/bleu, 1.5s chacune) via `esp_lcd_panel_draw_bitmap`
appelé directement, sans passer par LVGL. Ça isole complètement la
chaîne QSPI+init du reste :

- **Si une couleur apparaît** (même fausse/inversée) -> les pins QSPI et
  la table d'init RM690B0 fonctionnent, le problème est plus loin
  (LVGL flush_cb, buffers, byte-swap).
- **Si rien n'apparaît** -> le problème est en amont : pins QSPI
  (CS9/CLK10/D0-D3=11-14/RST21) ou table d'init RM690B0 elle-même.

Bloc marqué "DIAGNOSTIC TEMPORAIRE" dans le code, à retirer une fois
l'écran confirmé fonctionnel.

## Mise à jour du 12/08 (quater) -- erreur SPI identifiée, diagnostic corrigé

Le premier diagnostic (couleur plein écran en un seul appel) a échoué
avec `spi transmit (queue) color failed` x3 : une transaction DMA de
540000 octets d'un coup (600x450x2) est trop grosse. Signe encourageant
au passage : **aucune** erreur SPI n'apparaît après `[3/5]`, alors que
LVGL flushe déjà en continu par blocs de ~134 Ko (`EXAMPLE_LVGL_BUF_HEIGHT`
lignes) -- ça suggère que les pins QSPI et l'init du panneau fonctionnent,
et que le problème n'était que la taille de la transaction du diagnostic
lui-même, pas la chaîne QSPI en profondeur.

Diagnostic corrigé : même remplissage rouge/vert/bleu mais découpé en
bandes de `EXAMPLE_LVGL_BUF_HEIGHT` lignes (même taille que les flush
LVGL, donc directement comparable), avec log d'erreur par bande si ça
échoue encore.

Écran toujours rapporté noir malgré `[3/5] Ecran + LVGL ok` sans
erreur -- si le diagnostic par bandes ne montre toujours rien, le
prochain suspect est soit la table d'init RM690B0 (le contrôleur
accepte les commandes sans erreur mais n'affiche rien de valide),
soit `AMOLED_EN`/le timing entre l'activation EXIO et le reset GPIO21.

## Mise à jour du 12/08 (cinq) -- crash identifié et corrigé (bug de séquencement du diagnostic)

Le diagnostic par bandes (v2, message précédent) a fait planter le
firmware (`Guru Meditation Error: StoreProhibited`, `EXCVADDR: 0x00000010`)
avant même d'atteindre `[3/5]`. Cause : le callback `on_color_trans_done`
(câblé sur `example_notify_lvgl_flush_ready` → `lv_disp_flush_ready()`
dès la création de l'IO du panneau, donc *avant* que le diagnostic
tourne) se déclenche à la fin de chaque transaction QSPI. Le
diagnostic était placé avant `lv_disp_drv_register()`, donc
`disp_drv.draw_buf` était encore `NULL` -- premier `draw_bitmap` réussi
qui se termine (contrairement au v1, dont les transactions trop
grosses échouaient avant d'atteindre ce point) déclenche le callback,
qui déréférence ce pointeur NULL -> crash. Cohérent avec `EXCVADDR`
proche de zéro (offset du champ `flushing` dans la struct).

**Fix** : le bloc diagnostic est déplacé après `lv_disp_drv_register()`
(où `disp_drv.draw_buf` est déjà valide) et avant `xTaskCreate()` de la
tâche LVGL (pour éviter toute concurrence sur le bus SPI). Petit délai
(5ms) ajouté entre bandes pour laisser respirer la queue de
transactions.

## Mise à jour du 12/08 (six) -- sources officielles Waveshare obtenues, refonte complète

L'utilisateur a fourni les vraies sources de la démo LVGL officielle
Waveshare pour le 2.41 (`09_LVGL_Test.ino` + `esp_lcd_sh8601.c/h` +
`esp_lcd_touch_ft5x06.c/h`). Plusieurs hypothèses précédentes étaient
fausses :

1. **Pas d'IO-expander pour l'écran.** Aucune trace de TCA9554/EXIO
   dans le code officiel pour alimenter l'AMOLED -- contrairement à
   d'autres boards Waveshare (1.75/1.8/1.43) suivis par analogie à
   tort. `expanderInit()` reste dans le code (inoffensif, le TCA9554
   existe bel et bien sur le bus d'après `I2C_scan()`) mais n'est
   probablement pas la clé du problème d'écran noir.
2. **Panneau natif 450×600 portrait**, pas 600×450. Le mode paysage
   600×450 voulu s'obtient via `MADCTL=0x30` (bit MV), et la table
   d'init officielle adresse `CASET` sur **16..465** (pas 0..599 comme
   inventé précédemment -- offset matériel de 16px) et `RASET` sur
   0..599 pleine échelle.
3. **Séquence de déverrouillage vendor** (`0xFE` page-select vers la
   page 0x20 pour 2 registres, puis retour page 0x00) avant les
   commandes DCS normales -- totalement absente de nos tentatives.
4. **`flush_cb` officiel** : ajoute +16 sur Y (compense l'offset CASET
   du point 2) et ne fait **aucun swap d'octets** manuel (celui hérité
   du 1.91 était spécifique à ce montage-là, pas généralisable).
5. **Buffers LVGL en `V_RES/10`** (pas `/4`) -- assez petits pour
   `MALLOC_CAP_DMA` en SRAM interne, la complication PSRAM n'était pas
   nécessaire.

## Mise à jour du 12/08 (huit) -- le hardware est confirmé bon (démo officielle testée = OK)

L'utilisateur a testé le `.ino` officiel Waveshare tel quel sur ce
même hardware : **l'écran fonctionne**. Ça confirme que la table
d'init, les pins QSPI, et le panneau physique sont tous bons -- le
bug restant est forcément dans notre portage (PlatformIO vs Arduino
IDE, version LVGL 8.3.11 vs 8.4.0 attendue par le wiki, ou la logique
applicative des écrans/splash héritée telle quelle du 1.91).

Réintroduction du diagnostic couleur brut (retiré précédemment), cette
fois avec le bon offset +16 sur Y pour rester cohérent avec la vraie
table CASET. But : déterminer si le problème est encore dans la
chaîne bas niveau (peu probable maintenant, mais on élimine le doute)
ou franchement dans la logique applicative (écrans 9x hérités du 1.91,
splash à fond noir, `LV_MEM_SIZE=48Ko` peut-être insuffisant pour tous
ces écrans -- `LV_USE_ASSERT_MALLOC=1` est actif dans `lv_conf.h` donc
ça planterait proprement si c'était le cas, mais à garder en tête).

Si le diagnostic couleur montre enfin quelque chose : le driver est
bon, il faut chasser côté écrans applicatifs (position/couleurs/fond
noir du splash). Si toujours rien : comparer plus finement notre
`lv_conf.h` (hérité du 1.91) avec celui attendu pour LVGL 8.4.0, et/ou
tenter de forcer `lvgl@8.4.0` dans `platformio.ini` au lieu de
`8.3.11`.

## Mise à jour du 12/08 (neuf) -- version ESP-IDF suspectée, alignée sur le wiki

Diagnostic couleur toujours noir malgré une chaîne bas niveau
identique byte pour byte au driver officiel (`esp_lcd_rm690b0.c/h`
diffé de `esp_lcd_sh8601.c/h` officiel uniquement par le renommage,
vérifié par `diff`). Le code est donc bon -- reste l'environnement de
build.

Trouvé : `platformio.ini` pointait vers la release pioarduino
`53.03.13`, soit **Arduino-ESP32 core 3.1.3 / ESP-IDF 5.3.2** --
héritée telle quelle du projet `chrono-AMOLED` (1.91) sans être
revalidée pour le 2.41. Le wiki Waveshare exige explicitement le
**core 3.0.7** pour ce board, qui correspond à la release `51.03.07`
(**ESP-IDF 5.1.4**). Entre IDF 5.1 et 5.3, `esp_lcd`/`spi_master` ont
eu de vraies évolutions (gestion QSPI, validation des transactions) --
candidat sérieux pour expliquer un driver qui s'exécute sans la
moindre erreur mais ne produit rien de visible.

`platformio.ini` repointé vers `51.03.07`. LVGL également aligné sur
`8.4.0` (celui exigé par le wiki, on avait `8.3.11` hérité du 1.91) --
les deux changements en même temps puisqu'ils viennent de la même
logique "coller exactement aux versions que Waveshare a validées".

## Confirmation du 12/08 (sept) -- tous les exemples officiels obtenus

L'utilisateur a fourni l'archive complète des exemples Waveshare
(01 à 10 + Arduino_Playablity). Confirmations concordantes :

- `i2c_bsp.cpp` de `07_EX_GPIO` : GPIO47/48, identique à ce qu'on a.
- `gpio_bsp.cpp` (le vrai driver TCA9554 officiel, adresse 0x20
  confirmée) : ne configure QUE EXIO5 en sortie (pour le test
  GPIO croisé EXIO5/EXIO6 de la démo `07_EX_GPIO`) -- rien sur EXIO1.
  Et surtout, **`09_LVGL_Test.ino` n'appelle jamais `esp32_gpio_init()`
  ni aucune fonction TCA9554** : l'écran s'allume sans toucher à
  l'expandeur. Confirme définitivement que `expanderInit()`/EXIO_EN
  était une fausse piste (laissé dans le code, inoffensif, mais plus
  suspecté pour l'écran noir).
- Le seul GPIO d'alimentation lié à la batterie (`BAT_ON`, GPIO16
  direct, pas EXIO) sert au test Li-ion (`08_Li-ion_Test`), sans
  rapport avec l'écran.

## À valider au banc

RÉSOLU (12/08, dix) -- voir section suivante.

## RÉSOLU (12/08, dix) -- carte V2, pas V1 ! Cause trouvée et corrigée

Après une journée entière de diagnostic (PlatformIO, Arduino IDE,
environnement propre, deux cartes différentes testées, support
Waveshare contacté), la cause réelle : **notre board est une révision
"V2"** (étiquette "V2" à côté du marquage "2.41" sur les deux unités),
une révision matérielle absente du wiki public et du repo GitHub
"principal" qu'on a utilisé toute la journée -- confirmée via le repo
dédié que l'utilisateur a trouvé :
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2

**Différence matérielle clé** : sur le V1, `AMOLED_RST` est un vrai
GPIO direct (GPIO21). Sur le V2, GPIO21 n'est **plus** câblé au reset
de l'écran -- le reset de l'écran (et du tactile) passe désormais par
le TCA9554 (EXIO0 = reset écran, EXIO1 = reset tactile), avec un vrai
pulse (haut → bas 20ms → haut, puis 120ms d'attente) à effectuer avant
l'init du panneau. C'est exactement pour ça que tout ce qui a été
testé aujourd'hui (notre code V1, la démo V1 officielle recompilée,
le `.bin` du wiki V1, le `10_FactoryProgram` V1, même flashé sur un
environnement Arduino IDE totalement propre) laissait l'écran noir :
le reset matériel de l'écran ne partait jamais, quel que soit le
firmware -- tous supposaient GPIO21, qui ne va nulle part sur cette
carte.

**Confirmé fonctionnel** : le firmware précompilé du repo V2
(`03_Firmware/ESP32-S3-Touch-AMOLED-2.41-v2.bin`) flashé via le Flash
Download Tool -- écran opérationnel.

**Corrections apportées à `display_only_241`** :
- `lib/expander/expander_bsp.h/.cpp` réécrit : `expanderInit()`
  configure maintenant EXIO0/EXIO1/EXIO5 en sortie (plus EXIO1 seul
  comme avant, fausse piste "AMOLED_EN"), `expanderResetOled()` et
  `expanderResetTouch()` implémentent le vrai pulse de reset par
  broche, repris à l'identique du repo V2 officiel.
- `lib/display/display_bsp.cpp` : `EXAMPLE_PIN_NUM_LCD_RST` passé à
  `-1` (plus de GPIO direct), table CASET (`0x2A`)/RASET (`0x2B`)
  alignée sur les valeurs exactes du V2 (légèrement différentes du
  V1 : `0x00,0x10,0x00,0xD1` / `0x00,0x00,0x00,0x57`).
- `src/main.cpp` : séquence réordonnée pour matcher le V2 --
  `expanderInit()` → `expanderResetOled()` → `displayInit()` →
  `expanderResetTouch()` → `Touch_Init()`.

**Reste à valider au banc** : ces changements n'ont pas encore été
retestés avec ce firmware PlatformIO corrigé (seul le `.bin`
précompilé V2 a été confirmé). Prochain flash à faire pour vérifier
que le portage `display_only_241` fonctionne enfin.

## RÉSOLU (12/08, dix bis) -- ça marche sur PlatformIO !

Confirmé par l'utilisateur : `display_only_241` corrigé (reset via
TCA9554) boote jusqu'à `Pret.` et affiche bien l'application PigTeam
(l'écran Statut/chrono de test réel). Objectif du portage atteint.

Deux ajustements cosmétiques mineurs identifiés au premier essai :

1. **Bloc diagnostic couleur retiré** -- n'était plus nécessaire une
   fois la cause racine trouvée, retiré de `display_bsp.cpp`.
2. **Couleurs inversées (rouge/bleu)** -- `rgb_ele_order` passé de
   `LCD_RGB_ELEMENT_ORDER_RGB` à `LCD_RGB_ELEMENT_ORDER_BGR` dans
   `display_bsp.cpp`. Réglage global du panneau, corrige aussi les
   couleurs du splash au passage.
3. **Splash 536×240 mal dimensionné** sur le nouveau canevas 600×450
   -- attendu, image héritée du 1.91 telle quelle (déjà documenté
   plus haut dans ce fichier). Pas un bug, juste un asset à
   redimensionner/recentrer plus tard, sans urgence pour le bring-up.



Changement de plateforme (`51.03.07`) + LVGL `8.4.0` pas encore testé
sur le vrai 2.41 -- prochain flash à venir. Points restants si l'écran
reste noir malgré tout :

1. **Séquence exacte des registres 0xFE/0x26/0x24/0xC2** -- reprise
   telle quelle de la démo officielle, sens exact non documenté
   publiquement (registres vendor non génériques), mais c'est la
   référence connue-fonctionnelle donc le point de départ le plus
   fiable qu'on ait eu jusqu'ici.
2. **Boutons GPIO18/8** -- juste vérifiés "libres" sur le pinout,
   jamais câblés ni testés (sans rapport avec l'écran).
3. **Tactile** -- transformation mirror_y+swap_xy reprise de la démo
   officielle mais jamais testée avec un vrai doigt sur ce firmware.


## À reporter sur chrono-AMOLED (1.91) -- amélioration née sur le banc 2.41

- **Clignotement du chrono pendant le temps figé** (`lapFreezeS`, réglage
  "Pause chrono") -- 500ms allumé / 250ms éteint sur `lblBig`, pour
  distinguer visuellement un temps de tour figé d'un chrono qui tourne
  activement. Implémenté dans `updateStatusScreen()`, branche REC actif
  (`if (millis() < lapFreezeUntilMs)`).

Rafraîchissement de l'écran Statut testé un temps à 80ms (au lieu de
250ms), finalement pas nécessaire pour le réglage 500/250 retenu (750ms
= multiple exact de 250ms, motif fidèle même à ce taux) -- revenu à
250ms. À revoir seulement si un futur réglage de clignotement tombe sur
un cycle qui n'est pas multiple de 250ms.
