#pragma once

// ===================== Module IMU (QMI8658, accelerometre + gyroscope) =====================
//
// Capteur 6 axes deja valide au bring-up (README_AMOLED_bringup.md,
// etape "02_I2C_QMI8658", via le sketch Arduino/Wire du demo Waveshare)
// -- gravite ~9.3 sur Z carte a plat, gyro quasi nul au repos. Reutilise
// ici pour estimer l'angle d'inclinaison en virage (lean angle) et
// journaliser les valeurs brutes -- PAS d'affichage ecran, purement
// pour l'analyse post-session (pas le temps de regarder un ecran en
// pleine inclinaison).
//
// IMPORTANT -- ce module utilise les fonctions I2C bas niveau de
// i2c_bsp.h (I2C_write_buff/I2C_read_buff, driver ESP-IDF natif deja
// installe par I2C_master_Init()), PAS une lib Arduino a base de
// Wire/TwoWire : une lib Wire ferait son propre i2c_driver_install()
// sur le meme port I2C_NUM_0 deja pris, provoquant exactement le
// conflit que touch_bsp.c evite deja pour le FT3168 (meme bus
// physique, cf. son commentaire d'en-tete). initImu() DOIT donc etre
// appelee apres I2C_master_Init(), comme Touch_Init().
//
// STATUT (29/07) -- registres valides sur le vrai board : WHO_AM_I
// repond 0x05, magnitude accelero ~1g stable dans toutes les
// orientations testees (fix CTRL1 auto-increment necessaire, cf. son
// commentaire dans le .cpp). Axe roll confirme par test reel (carte
// penchee a gauche/droite) : Y est l'axe qui bascule avec le roulis,
// X est l'axe "vertical au repos" -- roll = atan2(accelY, accelX), pas
// atan2(accelY, accelZ) comme suppose au depart. Gyro Y suppose porter
// le meme axe physique (pas encore verifie en dynamique, cf.
// commentaire dedie dans le .cpp) -- impact limite de toute facon, le
// filtre complementaire ne lui donne que 2% de poids face a
// l'accelerometre.

// Initialise le capteur sur le bus I2C partage (deja monte par
// I2C_master_Init() -- meme bus que le tactile FT3168). Retourne false
// si WHO_AM_I ne repond pas 0x05 -- le reste du firmware continue de
// fonctionner normalement (getLeanAngleDeg() renverra simplement 0,
// getImuRaw() des zeros).
bool initImu();

// A appeler a chaque tour de loop() (inconditionnellement, meme hors
// REC, pour que le filtre reste "chaud"/stable des le debut d'un
// enregistrement). Se limite en interne a ~50Hz -- pas la peine de
// solliciter l'I2C partage plus vite. No-op si initImu() a echoue.
void imuTick();

// Angle d'inclinaison courant (roll), en degres signes -- positif d'un
// cote, negatif de l'autre (a interpreter selon le mapping d'axe
// final, cf. commentaire en tete de fichier). Calcule par filtre
// complementaire (gyro integre entre deux appels + recale lentement
// sur l'accelerometre) pour rester correct meme sous forte
// acceleration laterale en virage, contrairement a un simple
// atan2(accel) qui confondrait gravite et force centripete.
float getLeanAngleDeg();

// Dernieres valeurs brutes lues (mg pour l'accelerometre, dps pour le
// gyroscope) -- pour le log detaille (log_*.csv), permet de
// recalculer/corriger l'angle en post-traitement si besoin.
void getImuRaw(float& accelXmg, float& accelYmg, float& accelZmg,
               float& gyroXdps, float& gyroYdps, float& gyroZdps);

extern bool imuOk; // false si le capteur n'a jamais repondu a initImu()
