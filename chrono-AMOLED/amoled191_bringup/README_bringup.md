# Bring-up ESP32-S3-Touch-AMOLED-1.91 -- PlatformIO

Portage direct depuis le depot officiel Waveshare
(github.com/waveshareteam/ESP32-S3-AMOLED-1.91), meme methode que pour
le 1.64. Code driver copie tel quel, seuls les GPIO et deux-trois
constantes changent d'un board a l'autre.

## Comment tester

1. Copie le contenu de `mains/main_XXX.cpp` correspondant au test
   voulu dans `src/main.cpp`.
2. `pio run -t upload` puis moniteur serie (115200 bauds).
3. Passe au test suivant en changeant `src/main.cpp`.

Ordre recommande (meme que sur le 1.64) : IMU -> ADC -> SD -> LVGL/tactile.

## Differences de brochage vs le 1.64 (important, a ne pas confondre)

| Fonction | 1.64 | 1.91 |
|---|---|---|
| I2C SCL/SDA (IMU + tactile, meme bus) | 48 / 47 | 39 / 40 |
| ADC batterie | GPIO4 (ADC1_CH3), x3 | GPIO1 (ADC1_CH0), x2 |
| Ecran QSPI (CS/PCLK/D0-D3/RST) | 9/10/11/12/13/14/21 | 6/47/18/7/48/5/17 |
| SD | SPI (MOSI39/MISO40/CLK41/CS38) | **SDMMC 1-fil** (D0=8/CMD=42/CLK=9) |
| Resolution ecran | 280x456 (portrait) | 536x240 (paysage) |

## Points d'attention specifiques au 1.91

- **SD en mode SDMMC par defaut**, pas SPI comme sur le 1.64 -- controle
  par le flag `VersionControl_V2` en haut de `sd_card_bsp.cpp` (actif
  par defaut dans le repo, correspond a priori a la revision recue
  neuve). Si `SD_card_Init()` echoue silencieusement (pas de carte
  detectee alors qu'elle est bien inseree), essayer de commenter ce
  flag pour retomber sur l'ancien mode SPI (broches differentes, cf.
  le `#else` du fichier).
- **I2C partage entre IMU et tactile** (39/40 sur les deux drivers,
  comme sur le 1.64) -- si tu combines un jour les deux dans un meme
  firmware, il faudra factoriser en un seul `I2C_master_Init()`/
  `Touch_Init()` plutot que d'appeler les deux tels quels (le second
  `i2c_driver_install()` echouerait, bus deja pris).
- **LVGL v8.3.11** (meme version que le 1.64, verifie par diff direct
  des `lv_conf.h` des deux repos -- quasi identiques, juste le music
  demo active en plus ici, sans consequence).
- Driver ecran toujours base sur `esp_lcd_sh8601` (meme famille de
  commandes que le 1.64), tactile toujours **FT3168** (adresse I2C
  0x38) -- seul le nom de fichier change (`touch_bsp.c` au lieu de
  `FT3168.cpp`), le protocole est le meme.
- Pas de fichier `lcd_bsp.c` separe ici : tout le code LVGL/ecran est
  directement dans le `.ino` d'origine (recopie tel quel dans
  `main_lvgl.cpp`).

## Rappel post bring-up

Une fois les 4 tests valides, retirer les deux lignes
`+<../.pio/libdeps/${PIOENV}/lvgl/demos>` et `.../examples` du
`build_src_filter` dans `platformio.ini` avant de commencer le vrai
firmware (sinon compilation inutilement plus longue a chaque fois).
