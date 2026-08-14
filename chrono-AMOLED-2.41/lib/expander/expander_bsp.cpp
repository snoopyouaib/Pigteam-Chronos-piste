#include "expander_bsp.h"
#include "i2c_bsp.h"
#include <Arduino.h> // delay()

// Registres TCA9554 standard :
//   0x00 Input Port, 0x01 Output Port, 0x02 Polarity Inversion,
//   0x03 Configuration (1 = entree, 0 = sortie -- inverse du bon sens)
#define I2C_ADDR_TCA9554   0x20
#define REG_OUTPUT         0x01
#define REG_CONFIG         0x03

// Bits confirmes par le repo officiel V2 (09_LVGL_Test.ino,
// example_exio_init/example_exio_reset) :
#define TCA_GPIO_0   0  // EXIO_OLED_RESET
#define TCA_GPIO_1   1  // EXIO_TP_RESET
#define TCA_GPIO_5   5  // configure en sortie par la demo V2, role exact non documente

static uint8_t exioState = 0x00; // reflete l'etat courant du registre de sortie

static void exioWriteOutput()
{
  I2C_write_buff(I2C_ADDR_TCA9554, REG_OUTPUT, &exioState, 1);
}

static void exioSetState(uint8_t pin, uint8_t value)
{
  if (value) exioState |= (uint8_t)(1u << pin);
  else       exioState &= (uint8_t)~(1u << pin);
  exioWriteOutput();
}

void expanderInit()
{
  uint8_t outputMask = (uint8_t)((1u << TCA_GPIO_0) | (1u << TCA_GPIO_1) | (1u << TCA_GPIO_5));
  exioWriteOutput(); // etat initial (tout a 0) avant de configurer la direction
  uint8_t config = (uint8_t)(~outputMask); // 1=entree pour tout sauf les 3 bits ci-dessus (0=sortie)
  I2C_write_buff(I2C_ADDR_TCA9554, REG_CONFIG, &config, 1);
}

static void exioResetPulse(uint8_t pin)
{
  exioSetState(pin, 1);
  delay(20);
  exioSetState(pin, 0);
  delay(20);
  exioSetState(pin, 1);
  delay(120);
}

void expanderResetOled()
{
  exioResetPulse(TCA_GPIO_0);
}

void expanderResetTouch()
{
  exioResetPulse(TCA_GPIO_1);
}
