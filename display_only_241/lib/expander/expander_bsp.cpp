#include "expander_bsp.h"
#include "i2c_bsp.h"
#include <stdio.h>

// Registres TCA9554 standard :
//   0x00 Input Port, 0x01 Output Port, 0x02 Polarity Inversion,
//   0x03 Configuration (1 = entree, 0 = sortie -- inverse du bon sens)
#define I2C_ADDR_TCA9554   0x20
#define REG_OUTPUT         0x01
#define REG_CONFIG         0x03

#define EXIO_TE    (1 << 0)  // AMOLED_TE -- entree
#define EXIO_EN    (1 << 1)  // AMOLED_EN -- sortie, celle qu'on pilote
#define EXIO_TPINT (1 << 2)  // TP_INT -- entree
#define EXIO_INT2  (1 << 3)  // IMU_INT2 -- entree
#define EXIO_INT1  (1 << 4)  // IMU_INT1 / RTC_INT -- entree

void expanderInit()
{
  // Configuration : tout en entree (bit=1) sauf EXIO_EN en sortie (bit=0).
  uint8_t config = (uint8_t)(~EXIO_EN);
  uint8_t ret1 = I2C_write_buff(I2C_ADDR_TCA9554, REG_CONFIG, &config, 1);

  // Sortie : EXIO_EN a 1 (AMOLED_EN actif), reste a 0 (sans effet sur
  // les broches configurees en entree).
  uint8_t output = EXIO_EN;
  uint8_t ret2 = I2C_write_buff(I2C_ADDR_TCA9554, REG_OUTPUT, &output, 1);

  // I2C_write_buff renvoie l'esp_err_t de i2c_master_write_to_device
  // tronque en uint8_t -- 0 (ESP_OK) = le TCA9554 a bien ACKe. Non nul
  // (typiquement ESP_ERR_TIMEOUT=263->0x07 tronque, ou ESP_FAIL=-1->0xFF)
  // = rien n'a repondu a l'adresse 0x20 sur ce bus.
  printf("[expander] TCA9554 0x20 config=0x%02X ret=%d, output=0x%02X ret=%d\n",
         config, ret1, output, ret2);
}
