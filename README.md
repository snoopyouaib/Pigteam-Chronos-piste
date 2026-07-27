# PigTeam Chrono GPS piste

Chronomètre GPS pour moto de piste (détection de circuit, tours,
sessions, WiFi) -- projet PigTeam. Ce dépôt rassemble les **trois
variantes matérielles** du même firmware, chacune adaptée à un montage
différent mais partageant la même logique métier.

## Les trois variantes

| Variante | Board | Écran / entrée | Statut | Pour qui |
|---|---|---|---|---|
| [`chrono-TFT`](chrono-TFT/) | ESP32-S3-Tiny N8R8 | TFT ST7789 320x240 (Adafruit_GFX), encodeur EC11 + bouton BACK | Firmware de référence, complet (GPS, SD, WiFi, WebServerManager) | Firmware principal |
| [`chrono-OLED`](chrono-OLED/) | ESP32-S3-Tiny N8R8 | OLED 1.3" SH1106/SSD1315 + encodeur EC11 (module combiné) | Complet, même brique commune que TFT | Moto du fils -- écran minimal (dernier tour / meilleur tour) pour rester concentré sur le pilotage |
| [`chrono-AMOLED`](chrono-AMOLED/) | ESP32-S3-AMOLED-1.91 | Écran tactile LVGL + encodeur | Complet (GPS/SD/batterie/CourseManager/WebServerManager réels) ; portage validé au banc | Portage en cours depuis `chrono-TFT` |

Chaque dossier a son propre `README.md` avec le câblage, le matériel et
l'historique détaillés. Docs complémentaires par variante :

- `chrono-TFT/README_FS.md`, `chrono-TFT/README_LC76G.md`
- `chrono-OLED/README_FS.md`, `chrono-OLED/README_ALIM_12V.md`, `chrono-OLED/RESUME_SESSION_CHRONO_OLED.md`, `chrono-OLED/Guide_utilisateur_Chrono_GPS_PIGTEAM.docx`
- `chrono-AMOLED/README_AMOLED_bringup.md` (historique calibration écran/tactile)

## Architecture commune

Les trois firmwares partagent la même brique fonctionnelle, seuls
l'affichage et l'entrée utilisateur changent d'une variante à l'autre :

- **`GpsManager`** -- lecture GPS u-blox (UBX binaire, NAV-PVT) en UART direct.
- **`CourseManager`** (vendoré depuis [DovesLapTimer](https://github.com/TheAngryRaven/DovesLapTimer)) -- détection de circuit/tour, géofencing, capture de nouveau circuit, mode proximité.
- **`SdLogStorage`** -- logs GPS détaillés sur carte SD, avec repli sur LittleFS si absente/en panne (SPI sur TFT/OLED, SDMMC sur AMOLED).
- **`WebServerManager`** -- point d'accès WiFi, pages Sessions/Comparer/Circuits/Statut/Firmware (OTA). Porté sur les trois variantes.
- **Format de données communs** -- `circuits.csv` (8 circuits PIGTEAM, chargés via LittleFS) et `sessions.csv` par session.

**Règle importante** : si tu modifies un comportement commun (logs,
WiFi, format `circuits.csv`/`sessions.csv`, `GpsManager`, `SdLogStorage`...)
dans une variante, pense à reporter le changement dans les autres --
elles ne partagent pas de code via une lib commune, seulement par copie.

## Build

Chaque variante est un projet [PlatformIO](https://platformio.org/)
indépendant (`platformio.ini` à la racine de son dossier) :

```
cd chrono-TFT/       # ou chrono-OLED/, chrono-AMOLED/
pio run                 # compiler
pio run -t upload       # flasher par USB (obligatoire au 1er flash, ou après changement de partitions)
pio run -t uploadfs     # envoyer data/circuits.csv sur la LittleFS -- obligatoire avant le 1er boot
```

Voir le README de chaque variante pour les pièges de compilation
spécifiques (dépendances, partitions, particularités du board).

## Structure du dépôt

```
.
├── chrono-TFT/       # firmware de référence -- écran TFT couleur
├── chrono-OLED/      # variante écran minimal (moto du fils)
└── chrono-AMOLED/    # portage en cours -- écran tactile AMOLED
```
