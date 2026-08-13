#ifndef EXPANDER_BSP_H
#define EXPANDER_BSP_H

// ===================== IO-expander TCA9554 -- reset ecran+tactile (2.41 V2) =====================
//
// CORRECTION MAJEURE (12/08) : notre carte est un 2.41 **V2** (etiquette
// "V2" a cote du marquage "2.41"), une revision materielle absente du
// wiki/repo GitHub "principal" (V1) qu'on a utilise toute la journee.
// Confirme via le repo officiel dedie :
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2
//
// Difference cle : sur le V1, AMOLED_RST est un vrai GPIO direct (GPIO21).
// Sur le V2, GPIO21 n'est PLUS cable au reset de l'ecran -- le reset de
// l'ecran (et du tactile) passe desormais par le TCA9554 (adresse I2C
// 0x20, meme bus partage GPIO47/48) :
//   - EXIO0 (bit 0) = reset ecran (OLED_RESET)
//   - EXIO1 (bit 1) = reset tactile (TP_RESET)
//   - EXIO5 (bit 5) = egalement configure en sortie par la demo V2
//     (mais pas utilise pour l'ecran/tactile -- laisse tel quel par
//     coherence avec le firmware V2 officiel)
//
// C'est exactement pour ca que tout ce qu'on a flashe aujourd'hui
// (notre code V1, la demo V1 officielle, le .bin du wiki V1, le
// 10_FactoryProgram V1) laissait l'ecran noir malgre une init QSPI par
// ailleurs parfaitement correcte : le reset materiel de l'ecran ne
// partait jamais, GPIO21 ne va nulle part sur cette carte.
//
// L'ancienne version de ce fichier (avant cette correction) essayait
// d'activer un "AMOLED_EN" sur EXIO1 -- fausse piste, EXIO1 est en fait
// le reset tactile, pas une activation d'alimentation.

#ifdef __cplusplus
extern "C" {
#endif

// A appeler UNE FOIS dans setup(), juste apres I2C_master_Init(),
// AVANT tout le reste (Touch_Init(), displayInit()) -- configure les
// broches EXIO en sortie (config register) sans encore les piloter.
void expanderInit();

// Pulse de reset (haut -> bas 20ms -> haut, puis 120ms d'attente) sur
// EXIO0 (ecran). A appeler AVANT displayInit() -- sans ce pulse,
// l'ecran QSPI reste noir meme si toutes les commandes d'init sont
// par ailleurs correctement envoyees (le controleur RM690B0 n'est
// simplement jamais sorti de son etat de reset materiel).
void expanderResetOled();

// Meme chose pour EXIO1 (tactile). A appeler AVANT Touch_Init().
void expanderResetTouch();

#ifdef __cplusplus
}
#endif
#endif
