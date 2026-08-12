#include <Arduino.h>
#include <lvgl.h>

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "display_bsp.h"

// ===================== Test diagnostic couleurs : rectangles pleins =====================
//
// Objectif : isoler si le probleme de couleur vient du melange
// anti-crenelage des polices (bpp4) ou de la couleur brute elle-meme.
// Ici, AUCUN texte, juste des blocs de couleur unie -- si ces blocs
// s'affichent correctement (gris/vert/rouge/orange nets, sans "bavure"
// ni blanc residuel), le probleme est confirme cote police/anti-
// aliasing. Si les blocs sont eux-memes corrompus, le probleme est
// plus bas niveau (couleur/transport).

void setup() {
  Serial.begin(115200);
  delay(1000);

  I2C_master_Init();
  Touch_Init();
  displayInit();

  if (lvglLock(-1)) {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    const int w = 120, h = 50, gap = 10;
    lv_color_t colors[4] = {
      lv_palette_main(LV_PALETTE_GREY),
      lv_palette_main(LV_PALETTE_GREEN),
      lv_palette_main(LV_PALETTE_ORANGE),
      lv_palette_main(LV_PALETTE_RED),
    };
    const char* names[4] = {"GREY", "GREEN", "ORANGE", "RED"};

    for (int i = 0; i < 4; i++) {
      lv_obj_t* rect = lv_obj_create(scr);
      lv_obj_remove_style_all(rect);
      lv_obj_set_size(rect, w, h);
      lv_obj_set_style_bg_color(rect, colors[i], 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
      lv_obj_set_pos(rect, 4, 4 + i * (h + gap));

      Serial.printf("Rect %s: 0x%04X\n", names[i], lv_color_to16(colors[i]));
    }

    lvglUnlock();
  }

  Serial.println("Test rectangles pleins affiche.");
}

void loop() {
}
