#include <stdio.h>
#include "touch_bsp.h"
#include "i2c_bsp.h"

// ===================== Tactile FT3168 =====================
//
// Version allegee du touch_bsp.c d'origine Waveshare : celui-ci faisait
// son propre i2c_param_config()/i2c_driver_install() sur I2C_NUM_0 (le
// meme bus/port que l'IMU QMI8658), ce qui provoquerait un echec du
// second install() si on l'appelle apres I2C_master_Init() (deja fait
// pour l'IMU) -- cf. README_AMOLED_bringup.md, piege #5.
//
// Ici, Touch_Init() se contente de la commande specifique FT3168
// (bascule en mode normal) -- l'init du bus I2C lui-meme est deja faite
// une fois pour toutes par I2C_master_Init() (lib i2c_shared), appelee
// AVANT Touch_Init() dans setup().

extern unsigned long millis(void); // evite d'inclure Arduino.h (C++) dans ce fichier .c

#define I2C_ADDR_FT3168 0x38
#define EXAMPLE_LCD_H_RES  536
#define EXAMPLE_LCD_V_RES  240

// Fenetre de grace -- le registre "point touche" (0x02) semble se
// comporter comme un flash "nouvel echantillon" plutot qu'un etat
// "actuellement touche" en continu : lu depuis le pipeline LVGL (cycle
// fixe ~30ms), il retombe a 0 des le 2e cycle meme doigt immobile et
// toujours pose, alors qu'un sondage direct en boucle serree le voit
// rester a 1 en continu (constate au banc, cf. README piege). Plutot
// que de remonter un relachement premature a LVGL, on continue de
// signaler "touche" avec la derniere position connue pendant cette
// fenetre, et on ne declare un vrai relachement qu'apres son
// expiration sans nouvel echantillon.
#define TOUCH_GRACE_MS 120

static uint16_t lastX = 0, lastY = 0;
static unsigned long lastTouchMs = 0;
static uint8_t hadTouch = 0;

void Touch_Init(void)
{
  uint8_t data = 0x00;
  I2C_write_buff(I2C_ADDR_FT3168, 0x00, &data, 1); // bascule en mode normal
}

uint8_t getTouch(uint16_t *x,uint16_t *y)
{
  uint8_t data;
  uint8_t buf[4];
  I2C_read_buff(I2C_ADDR_FT3168,0x02,&data,1);
  if(data)
  {
    I2C_read_buff(I2C_ADDR_FT3168,0x03,buf,4);
    *y = (((uint16_t)buf[0] & 0x0f)<<8) | (uint16_t)buf[1];
    *x = (((uint16_t)buf[2] & 0x0f)<<8) | (uint16_t)buf[3];
    if(*x > EXAMPLE_LCD_H_RES)
    *x = EXAMPLE_LCD_H_RES;
    if(*y > EXAMPLE_LCD_V_RES)
    *y = EXAMPLE_LCD_V_RES;
    *y = EXAMPLE_LCD_V_RES - *y;

    lastX = *x;
    lastY = *y;
    lastTouchMs = millis();
    hadTouch = 1;
    return 1;
  }

  // Rien remonte ce cycle-ci -- si un echantillon valide date de moins
  // de TOUCH_GRACE_MS, on considere que le doigt est toujours pose.
  if (hadTouch && (millis() - lastTouchMs) < TOUCH_GRACE_MS) {
    *x = lastX;
    *y = lastY;
    return 1;
  }

  hadTouch = 0;
  return 0;
}
