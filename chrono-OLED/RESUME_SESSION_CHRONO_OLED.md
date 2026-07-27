# Résumé de session — PIGTEAM Chrono OLED (ESP32-S3-Zero)
*Conversation du 1er juillet 2026 — projet `pigteam-chrono-oled`*

---

## 1. Contexte du projet

Chrono GPS moto piste **pour la moto du fils**. Contrainte principale :
pas d'affichage "live" distrayant — il reste concentré sur le pilotage.
L'écran OLED montre juste le **dernier tour** et le **meilleur temps**,
mis à jour uniquement au franchissement de la ligne.

Le GPS vient d'un **RaceBox Micro BLE** (25Hz, SAM-M10Q) partagé depuis
le kit du projet TFT.

---

## 2. Matériel validé

| Composant | Détail |
|---|---|
| MCU | ESP32-S3-Zero (4MB flash / 2MB PSRAM) |
| Écran | OLED 1.3" SH1106 128×64 I2C + encodeur EC11 sur même module |
| Alimentation | LiPo 3.7V 1000mAh + module boost IP68 5V |
| GPS | RaceBox Micro BLE (partagé avec la CBR125 TFT) |

### Câblage GPIO (ordre physique du connecteur, sans croisement)

| Signal | GPIO |
|---|---|
| CONFIRM | 7 |
| OLED SDA | 8 |
| OLED SCL | 9 |
| Encodeur PUSH | 10 |
| Encodeur CLK (A) | 11 |
| Encodeur DT (B) | 12 |
| BACK | 13 |
| GPIO libres pour microSD | 2, 4, 5, 6 |

---

## 3. Architecture firmware (1494 lignes, 4 fichiers src/)

```
src/
  main.cpp              (~1494 lignes)
  WebServerManager.h    interface du module WiFi/HTTP
  WebServerManager.cpp  implémentation
  logo_pigteam_xbm.h    bitmap monochrome splash boot
```

### Points techniques importants

- **BLE scan par fenêtres bornées** : 4s scan / 5s pause, interval=100/window=10
  → évite de bloquer l'UI/boutons pendant la recherche du RaceBox
- **CONFIRM + BACK par interruption matérielle** (ISR + anti-rebond 25ms
  confirmé en boucle principale) → ne rate plus d'appuis pendant le scan BLE
- **Jamais de `BLEDevice::deinit()`** : crash systématique (StoreProhibited)
  confirmé sur les cycles WiFi/BLE répétés ; solution = déconnecter le client
  + arrêter le scan, laisser la pile BLE initialisée mais inactive
- **`timegm()` absent** du toolchain → `utcTmToEpoch()` custom
- **Fuseau CET/CEST** : `setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1)`
- **Encodeur A/B échangés** dans le constructeur uniquement (sens de rotation
  inversé sans toucher au câblage ni aux #define)
- **`ARDUINO_USB_CDC_ON_BOOT=1`** obligatoire (pas de puce UART CH343 sur
  cette carte)
- **Partition flash** : `default.csv` (4MB), ou `partitions_ota.csv` si OTA activée

---

## 4. Affichage OLED

### Écran principal (par défaut)

```
0:12.345          ← petit chrono tour en cours (u8g2_font_6x10_tf)
                    visible seulement quand REC actif
1:23.456          ← dernier tour terminé (u8g2_font_logisoso24_tn, gros)
B 1:21.789        ← meilleur temps (u8g2_font_7x14B_tf)
                                              • ← point REC discret (drawDisc)
```

- "En attente..." si aucun tour terminé
- "BT Coupe" si BLE désactivé manuellement
- Splash logo PigTeam 1.8s au boot (bitmap XBM 64×64 converti depuis RGB565)

### Navigation (menu principal, BACK depuis l'écran statut)

```
Menu principal (5 entrées)
├── Circuit
│   ├── Auto (détection)
│   ├── Croix-en-Ternois
│   ├── Carole
│   ├── Pau-Arnos
│   ├── Lédenon
│   ├── Alès
│   ├── Clastres
│   ├── Los Arcos
│   └── Le Mans Bugatti
├── Connexion (BLE/Fix/Sat/circuit actif)
├── Session (consultation carnet par session et par tour)
├── Réglages
│   ├── WiFi téléchargement
│   └── BT (COUPER/ACTIVER)
└── Démo (rejeu Croix-en-Ternois x10)
```

---

## 5. Enregistrement (style RaceChrono)

- **Démarrage automatique** dès qu'un vrai circuit est actif
  (auto-détecté par CourseManager OU forcé depuis le menu) —
  le mode "proximité" (Lap Anything) ne déclenche PAS l'auto-start
- **Arrêt toujours manuel** (CONFIRM) — pas de coupure surprise
- Un arrêt manuel ne redémarre pas tout seul (`autoStartSuppressed`)
  tant que le circuit ne change pas explicitement

---

## 6. Détection de circuit

- **CourseManager** (DovesLapTimer) : 8 circuits calibrés en dur
- **Mode manuel** : `DovesLapTimer` indépendant (CourseManager n'expose
  pas de méthode pour forcer un circuit)
- **Repli Lap Anything** : si aucun des 8 circuits ne correspond, mode
  proximité automatique (moins précis, n'active pas l'auto-start)

### 8 circuits calibrés

| Circuit | Longueur |
|---|---|
| Croix-en-Ternois | 5868 ft |
| Carole | 6699 ft |
| Pau-Arnos | 9747 ft |
| Lédenon | 10242 ft |
| Alès | 8031 ft |
| Clastres | 7770 ft |
| Los Arcos | 12758 ft |
| Le Mans Bugatti | 14050 ft |

---

## 7. Logs et stockage

### Structure fichiers (LittleFS)

| Fichier | Contenu | Taille |
|---|---|---|
| `/sessions.csv` | Carnet cumulatif (résumé par tour) | Quelques Ko |
| `/log_AAAAMMJJ_HHMMSS.csv` | Log GPS détaillé par session (10Hz) | ~780 Ko/20min |
| `/croix_replay.csv` | Données démo (11070 pts) | 595 Ko |

### Problème de capacité

10Hz × 20min = ~780 Ko par session. LittleFS = ~1.4 Mo →
**2 sessions maximum** avant saturation. Solutions prévues :
- **Court terme** : microSD SPI sur GPIO libres (2, 4, 5, 6)
- **Long terme** : remplacer le Zero par ESP32-S3-Tiny N8R8 (8MB flash)
  + OTA via WiFi (plus jamais besoin de rebrancher la nappe USB)

### Commandes Serial

| Commande | Action |
|---|---|
| `l` | Lister les fichiers de log GPS |
| `d` | Dump du dernier log GPS |
| `c` | Effacer tous les logs GPS |
| `s` | Dump du carnet de session |
| `x` | Effacer le carnet de session |
| `w` | Toggle WiFi download mode |
| `e` | Toggle mode démo (rejeu Croix x10) |

---

## 8. Module WebServerManager (src séparé)

Même logique de découplage que la Pendule Paddock. 4 callbacks fournis
par `main.cpp` : `stopBLE`, `restartBLE`, `wifiCallback_flushLogs`,
`webServerCallback_getStatus`.

### Pages disponibles

| Page | Contenu |
|---|---|
| `/` (Sessions) | Fichiers groupés par date, résumé, téléchargement, suppression, barre de stockage |
| `/status` | BLE/Fix/Sat/circuit/REC/heap/uptime, lien Rafraîchir |

Réseau ouvert "ChronoMoto" (WPA2 = crash mémoire confirmé sur cette carte).

---

## 9. Mode démo (rejeu Croix-en-Ternois)

- Rejeu à x10 (~2 min pour les 18 min de session)
- Données réelles, résultat connu : meilleur tour ~1:01.024
  (validé à 0.7ms vs RaceChrono)
- BLE/GPS réel continue en arrière-plan (non perturbé)
- Reboucle automatiquement en fin de fichier
- Accessible depuis le menu Démo ou commande Serial `e`

---

## 10. Matériel en cours / à venir

### Décidé et commandé

- **ESP32-S3-Tiny N8R8** (Waveshare, deux cartes + nappe FPC)
  - 8MB Flash + 8MB PSRAM
  - Nappe utilisée uniquement pour le premier flash
  - Mises à jour suivantes par OTA via WiFi (à implémenter)
  - Livraison prévue avant le 14 juillet

### Prévu mais pas encore commencé

- **Module microSD SPI** pour le Zero actuel
  - Résout le problème de stockage (illimité vs 1.4 Mo)
  - 4 GPIO libres : CS=4, SCK=5, MISO=6, MOSI=2
  - Lib `SD.h` native Arduino

- **NEO-M9N** (GPS UART standalone, 25Hz)
  - Remplacement futur du RaceBox (firmware dédié)
  - Compatible ESP32-S3-Zero ou N16R8 (déjà en main)
  - Plus de BLE = plus de problème de coexistence WiFi/BLE

- **Fonction "Nouveau circuit"** (auto-calibration)
  - Rouler lentement sur la ligne → capture position + cap GPS
  - Saisir la largeur de piste à l'encodeur
  - Calcul trigonométrique des points A/B
  - Sauvegarde en flash comme 9ème circuit

- **OTA sur WebServerManager**
  - Page `/update` avec upload de `.bin`
  - Lib `Update.h` (core Arduino ESP32)
  - Prérequis : table de partitions custom (`partitions_ota.csv`)

- **Réglages WiFi** (paramètres distants)
  - Formulaire web : identifiant moto, SSID personnalisé
  - Pas prioritaire pour l'instant

---

## 11. Trackday 14 juillet 2026

Circuit prévu : **Croix-en-Ternois** (déjà calibré, déjà utilisé pour
les données de démo). La détection automatique devrait enclencher
l'enregistrement dès les premiers tours de sortie de stands.

Deux motos concernées :
- **BMW R1250RS** (ton chrono TFT)
- **CBR125 (fils)** → ce firmware OLED

---

## 12. Ce qui a été validé en test réel

✅ BLE RaceBox connecté + Fix GPS  
✅ OLED SH1106 (driver correct du premier coup)  
✅ Menu navigation (BACK/CONFIRM/encodeur)  
✅ Sélection manuelle de circuit  
✅ Enregistrement automatique au choix de circuit  
✅ Arrêt manuel stable (pas de redémarrage auto)  
✅ Coupure/relance BT manuelle depuis Réglages  
✅ WiFi 4 cycles ON/OFF stables (heap stable ~133-138 Ko après WiFi)  
✅ Page Sessions (groupage, résumé, suppression)  
✅ Mode démo x10 (reboucle, chrono live, best time)  
✅ Splash logo PigTeam au boot  
✅ Partition flash 4MB corrigée  
✅ Boutons par ISR (plus d'appuis manqués pendant scan BLE)  
✅ Encodeur sens inversé (A/B échangés dans constructeur)  
