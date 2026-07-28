#ifndef I2C_BSP_H
#define I2C_BSP_H
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===================== Bus I2C partage IMU + tactile =====================
//
// Sur le 1.91, QMI8658 (IMU) et FT3168 (tactile) sont sur le MEME bus
// I2C (GPIO39/40) -- cf. README_AMOLED_bringup.md, piege #5. Chacun des
// deux drivers d'origine (Waveshare) fait son propre i2c_param_config()
// + i2c_driver_install() sur I2C_NUM_0 : appeler les deux tels quels
// provoquerait un echec du second install (bus deja pris), et les deux
// fichiers definissaient chacun leur propre copie de I2C_write_buff/
// I2C_read_buff (symboles dupliques -> erreur de lien des qu'on les
// combine).
//
// Ce module factorise donc l'init I2C en un seul endroit -- a appeler
// UNE SEULE FOIS dans setup(), avant qmi8658_init() et Touch_Init().
// Les deux drivers (qmi8658c.cpp, touch_bsp.c) utilisent ensuite ces
// memes fonctions I2C_write_buff/I2C_read_buff/I2C_master_write_read_device
// (memes signatures que les fichiers Waveshare d'origine, donc aucune
// modification necessaire cote qmi8658c.cpp).

void I2C_master_Init(void);
uint8_t I2C_write_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len);
uint8_t I2C_read_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len);
uint8_t I2C_master_write_read_device(uint8_t addr,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen);

#ifdef __cplusplus
}
#endif
#endif
