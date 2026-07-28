# Bring-up écrans AMOLED Waveshare -- PigTeam

Validation matérielle et portage Arduino IDE -> PlatformIO de deux
écrans tout-en-un ESP32-S3 + AMOLED reçus pour évaluation en vue d'un
remplacement éventuel du trio actuel (ESP32-S3-Tiny + TFT ST7789 +
GPS séparé) du projet chrono GPS moto piste.

Boards testées :
- **ESP32-S3-Touch-AMOLED-1.64** (280x456, boîtier compact CNC)
- **ESP32-S3-Touch-AMOLED-1.91** (536x240 paysage, format Pico-header,
  option tactile)

## Sommaire

- [Matériel](#matériel)
- [Méthode de test](#méthode-de-test)
- [Résultats -- 1.64](#résultats----164)
- [Résultats -- 1.91](#résultats----191)
- [Différences de brochage entre les deux boards](#différences-de-brochage-entre-les-deux-boards)
- [Pièges rencontrés et solutions](#pièges-rencontrés-et-solutions)
- [Structure des projets PlatformIO](#structure-des-projets-platformio)
- [Prochaines étapes](#prochaines-étapes)

---

## Matériel

### ESP32-S3-Touch-AMOLED-1.64
- ESP32-S3R8 (Wi-Fi/BLE), 16MB Flash, 8MB PSRAM octale
- Écran AMOLED 280x456, driver **CO5300/SH8601-compatible**, QSPI
- Tactile capacitif **FT3168** (I2C)
- IMU 6 axes **QMI8658** (I2C, même bus que le tactile)
- Lecteur carte TF intégré (SPI dédié)
- Charge Li-Po intégrée (AXP2101 ou équivalent), mesure ADC batterie
- Boîtier compact CNC, une douzaine de GPIO exposés + TX/RX dédiés

### ESP32-S3-Touch-AMOLED-1.91 (option tactile)
- ESP32-S3, PSRAM octale
- Écran AMOLED 536x240 (paysage), driver **RM67162**, piloté via le
  même driver logiciel `esp_lcd_sh8601` (compatible registre-à-registre)
- Tactile capacitif **FT3168** (I2C, même puce que le 1.64)
- IMU 6 axes **QMI8658**
- Lecteur carte TF (mode **SDMMC 1-fil** par défaut, pas SPI)
- Format **Pico-header** (2x20 broches), ~27 GPIO exposés -- bien plus
  de marge que le 1.64 pour un futur firmware complet (GPS + encodeur +
  bouton + SD + batterie sur une seule carte)

---

## Méthode de test

Même démarche pour les deux boards, dans le même ordre à chaque fois :

1. **Sanity check global** : flash du firmware de test précompilé
   fourni par Waveshare (`Factory Program` sur le 1.64) via le Flash
   Download Tool Espressif -- valide écran, tactile, WiFi, BLE,
   luminosité en un seul flash, avant tout code custom.
2. **IMU (QMI8658)** -- lecture brute accéléro/gyro/température.
3. **ADC (tension batterie/USB)** -- lecture tension système.
4. **Carte SD** -- montage + lecture des infos carte.
5. **LVGL + écran + tactile** -- rendu du dashboard `lv_demo_widgets`
   avec interaction tactile.

Chaque étape testée d'abord sous **Arduino IDE** (démos officielles
Waveshare telles quelles, zip `..-Demo.zip` du wiki produit, ou dépôt
GitHub `waveshareteam/ESP32-S3-AMOLED-1.91` pour le second board), puis
**portée sous PlatformIO** (VSCode) pour rester cohérent avec le reste
des projets PigTeam et permettre la réutilisation future de modules
partagés (`GpsManager`, `SdLogStorage`...).

---

## Résultats -- 1.64

| Test | Arduino IDE | PlatformIO |
|---|---|---|
| Sanity check (Factory firmware) | OK | -- (pas de portage nécessaire) |
| IMU QMI8658 | OK | OK |
| ADC / tension batterie | OK (~4.73V sous USB sans batterie) | OK (~4.71-4.72V, écart négligeable) |
| Carte SD (32GB SDHC) | OK (29.54G pratique, 20MHz) | OK (identique) |
| LVGL / écran / tactile | OK | OK |
| GPS réel (Quectel LC76G, NMEA) | OK (fix 3D, 10 satellites) | OK (headers soudés, test complet) |

Brochage GPS confirmé : **TXD = GPIO43, RXD = GPIO44** (UART0, libre
car le monitoring série passe par l'USB natif) -- module `GpsManager`
porté depuis le projet TFT (même interface `GpsData`/`liveData`/
`initGps()`/`pollGps()`, seuls les pins changent). Cold start observé
lors d'un second essai après débranchement (quelques minutes sans fix,
comportement normal le temps de retélécharger les éphémérides -- pas un
bug de portage).

## Résultats -- 1.91

| Test | Arduino IDE | PlatformIO |
|---|---|---|
| IMU QMI8658 | -- (porté direct) | OK (repos ~9.8-9.9g, mieux calé que le 1.64) |
| ADC / tension batterie | -- | OK (~4.19V sous USB, GPIO1/ADC1_CH0, x2) |
| Carte SD (mode SDMMC) | -- | OK (32GB SDHC, 29.54G, `bus_width=1` confirmé) |
| LVGL / écran / tactile | -- | OK (après résolution du souci de fontes, cf. pièges) |
| GPS réel (Quectel LC76G, NMEA) | -- | OK (fix 3D, jusqu'à 13 satellites -- meilleur que le 1.64) |
| Encodeur EC11 + bouton BACK | -- | OK (après ajout d'un pull-up explicite, cf. pièges) |

Brochage encodeur/bouton retenu pour ce board : **CLK=GPIO2, DT=GPIO3,
PUSH=GPIO10, BACK=GPIO14** -- lib `AiEsp32RotaryEncoder@^1.4`, même
pattern d'init que le firmware TFT existant.

Brochage GPS confirmé par la fiche de brochage officielle Waveshare :
broche 1 = GPIO43 = UART0 TX, broche 2 = GPIO44 = UART0 RX --
**exactement les mêmes GPIO que le 1.64** (même silicium ESP32-S3,
UART0 câblé pareil en interne) -- le module `GpsManager` du 1.64 a été
réutilisé tel quel, sans aucune modification de pins.

---

## Différences de brochage entre les deux boards

| Fonction | 1.64 | 1.91 |
|---|---|---|
| I2C SCL / SDA (IMU + tactile, bus partagé) | GPIO48 / GPIO47 | GPIO39 / GPIO40 |
| ADC batterie -- broche / canal / multiplicateur | GPIO4 (ADC1_CH3), x3 | GPIO1 (ADC1_CH0), x2 |
| Écran QSPI -- CS / PCLK / D0 / D1 / D2 / D3 / RST | 9/10/11/12/13/14/21 | 6/47/18/7/48/5/17 |
| Carte SD | SPI dédié (MOSI39/MISO40/CLK41/CS38) | **SDMMC 1-fil** (D0=8/CMD=42/CLK=9) |
| UART GPS | TXD=43, RXD=44 (UART0, libre) | **TXD=43, RXD=44 (identique)** |
| Résolution écran | 280x456 (portrait) | 536x240 (paysage) |

---

## Pièges rencontrés et solutions

### 1. `lv_demo_widgets` : "undefined reference" (les deux boards)
PlatformIO ne compile par défaut que le dossier `src/` d'une
bibliothèque -- les dossiers `demos/` et `examples/` de LVGL en sont
exclus, même avec `LV_USE_DEMO_WIDGETS 1` dans `lv_conf.h`. Fix
documenté officiellement par LVGL :

```ini
build_src_filter =
    +<*>
    +<../.pio/libdeps/${PIOENV}/lvgl/demos>
    +<../.pio/libdeps/${PIOENV}/lvgl/examples>
```

A retirer une fois le bring-up terminé (compilation inutilement plus
longue sinon, sur le vrai firmware).

### 2. Polices Montserrat manquantes à l'édition de liens (1.91 uniquement)
Même après un `lv_conf.h` correct (`LV_FONT_MONTSERRAT_12`/`_16` à 1)
et un rebuild complet (`.pio` supprimé), les symboles
`lv_font_montserrat_12`/`_16` restaient introuvables au link. Cause
précise non totalement élucidée (probable souci de résolution de
`lv_conf.h` propre à cet environnement, non reproduit sur le 1.64 avec
une config quasi identique). Fix définitif : forcer les macros en
ligne de commande, qui prend effet avant tout header :

```ini
build_flags =
    ...
    -D LV_FONT_MONTSERRAT_12=1
    -D LV_FONT_MONTSERRAT_16=1
```

### 3. Deux blocs `build_flags` dans le même environnement
`InvalidProjectConfError` si `build_flags` (ou toute autre clé)
apparaît deux fois dans un même `[env:xxx]` -- toujours fusionner en un
seul bloc.

### 4. SD en mode SDMMC par défaut sur le 1.91 (pas SPI comme le 1.64)
Contrôlé par le flag `VersionControl_V2` en tête de `sd_card_bsp.cpp`,
actif par défaut dans le dépôt (correspond a priori à la révision
matérielle reçue). Si le montage SD échoue silencieusement, commenter
ce flag bascule vers l'ancien câblage SPI (broches différentes, cf.
le `#else` du fichier).

### 5. Bus I2C partagé entre IMU et tactile (les deux boards)
`I2C_master_Init()` (IMU) et `Touch_Init()` (tactile) appellent chacun
`i2c_driver_install()` sur le même port I2C. Fonctionne tant qu'un seul
des deux tourne à la fois (comme dans ces tests isolés) -- mais un futur
firmware combinant les deux devra factoriser en une seule
initialisation I2C partagée, sous peine d'échec du second
`i2c_driver_install()` (bus déjà pris).

### 6. Version LVGL à épingler en dur
`lvgl/lvgl@8.3.11` (pas de caret `^`) -- LVGL v9 a changé l'API du
driver d'affichage (`lv_disp_drv_t`, etc.) utilisée par le code
Waveshare. Un caret laisserait un jour PlatformIO installer une v9 et
tout casserait à la compilation.

### 7. Board PlatformIO générique
Aucune définition de board PlatformIO officielle pour ces modules
Waveshare -- utiliser `esp32-s3-devkitc-1` en board générique, avec
`board_build.arduino.memory_type = qio_opi` (Flash quad + PSRAM octale)
et une taille de flash/partitions adaptée (16MB ici).

### 8. Alertes de compilation nombreuses mais bénignes
Le message `#pragma message: Possible failure to include lv_conf.h...`
apparaît systématiquement dans les logs de compilation LVGL, sur les
deux boards, que la compilation réussisse ou non -- c'est un
avertissement générique de LVGL, pas un indicateur fiable de panne
réelle. Ne pas s'y fier pour diagnostiquer un problème (piège dans
lequel on est tombé en pensant à tort qu'il expliquait le souci de
polices du point 2).

### 9. Retirer `lvgl` de `lib_deps` sans retirer `LV_CONF_INCLUDE_SIMPLE`
Après le test GPS/encodeur (qui n'utilisent pas LVGL), `lvgl` a été
retiré de `lib_deps` mais le flag `-D LV_CONF_INCLUDE_SIMPLE` et le
`build_src_filter` dédié aux demos LVGL sont restés dans
`build_flags`/`build_src_filter` sans que la lib soit présente -- ou
l'inverse (lib gardée sans le flag) -- dans les deux cas ça casse la
compilation (`lv_conf.h: No such file or directory`, LVGL ne sachant
plus où chercher son fichier de config). Les trois éléments
(`lib_deps` LVGL + `LV_CONF_INCLUDE_SIMPLE` + `build_src_filter`
demos/examples) vont toujours ensemble -- retirer l'un sans les deux
autres casse systématiquement la compilation.

### 10. Encodeur EC11 : pas de pull-up sur CLK/DT malgré la lib
Sur le montage 1.91 (GPIO2/GPIO3), la lib `AiEsp32RotaryEncoder` ne
semble pas activer correctement le pull-up interne sur les broches
CLK/DT -- lecture bloquée à `0/0` même broches débranchées (donc
purement flottantes). Fix : forcer explicitement
`pinMode(ENCODER_CLK, INPUT_PULLUP)` /
`pinMode(ENCODER_DT, INPUT_PULLUP)` avant `rotaryEncoder.begin()`.
Diagnostic qui a permis de le confirmer : lire les deux broches en brut
(`digitalRead()`) dans `loop()` en parallèle de la lib, et tester
d'abord broches déconnectées (élimine la piste câblage physique avant
d'aller chercher plus loin).

### 11. Deux noms différents pour la même fonction I2C selon le fichier Waveshare
En intégrant IMU + tactile sur un bus I2C partagé (cf. piège #5), le
`qmi8658c.cpp` du 1.91 appelle `I2C_write_buff` (nom corrigé de la
coquille), alors que le `touch_bsp.c` du même dépôt Waveshare définit
sa propre copie de la fonction sous le nom `I2C_writr_buff` (avec la
coquille, comme sur le 1.64) -- les deux fichiers n'étaient jamais
prévus pour être compilés ensemble à l'origine, d'où l'incohérence.
En factorisant l'I2C en un module partagé, il faut choisir **un seul**
nom et s'assurer que le driver IMU (qu'on ne veut pas modifier) et le
driver tactile (réécrit pour l'occasion) appellent bien le même.

### 12. Linkage C/C++ : `extern "C"` manquant sur un header partagé entre .c et .cpp
Le module I2C partagé (`i2c_bsp.h`/`.cpp`) est utilisé à la fois par un
fichier **C pur** (`touch_bsp.c`, compilé avec `gcc`) et des fichiers
**C++** (`qmi8658c.cpp`, `main.cpp`, compilés avec `g++`). Sans garde
`extern "C" { ... }` dans le header, le compilateur C++ mangle les noms
de symboles à sa façon -- le fichier C++ (`i2c_bsp.cpp`) qui implémente
`I2C_write_buff`/`I2C_read_buff` compile sans erreur, mais le fichier C
(`touch_bsp.c`) qui les appelle échoue à l'édition de liens
("undefined reference"), alors que tout compile individuellement sans
souci. Symptôme révélateur : erreur de LIEN (pas de compilation), et
uniquement sur les fichiers `.c`, jamais sur les `.cpp` qui utilisent le
même header. Fix : entourer les déclarations du header partagé d'un
`#ifdef __cplusplus extern "C" { ... } #endif`.

---

### 13. Couleurs RGB565 inversées, indépendamment de `LV_COLOR_16_SWAP`
En construisant l'écran Statut du 1.91 (premier usage de vraies teintes
-- gris/vert/orange/rouge -- après plusieurs tests en blanc/noir
uniquement), les couleurs sortaient fausses (gris affiché en rose vif,
etc.), alors que le blanc/noir avait toujours été correct -- normal,
ces deux-là restent inchangés par un swap d'octets, contrairement à une
vraie teinte. Calcul de confirmation : gris LVGL `0x9E9E9E` converti en
RGB565 puis avec ses 2 octets inversés donne `(240,112,224)` -- un rose
vif, exactement ce qui était observe.

Pistes classiques essayées sans effet : `LV_COLOR_16_SWAP` (LVGL) à 0
ou 1 -- aucune différence visible, même après reconstruction complète
confirmée (`.pio` supprimé, `#warning` de vérification affiché) ; bit
BGR du MADCTL (`0x36`) testé à `0xF8` au lieu de `0xF0` -- aucun effet
non plus. Le swap semble se produire plus bas que LVGL, dans la couche
de transport ESP-IDF (`esp_lcd_panel_io_spi`), independamment de ces
deux reglages.

**Fix retenu** : swap manuel des octets de chaque pixel directement
dans `example_lvgl_flush_cb` (`display_bsp.cpp`), juste avant l'appel à
`esp_lcd_panel_draw_bitmap()` -- le seul point qu'on contrôle
totalement avant l'envoi réel à l'écran :
```cpp
uint16_t* buf16 = (uint16_t*)color_map;
uint32_t pixelCount = lv_area_get_width(area) * lv_area_get_height(area);
for (uint32_t i = 0; i < pixelCount; i++) {
  buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
}
```
Avec `LV_COLOR_16_SWAP` repassé à `0` dans `lv_conf.h` (pour éviter un
double swap LVGL + le nôtre).

**Diagnostic qui a permis d'isoler le problème** : tester des
rectangles de couleur pleine (sans texte, donc sans anti-crénelage) --
si un simple aplat de couleur ressort déjà faux, le bug est bas niveau
(couleur/transport), pas dans le rendu des polices. Comparaison utile
au passage : les exemples tactiles Waveshare `RGBpalette`/
`SliderControl` (`03_Playablity`) utilisent un driver dédié
(`rm67162.cpp`) qui envoie les octets bruts sans swap -- cette lecture
a orienté le diagnostic sans toutefois expliquer pourquoi
`LV_COLOR_16_SWAP` restait sans effet côté LVGL.

**Résidu mineur accepté tel quel** : après ce fix, orange tire
légèrement au jaune et rouge légèrement à l'orange (décalage de teinte
cohérent, pas une inversion de canal) -- probable caractéristique de la
dalle AMOLED elle-même sur les teintes saturées, pas creusé plus loin
faute d'impact réel sur la lisibilité.

### 16. Les labels LVGL ne sont PAS cliquables par défaut (contrairement aux `lv_obj_create()`)
En convertissant Circuit/Session/Réglages au tap direct sur les lignes
de liste, aucun tap n'était reconnu malgré un callback `LV_EVENT_CLICKED`
correctement enregistré -- contrairement au piège #15 (un `lv_obj_create()`
de base EST cliquable par défaut), un `lv_label_create()` ne l'est PAS.
Fix : `lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE)` explicite sur
chaque ligne de liste voulue tappable. Les deux pièges (#15 et #16) sont
symétriques et faciles à confondre -- retenir que le comportement par
défaut dépend du **type de widget**, pas d'une regle unique pour tous
les objets LVGL.

### 17. Différencier un tap d'un glissement dans une liste scrollable
Une fois les listes rendues tactiles (tap pour sélectionner + glissement
vertical pour défiler sur le même conteneur), un glissement pouvait
parfois se faire interpréter comme un tap de sélection accidentel.
Cause : `LV_INDEV_DEF_SCROLL_LIMIT` (tolérance de mouvement en dessous
de laquelle un appui reste un tap, 10px par défaut dans LVGL) trop
permissive pour ce cas d'usage. Fix : reduit a 6px dans `lv_conf.h`.
Compromis a garder en tete : trop bas, les taps eux-memes peuvent
devenir capricieux (mouvement naturel du doigt pendant un tap franc mal
tolere) -- valeur a retoucher a l'usage si l'un ou l'autre geste
commence a mal se comporter.

### 18. Splash screen : inversion rouge/bleu lors de la conversion ARGB8888 -> RGB565
Le fichier splash fourni (export Piskel, meme format brut que
`splash_pigteam.h` du projet TFT) est un tableau de pixels ARGB8888 --
incompatible tel quel avec LVGL, qui attend un `lv_img_dsc_t` avec des
donnees dans la profondeur couleur configuree (RGB565 ici,
`LV_COLOR_DEPTH=16`). Conversion faite via un script Python (extraction
des composantes A/R/G/B depuis chaque `uint32_t`, repackees en RGB565).
Premier essai avec l'hypothese de format `0xAARRGGBB` -- couleurs
fausses a l'affichage (rouge et bleu inverses). Fix : format source en
realite `0xAABBGGRR` (R et B inverses par rapport a l'hypothese
initiale) -- aucun moyen de le determiner a l'avance sans un premier
essai visuel, piege difficile a eviter autrement que par test empirique
direct.

### 14. Glissement tactile qui ne suit pas -- registre FT3168 en "flash"
En testant le tactile (`getTouch()`) pour du glissement continu (pas
juste des taps), le marqueur affiché ne suivait pas le doigt -- un
sondage direct en boucle serrée (`loop()`) suivait bien le mouvement en
continu, mais via le pipeline LVGL (lecture toutes les ~30ms), un seul
échantillon passait puis plus rien tant que le doigt restait immobile
et pose. Plusieurs pistes classiques testées sans effet : vitesse I2C
(300kHz -> 100kHz, aucun changement).

**Cause identifiée** : le registre "point touché" (`0x02`) du FT3168
semble se comporter comme un flash "nouvel echantillon disponible"
plutot qu'un etat "actuellement touché" permanent -- probablement lié
au cycle de scan interne propre a ce controleur (fenetre courte pendant
laquelle la donnee est valide, qui peut ne pas coincider avec le
rythme de lecture fixe de LVGL).

**Fix** : fenetre de grace de 120ms dans `getTouch()`
(`lib/touch/touch_bsp.c`) -- si le registre repond "rien" mais qu'un
echantillon valide date de moins de 120ms, on continue de renvoyer
"touché" avec la derniere position connue, et on ne declare un vrai
relachement qu'apres expiration de cette fenetre sans nouvel
echantillon.

**Diagnostic qui a permis de le confirmer** : logger `getTouch()`
directement dans le callback de lecture LVGL (`example_lvgl_touch_cb`,
`display_bsp.cpp`) plutot que de se fier aux evenements LVGL de haut
niveau -- a revele que le driver, une fois la fenetre de grace
ajoutee, renvoyait bien un signal continu, ce qui a permis d'isoler
le probleme suivant (piege #15) comme etant cote LVGL et non plus cote
driver.

### 15. Un objet LVGL visuel (marqueur de debug) qui vole le focus tactile
Meme apres le fix du piege #14, le glissement restait interrompu apres
une seule mise a jour -- `PRESSED`/`PRESSING` une fois puis
`PRESS_LOST` au cycle suivant (~30ms), alors que le driver renvoyait
desormais un signal continu confirme par log.

**Cause** : tout objet cree via `lv_obj_create()` est **cliquable par
defaut** sous LVGL (`LV_OBJ_FLAG_CLICKABLE` present dans les flags par
defaut). Le marqueur visuel de debug (petit carre rouge affiche a
l'endroit touche) etait cache au depart, donc invisible pour la
detection de collision au premier appui -- mais des qu'il apparaissait
et se repositionnait exactement sur le point touche, LVGL refaisait un
test de collision au cycle suivant, trouvait desormais le marqueur
(au lieu de l'ecran, l'objet initialement presse) sous le doigt, et
declarait l'appui d'origine perdu (`PRESS_LOST`) -- meme sans le
moindre mouvement reel du doigt.

**Fix** : `lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE)` sur tout
objet purement visuel/decoratif qui pourrait se retrouver positionne
sous un point de contact actif (marqueurs, indicateurs, labels
d'affichage non interactifs) -- a appliquer par reflexe sur ce genre
d'element des la construction de l'UI tactile, plutot que de le
decouvrir a l'usage.

## Structure des projets PlatformIO

Deux projets distincts (un par board), même squelette :

```
<board>_bringup/
├── include/
│   └── lv_conf.h          (LVGL v8.3.11, un seul fichier par board)
├── lib/
│   ├── imu.../            (i2c_bsp + qmi8658c)
│   ├── adc.../            (adc_bsp + user_app)
│   ├── sd.../              (sd_card_bsp)
│   └── lvgl_display.../    (esp_lcd_sh8601 + driver tactile)
├── mains/                  (un main.cpp de référence par test --
│                            à copier dans src/main.cpp selon le test)
└── platformio.ini
```

Code driver copié tel quel depuis les démos officielles Waveshare
(zip wiki pour le 1.64, dépôt GitHub `waveshareteam/ESP32-S3-AMOLED-1.91`
pour le second) -- seuls les GPIO et deux-trois constantes changent
d'un board à l'autre, la logique reste identique.

**Firmware unifié** (`firmware_1.91/`, projet séparé des deux dossiers
de bring-up ci-dessus) : combine tous les modules validés du 1.91 en
un seul programme, avec un module `lib/i2c_shared/` factorisant l'init
I2C partagée entre IMU et tactile (cf. pièges #5, #11, #12). C'est ce
projet qui sert de base pour construire le vrai firmware.

**`chrono-AMOLED/`** (vrai firmware, ex `firmware_191/` -- dossier
remonté à la racine du repo -- lui-même renommé depuis
`display_only_191/` au moment du passage au GPS/CourseManager/SD/
batterie réels, cf. `chrono-AMOLED/README.md`). A servi de banc de
calibration écran/tactile 100% simulé au départ (mise en page LVGL,
tailles de police, couleurs), avant de recevoir la vraie logique
métier portée depuis `pigteam-chrono-tft`. Écran Statut construit et
validé (cf. piège #13 pour l'historique du bug de couleurs rencontré
et résolu dessus).

**`display_only_191/`** : suite au renommage ci-dessus, un **second**
projet du même nom a été recréé à part -- banc de test d'affichage à
nouveau 100% simulé (aucune dépendance GPS/SD/batterie/WiFi réelle),
maintenu en parallèle de `chrono-AMOLED/` pour continuer à calibrer
l'écran sans risque sur le vrai firmware. Les deux projets partagent
le même code d'écrans/navigation/tactile -- toute évolution visuelle
faite sur l'un est reportée manuellement sur l'autre (pas de lien
automatique entre les deux). Cf. `chrono-AMOLED/README.md`, section
"Ajustements d'affichage", pour le détail des dernières retouches
(taille de police, hints de navigation, vitesse max par tour...).

---

## Prochaines étapes

- **Squelette matériel unifié validé** (`firmware_1.91/`) : les 7
  modules (I2C partagé, IMU, tactile, écran/LVGL, ADC, GPS, encodeur+
  bouton) cohabitent sans conflit dans un seul firmware. Base solide
  pour construire la vraie UI.
- **8 écrans construits et navigables, renommé `firmware_191/`**
  (ex `display_only_191/`) : Statut, Circuit, **Nouveau circuit**
  (écran dédié, intégré à l'anneau), Connexion, Session (liste +
  tours), Réglages, WiFi. Anneau Circuit→Nouveau circuit→Connexion→
  Session→Réglages→Circuit... Polices Teko converties (5 tailles, cf.
  ci-dessous), mise en page calibrée à l'œil sur le vrai board. Bug de
  couleurs RGB565 rencontré et résolu (piège #13).
- **Splash screen PigTeam** ajouté au démarrage (3.5s avant bascule sur
  le Statut) -- image convertie depuis le format Piskel/ARGB8888
  d'origine vers RGB565 LVGL, piège d'inversion rouge/bleu rencontré et
  corrigé au passage (piège #18).
- **Tout-tactile finalement engagé pour de vrai** : une fois le
  tactile fiabilisé (pièges #14/#15), Circuit/Session/Réglages sont
  passés au **tap direct** sur les lignes (sélectionne + valide en un
  seul geste, cf. pièges #16/#17), Circuit/Session/Tours sont des
  **listes défilables** au glissement vertical (conteneur LVGL flex +
  scroll natif), et le **swipe horizontal** change d'écran dans
  l'anneau.
- **Encodeur rotatif retiré, contrôles REC revus (27-28/07)** :
  l'EC11 (CLK/DT) a été physiquement retiré (faux contacts tactiles
  capacitifs constatés en parallèle, cause matérielle distincte) et
  remplacé par un simple bouton poussoir sur la même broche (PUSH,
  GPIO10) -- BACK reste un bouton séparé. Plus de bouton REC tactile
  ni de widget cliquable sur le chemin d'enregistrement : seuls
  PUSH/BACK (contacts mécaniques) contrôlent le REC, via un écran de
  confirmation à deux temps façon RaceChrono (PUSH pendant
  l'enregistrement -> pause ; PUSH sur l'écran de pause -> reprend ;
  BACK -> arrêt définitif, avec timeout de sécurité 5 min). Détail
  complet (les deux bugs distincts à l'origine de ce changement, plus
  le piège d'arithmétique non signée sur le timeout) dans
  `chrono-AMOLED/README.md`, section "Contrôles REC / arrêt
  d'enregistrement".
- **5 polices Teko** (Bold 84/56/38, Medium 34/26 -- taille
  intermédiaire Bold 56 ajoutée pour l'horloge, trop grande en 84px).
  Plage de caractères étendue à Latin-1 (accents français).
- **Intégration du chrono réel FAITE et VALIDÉE AU BANC** (`firmware_191/`) :
  GPS (`GpsManager`), détection circuit/tour (`CourseManager`, vendoré
  depuis la vraie lib publique **DovesLapTimer** --
  github.com/TheAngryRaven/DovesLapTimer), sessions (`/sessions.csv`,
  LittleFS), batterie (ADC interne), stockage SD (`SdLogStorage`
  adapté SDMMC) -- tous portés depuis `pigteam-chrono-tft` à la place
  de l'ancienne couche `simXxx()`. Compile et tourne sur le vrai board
  (GPS actif, batterie réelle, 8 circuits PIGTEAM chargés depuis
  `circuits.csv`). Deux pièges de compilation rencontrés et résolus :
  dépendance `hideakitai/ArxTypeTraits` manquante (requise par
  DovesLapTimer, version `^0.3.5` inexistante au registre PlatformIO --
  corrigé sans contrainte de version), et variables/fonction GPS
  (`gpsFixStatus`/`gpsNumSVs`/`gpsSpeedKmh`/`gpsUpdateFromLiveData()`)
  supprimées par erreur lors du remplacement en bloc de l'ancienne
  couche de simulation -- restaurées. **Testé en conditions réelles
  avec un vrai fix GPS** : géofencing confirmé fonctionnel (activation
  automatique du bon circuit à 1.4km de distance, log "Geofencing : a
  1.4km de... -- activation directe" observé au banc, GPS en
  extérieur) -- reste à valider la détection de ligne/tour en roulant.
  Détail complet dans `chrono-AMOLED/README.md` (dossier renommé depuis).
- **`WebServerManager` intégré et fonctionnel** (`firmware_191/`) --
  ~20 routes portées telles quelles (Sessions, Comparer, Circuits
  éditables, Statut, Import/Export CSV/RaceChrono, sauvegarde `.zip`,
  restauration, OTA Firmware...). Point d'accès WiFi `ChronoMotoAMOLED`,
  schéma de partitions dédié (`partitions_ota_16mb.csv`, OTA-compatible,
  16MB). Deux bugs de concurrence/ressources trouvés et corrigés
  (démarrage WiFi bloquant appelé depuis un callback tactile LVGL déjà
  verrouillé ; fuite de descripteur de fichier dans
  `forEachGpsLogFile()`). **Bug résiduel non résolu, contourné** : la
  page d'accueil plante de façon reproductible sur certaines sessions
  réelles précises (corruption mémoire isolée par `addr2line` +ajout de
  logs de diagnostic jusqu'à `lapTimeToMs()`/`handleHomePage()`, cause
  exacte non identifiée -- nécessiterait un vrai débogueur JTAG/GDB).
  Contournement : la page d'accueil n'affiche plus que le résumé par
  session (le détail tour-par-tour + graphique, source du plantage, a
  été retiré de cette page -- reste accessible via `/lap`, avec le même
  risque résiduel sur la session concernée). Détail complet dans
  `chrono-AMOLED/README.md` (dossier renommé depuis).
- Batterie : connecteur MX1.25 confirmé compatible LiPo 3.7V direct
  (recharge/décharge gérées par le circuit du board, pas de buck/boost
  à ajouter) -- pas de batterie de ce type disponible pour test dans
  l'immédiat, à reprendre plus tard.
- **`display_only_191/` remis à niveau (28/07)** : après le renommage
  en `chrono-AMOLED/`, le banc avait pris du retard (retrait de
  l'encodeur, écran de confirmation d'arrêt...) et un fichier
  intermédiaire avait même écrasé son firmware de test par erreur.
  Repris depuis le `main.cpp` réel de `chrono-AMOLED/` et
  "dé-réalisé" (GPS/CourseManager/SD/batterie/WiFi remplacés par une
  simulation RAM aux mêmes signatures de fonctions) pour redevenir
  100% simulé -- cycle recherche->détecté->REC piloté par la sélection
  d'un circuit puis PUSH, tours simulés à durée aléatoire (8-29s) pour
  vérifier dernier/meilleur tour, batterie/horloge/WiFi factices.
  Petite série de retouches d'affichage testées ici puis reportées sur
  `chrono-AMOLED/` (police Dernier/Best/Tours agrandie, texte de
  l'écran de pause raccourci, vitesse max par tour, hints de
  navigation retirés) -- détail dans `chrono-AMOLED/README.md`, section
  "Ajustements d'affichage".
