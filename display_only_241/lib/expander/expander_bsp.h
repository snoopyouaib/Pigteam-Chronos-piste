#ifndef EXPANDER_BSP_H
#define EXPANDER_BSP_H

// ===================== IO-expander TCA9554 (EXIO0-7, 2.41) =====================
//
// N'existe pas sur le 1.91 -- nouveau sur le 2.41. AMOLED_TE, AMOLED_EN,
// TP_INT, IMU_INT1/2, RTC_INT et EXIO5-7 passent par ce TCA9554 I2C
// (adresse 0x20, meme bus partage GPIO47/48 que le tactile/IMU/RTC),
// au lieu d'etre des GPIO ESP32-S3 directs comme sur le 1.91.
//
// Seul EXIO1 (AMOLED_EN) nous interesse pour l'instant : sans lui a 1,
// le panneau RM690B0 peut rester sans alimentation meme si toutes les
// commandes QSPI sont correctement envoyees (ecran noir malgre un
// displayInit() qui ne plante pas).

#ifdef __cplusplus
extern "C" {
#endif

// A appeler UNE FOIS dans setup(), APRES I2C_master_Init() et AVANT
// displayInit() -- configure EXIO1 en sortie et le met a 1 (AMOLED_EN
// actif). Les autres EXIO restent en entree par defaut (etat le plus
// sur pour TP_INT/IMU_INT/RTC_INT qu'on ne pilote pas ici).
void expanderInit();

#ifdef __cplusplus
}
#endif
#endif
