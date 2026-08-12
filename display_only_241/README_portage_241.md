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

## À valider au banc (mis à jour, restant après les fixes du 12/08)




1. **Table d'init RM690B0** -- la plus grosse inconnue restante. Le
   driver tourne, l'écran est maintenant alimenté (EXIO_EN), mais peut
   rester noir/afficher n'importe quoi selon ce que le contrôleur
   attend vraiment à l'init.
2. **Orientation (MADCTL 0x36)** -- 0x00 est un point de départ neutre,
   probablement à ajuster (mirror/swap) une fois l'image visible.
3. **Adresse I2C FT6336** -- 0x38 hérité du 1.91, à confirmer.
4. **Boutons GPIO18/8** -- juste vérifiés "libres" sur le pinout,
   jamais câblés ni testés.
