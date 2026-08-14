#include <stdio.h>
#include "touch_bsp.h"
#include "i2c_bsp.h"

// ===================== Tactile FT6336 / FT5x06 (2.41) =====================
//
// Adresse I2C et carte de registres confirmees par la demo officielle
// Waveshare (esp_lcd_touch_ft5x06.c/h, fournis par l'utilisateur) :
// 0x38, registres XH/XL/YH/YL a partir de 0x03 -- exactement ce qu'on
// avait deja. Ce qui manquait : la transformation mirror_y+swap_xy que
// la demo officielle applique (config x_max=V_RES-1, y_max=H_RES-1,
// flags.swap_xy=1, flags.mirror_y=1 pour le mode paysage) -- le panneau
// tactile est monte nativement en 450x600 portrait, independamment de
// la rotation logicielle 600x450 qu'on veut cote LVGL.
//
// Meme piege que sur le 1.91 (cf. README_AMOLED_bringup.md, piege #5) :
// Touch_Init() ne fait PAS sa propre init de bus I2C, deja faite par
// I2C_master_Init() (lib i2c_shared) AVANT Touch_Init() dans setup().

extern unsigned long millis(void); // evite d'inclure Arduino.h (C++) dans ce fichier .c

#define I2C_ADDR_FT6336 0x38
#define EXAMPLE_LCD_H_RES  600
#define EXAMPLE_LCD_V_RES  450

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
  uint8_t ret = I2C_write_buff(I2C_ADDR_FT6336, 0x00, &data, 1); // bascule en mode normal
  printf("[touch] FT6336 0x38 ret=%d (0=ACK recu, sinon rien a cette adresse)\n", ret);
}

uint8_t getTouch(uint16_t *x,uint16_t *y)
{
  uint8_t data;
  uint8_t buf[4];
  I2C_read_buff(I2C_ADDR_FT6336,0x02,&data,1);
  if(data)
  {
    I2C_read_buff(I2C_ADDR_FT6336,0x03,buf,4);
    // Registres FT5x06/FT6336 : XH/XL puis YH/YL (ordre natif du panneau,
    // physiquement monte en 450(large)x600(haut) portrait, independant
    // de la rotation logicielle de l'ecran).
    uint16_t raw_x = (((uint16_t)buf[0] & 0x0f) << 8) | (uint16_t)buf[1];
    uint16_t raw_y = (((uint16_t)buf[2] & 0x0f) << 8) | (uint16_t)buf[3];
    if (raw_x > (EXAMPLE_LCD_V_RES - 1)) raw_x = EXAMPLE_LCD_V_RES - 1;
    if (raw_y > (EXAMPLE_LCD_H_RES - 1)) raw_y = EXAMPLE_LCD_H_RES - 1;

    // Meme transform que la config officielle Waveshare pour le mode
    // paysage (swap_xy=1, mirror_y=1, x_max=V_RES-1, y_max=H_RES-1,
    // cf. esp_lcd_touch_get_coordinates() dans esp_lcd_touch.c : mirror
    // d'abord, puis swap) : final_x = (H_RES-1) - raw_y ; final_y = raw_x.
    *x = (EXAMPLE_LCD_H_RES - 1) - raw_y;
    *y = raw_x;

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
