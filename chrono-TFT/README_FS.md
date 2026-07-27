# Stockage fichiers -- SD + LittleFS -- variante TFT

Ce document récapitule la démarche et les pièges rencontrés pour rebrancher
une carte SD sur ce projet, en complément du stockage LittleFS. Écrit après
coup pour servir de référence -- notamment pour refaire la même intégration
côté variante OLED sans retomber dans les mêmes ornières.

## Pourquoi

LittleFS seule sature vite : une journée de piste classique (6 sessions)
occupe déjà plusieurs Mo, et un import RaceChrono complet peut à lui seul
peser 700 Ko-1 Mo. Avec une table de partitions généreuse en OTA (2x 3 Mo
par défaut), il ne restait qu'environ 1,9 Mo de LittleFS sur le TFT -- pas
de quoi tenir une journée complète, encore moins plusieurs à la suite.

Deux leviers ont été actionnés en parallèle sur ce projet :
1. **Réduire les partitions OTA** (3 Mo -> 1,75 Mo chacune, cf.
   `partitions_ota.csv`) pour agrandir LittleFS à ~4,5 Mo -- déjà un net
   mieux, mais qui reste limité par la taille de la flash (8 Mo au total).
2. **Déporter les logs GPS détaillés sur une carte SD** (ce document) --
   LittleFS ne garde plus que la config (`circuits.csv`) et le carnet
   cumulatif (`sessions.csv`), légers et peu amenés à grossir vite.

## Câblage -- bus SPI partagé avec l'écran

Avec seulement 3 GPIO libres sur ce montage (1, 4, 5) et une carte SD SPI
classique qui demande 4 signaux (MOSI/MISO/SCK/CS), le partage du bus avec
l'écran ST7789 (déjà en SPI matériel) était la seule option réaliste.

L'écran n'utilise **jamais MISO** (écriture seule, pas de lecture) --
`tftSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS)` le confirmait déjà avant ce
changement. Ça libère de fait MISO pour la SD, qui n'a donc besoin que de
2 GPIO neufs (CS + MISO) sur les 3 disponibles, laissant GPIO 1 en réserve.

| Signal | Pin      | Partagé avec l'écran ? |
|--------|----------|-------------------------|
| MOSI   | GPIO 11  | Oui (TFT_MOSI)          |
| SCLK   | GPIO 12  | Oui (TFT_SCLK)          |
| CS     | GPIO 4   | Non -- dédié SD         |
| MISO   | GPIO 5   | Non -- dédié SD (inutilisé par l'écran) |

Côté logiciel, le bus partagé est un seul objet `SPIClass tftSPI(FSPI)`,
initialisé une fois avec le vrai pin MISO (`tftSPI.begin(TFT_SCLK, SD_MISO,
TFT_MOSI, TFT_CS)`), puis réutilisé tel quel pour la SD (`SD.begin(SD_CS,
tftSPI)`). Pas besoin d'un second objet `SPIClass`.

**Vérification faite avant d'aller plus loin :** écran et SD actifs en
même temps sur le même bus, aucun artefact visuel, aucune corruption de
données constatée sur plusieurs sessions.

## Bibliothèque -- `<SD.h>`, pas `SdFat`

Deux options existaient : la lib SD intégrée au framework Arduino-ESP32
(`<SD.h>`) et `SdFat` (bibliothèque tierce, réputée mieux maintenue sur
ESP32-S3).

**Premier essai : SdFat.** Compile et fonctionne très bien en test isolé
(lecture/écriture confirmées). Mais son API (`SdFat32`, `File32`) est
totalement différente de `fs::FS`/`fs::File` -- l'interface que tout
`WebServerManager.cpp` utilise déjà (`g_logsFs->open()`, `->exists()`,
`->remove()`...). Intégrer SdFat proprement aurait demandé de réécrire
toute la couche d'accès fichier de `WebServerManager.cpp`, ou d'implémenter
un adaptateur `fs::FS` par-dessus SdFat -- non trivial, `fs::FS` n'étant
pas pensé pour être sous-classé facilement (il délègue à un `FSImpl`
privé, pas une interface publique pensée pour l'extension tierce).

**Solution retenue : `<SD.h>`.** Implémente `fs::FS` nativement (même
interface que `LittleFS`) -- s'intègre donc directement dans `gpsLogFs`
sans toucher une ligne de `WebServerManager.cpp`. Un échec de compilation
avait été signalé lors d'une tentative précédente sur ce montage, mais
s'est avéré être tout autre chose une fois creusé (cf. piège suivant) --
pas un vrai problème de compatibilité ESP32-S3.

## Piège PlatformIO -- conflit de nom sur "SD"

**Symptôme :** `#error Architecture or board not supported.` dans
`Sd2PinMap.h`, suivi d'une cascade d'erreurs `reference to 'File' is
ambiguous` (`SDLib::File` vs `fs::File`) et de redéfinitions
`FILE_READ`/`FILE_WRITE`.

**Cause réelle :** PlatformIO a **deux bibliothèques homonymes** :
- Celle intégrée au framework (`framework-arduinoespressif32/libraries/SD`)
  -- compatible ESP32-S3, hérite de `fs::FS`. C'est celle qu'on veut.
- Un (ou deux) paquet(s) du **registre public PlatformIO**, littéralement
  nommé(s) `SD` (`arduino-libraries/SD@1.3.0`, `adafruit/SD@0.0.0-alpha`)
  -- l'ancienne lib historique AVR/`SDLib`, qui ne connaît même pas
  l'ESP32.

Le Library Dependency Finder, face à `#include <SD.h>` sans directive
explicite, télécharge et privilégie une des versions du registre au lieu
de celle du framework.

**Ce qui NE marche PAS :** `lib_ignore = SD` -- exclut les deux
indifféremment (même nom), y compris la bonne. Résultat : `fatal error:
SD.h: No such file or directory`.

**Ce qui marche :** déclarer `SD` explicitement dans `lib_deps` en
pointant *directement* sur le dossier de la lib framework, via la
variable PlatformIO `${platformio.packages_dir}` (se résout au chemin
d'installation propre à chaque machine -- portable, pas de chemin
utilisateur en dur) :

```ini
lib_deps =
    ...
    file://${platformio.packages_dir}/framework-arduinoespressif32/libraries/SD
```

Plus d'ambiguïté possible : PlatformIO n'a plus besoin de deviner, la
source est donnée explicitement.

**Après tout changement de `lib_deps`/`lib_ignore` :** toujours supprimer
le dossier `.pio` **en entier** avant de relancer `pio run`. Un rebuild
simple ne suffit pas -- les dépendances déjà résolues/téléchargées restent
en cache et masquent l'effet du changement.

## Intégration logicielle -- `gpsLogFs`

Un pointeur global `fs::FS* gpsLogFs` décide où vivent les logs GPS
détaillés (`log_*.csv`) :

```cpp
fs::FS* gpsLogFs = &LittleFS; // valeur par defaut -- repli si la SD est absente/en panne
```

Dans `setup()`, après `LittleFS.begin()` :

```cpp
if (!SD.begin(SD_CS, tftSPI)) {
  // repli sur LittleFS -- gpsLogFs deja initialise a &LittleFS, rien a faire
} else {
  gpsLogFs = &SD;
  migrateLittleFsLogsToSd(); // cf. section suivante
}
```

**Ce qui suit `gpsLogFs` :** écriture pendant `REC` (`startRecording()`),
listing (`forEachGpsLogFile()`), dump Serial, suppression -- et surtout
l'appel à `webServerManager.begin(..., *gpsLogFs, ...)`, qui propage le
choix à **toutes** les fonctionnalités web (téléchargement, suppression,
comparaison, sauvegarde ZIP, imports...) sans qu'aucune d'elles n'ait eu
besoin d'être modifiée.

**Ce qui reste TOUJOURS sur LittleFS, quel que soit l'état de la SD :**
`sessions.csv` (carnet cumulatif) et `circuits.csv` (configuration) --
petits fichiers, peu volatils, pas de raison de les exposer au risque
d'une carte SD retirée en cours de route.

## Migration des sessions pré-existantes

Au moment de basculer `gpsLogFs` sur la SD, les sessions déjà enregistrées
(ou importées) restent physiquement sur LittleFS -- invisibles côté
WebServerManager, qui ne scanne plus que `gpsLogFs`, tout en continuant à
occuper une place précieuse.

`migrateLittleFsLogsToSd()` (appelée juste après la bascule sur SD) :
- scanne LittleFS à la recherche de `log_*.csv`,
- copie chacun vers la SD,
- **vérifie la taille copiée avant de supprimer la source** -- aucun
  risque de perte si la carte est retirée ou tombe en panne en plein
  milieu,
- idempotente : ne fait rien dès la 2e fois (plus rien à trouver une fois
  la migration terminée).

Testé avec 6 sessions réelles : migration complète, 0 échec, LittleFS
passé de 11 % à 100 % libre.

## Barre de stockage SD sur la page Sessions

`usedBytes()`/`totalBytes()` ne font **pas** partie de l'interface
commune `fs::FS` -- chaque implémentation (LittleFS, SD) les ajoute à
part. `WebServerManager.cpp` ne peut donc pas les appeler à travers le
pointeur générique `g_logsFs`. Solution : `main.cpp` (qui connaît le type
concret) les expose via `WebServerStatusInfo`, déjà utilisée pour tout le
reste du statut système :

```cpp
// main.cpp, dans getStatusCallback()
if (gpsLogFs == &SD) {
  s.hasSeparateLogsFs = true;
  s.logsFsLabel = "SD (logs GPS detailles)";
  s.logsFsUsedBytes = SD.usedBytes();
  s.logsFsTotalBytes = SD.totalBytes();
}
```

`WebServerManager.cpp` n'affiche cette 2e barre que si
`hasSeparateLogsFs` est vrai -- pas de barre redondante quand `gpsLogFs`
est en repli sur LittleFS (déjà couverte par sa propre barre).
`storageBarHtml()` a été passée de `size_t` à `uint64_t` à cette
occasion -- une carte SD peut largement dépasser les ~4 Go plafond d'un
`size_t` 32 bits.

## Choisir une carte SD

Testé et validé avec une carte de ~1 Go -- confirme qu'aucune capacité
minimale n'est requise pour ce projet (une journée complète de piste
pèse quelques Mo, cf. plus haut). Quelques repères pour l'achat :

- **Capacité :** en 2026, quasiment plus de cartes neuves sous 16-32 Go
  dans le commerce (vérifié -- même les gammes "basiques" démarrent
  largement au-dessus). Pas la peine de chercher du vintage en 500 Mo-1
  Go : prendre du 16 ou 32 Go ne coûte rien de plus et évite de traquer
  une capacité difficile à trouver/potentiellement contrefaite.
- **Format de fichiers -- point bloquant si ignoré :** `<SD.h>` ne gère
  que FAT16/FAT32, pas exFAT. Les cartes ≤32 Go sortent generalement deja
  en FAT32 d'usine (pas de manip a prevoir). Au-dela de 32 Go, elles
  sortent en exFAT par defaut -- `SD.begin()` echouera tel quel. Soit
  rester en ≤32 Go, soit reformater en FAT32 depuis un PC avant usage
  (Windows refuse ce formatage en natif au-dela de 32 Go -- utiliser un
  outil type SD Card Formatter, officiel SD Association).
- **Endurance/vibrations :** usage prevu = ecriture continue en roulant,
  vibrations moto. Les cartes "high endurance" (SanDisk High Endurance,
  Samsung PRO Endurance...) sont concues pour de l'ecriture en boucle
  continue (dashcams, videosurveillance) plutot que le stockage
  occasionnel classique -- pas obligatoire mais plus rassurant sur la
  duree qu'une carte grand public.
- **Marque :** rester sur du connu (SanDisk, Samsung, Kingston,
  Transcend). Le marche des cartes bon marche regorge de contrefacons
  qui annoncent une capacite qu'elles n'ont pas reellement, et qui
  plantent silencieusement une fois la vraie capacite physique depassee.
- **Prevoir plusieurs cartes** (2-3) -- prix derisoire a cette capacite,
  autant avoir des rechanges sous la main un jour de piste plutot que de
  dependre d'un seul exemplaire.

## Fichiers partagés vs spécifiques à chaque variante

Point à ne pas oublier avant de continuer à faire évoluer le WebServer
commun : tous les changements ci-dessus ne touchent pas les fichiers de
la même façon.

**Mise à jour -- extraction de `SdLogStorage.h/.cpp` :** la logique
`gpsLogFs`/`migrateLittleFsLogsToSd()`/`sdUsedBytes()`/`sdTotalBytes()`,
d'abord écrite directement dans le `main.cpp` du TFT, a été extraite en
module partagé lors de l'intégration côté OLED -- meme esprit que
`GpsManager.h`/`WebServerManager.h` (interface minimale, mutualisée
telle quelle). Le TFT a ensuite été aligné dessus a son tour : les deux
`main.cpp` ne gardent plus que ce qui differe reellement (le bus SPI --
partage avec l'ecran cote TFT, dedie cote OLED) et appellent le module
pour tout le reste.

| Fichier                  | Partagé OLED/TFT ? | Impact de ce travail |
|---------------------------|---------------------|------------------------|
| `WebServerManager.h`      | **Oui**             | Nouveaux champs `hasSeparateLogsFs`/`logsFsLabel`/`logsFsUsedBytes`/`logsFsTotalBytes` sur `WebServerStatusInfo`, `storageBarHtml()` en `uint64_t`. |
| `WebServerManager.cpp`    | **Oui**             | `gpsLogFs`, migration, barre SD conditionnelle. |
| `SdLogStorage.h`/`.cpp`   | **Oui**             | Module dédié : `gpsLogFs`/`gpsLogsOnSd`, `initSdLogStorage(csPin, spi)`, `migrateLittleFsLogsToSd()`, `sdUsedBytes()`/`sdTotalBytes()`. Ne touche jamais au bus SPI lui-même (reçoit un `SPIClass&` déjà initialisé). |
| `main.cpp`                | **Non** (un par variante) | Ne garde que le câblage propre à chaque carte : pins CS/MISO/MOSI/SCLK, init du bus SPI (partagé avec l'écran côté TFT via `tftSPI`, dédié côté OLED via `sdSPI`), et l'appel à `initSdLogStorage()`/`migrateLittleFsLogsToSd()`/remplissage des champs de statut dans `getStatusCallback()`. |

**Rétrocompatibilité vérifiée :** les nouveaux champs de
`WebServerStatusInfo` ont des valeurs par défaut neutres (`false`, `0`).
Un `main.cpp` qui ne les renseigne pas ne casse rien -- `hasSeparateLogsFs`
reste `false`, la 2e barre reste simplement invisible.

## Checklist pour refaire cette intégration sur une nouvelle carte

1. Reporter tel quel `WebServerManager.h`/`WebServerManager.cpp` et
   `SdLogStorage.h`/`SdLogStorage.cpp` (tous partagés) -- sans risque,
   rétrocompatible tant que la SD n'est pas câblée (cf. section
   précédente).
2. Identifier les GPIO libres et si l'écran (I2C, pas de MISO à
   récupérer comme sur un bus SPI déjà en place) laisse la place pour 4
   signaux SPI dédiés, ou s'il faut partager un bus existant.
3. Câbler, tester en bas niveau isolé (`SdFat` ou `SD.h`, peu importe à ce
   stade -- juste confirmer lecture/écriture) avant toute intégration.
4. Si usage de `<SD.h>` : ajouter `file://${platformio.packages_dir}/...`
   dans `lib_deps` **dès le départ**, pour éviter de retomber dans le
   piège du conflit de nom.
5. Dans `main.cpp` : créer/initialiser le bus SPI (`SPIClass` dédié ou
   partagé selon le montage), puis appeler `initSdLogStorage(csPin, spi)`
   -- tout le reste (repli LittleFS, migration, stats) est déjà géré par
   le module.
6. Appeler `migrateLittleFsLogsToSd()` si `initSdLogStorage()` a renvoyé
   `true`.
7. Remplir les nouveaux champs de `WebServerStatusInfo` dans
   `getStatusCallback()` (via `gpsLogsOnSd`/`sdUsedBytes()`/
   `sdTotalBytes()`) pour faire apparaître la barre SD.
8. Toujours supprimer `.pio` entièrement après un changement de
   `platformio.ini`.
