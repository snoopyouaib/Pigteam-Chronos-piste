#include <Arduino.h>
#include <lvgl.h>

#include "i2c_bsp.h"
#include "touch_bsp.h"
#include "display_bsp.h"
#include "fonts_teko.h"

// ===================== Test diagnostic tactile =====================
//
// Objectif : confirmer que le tactile repond correctement et que les
// coordonnees sont coherentes avec l'orientation reelle de l'ecran
// (536x240 paysage) avant de decider comment l'integrer a la
// navigation (zones tactiles, boutons, swipe...).
//
// Affiche un petit marqueur a l'endroit touche + les coordonnees en
// gros au centre, et log aussi sur le port serie. Touche les 4 coins
// et le centre de l'ecran pour verifier que les valeurs sont
// coherentes avec ce que tu attends (0,0 en haut a gauche, 536x240 en
// bas a droite).

static lv_obj_t* lblCoords;
static lv_obj_t* marker;

static void update_touch_ui(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  const char* codeName = "?";
  switch (code) {
    case LV_EVENT_PRESSED:     codeName = "PRESSED"; break;
    case LV_EVENT_PRESSING:    codeName = "PRESSING"; break;
    case LV_EVENT_RELEASED:    codeName = "RELEASED"; break;
    case LV_EVENT_PRESS_LOST:  codeName = "PRESS_LOST"; break;
    case LV_EVENT_CLICKED:     codeName = "CLICKED"; break;
    default: break;
  }

  lv_indev_t* indev = lv_indev_get_act();
  if (indev) {
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    Serial.printf("[EVENT %s] x=%d y=%d  t=%lu\n", codeName, p.x, p.y, millis());

    char buf[32];
    snprintf(buf, sizeof(buf), "x=%d  y=%d", p.x, p.y);
    lv_label_set_text(lblCoords, buf);
    lv_obj_align(lblCoords, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_pos(marker, p.x - 10, p.y - 10);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
  } else {
    Serial.printf("[EVENT %s] (pas d'indev actif)  t=%lu\n", codeName, millis());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  I2C_master_Init();
  Touch_Init();
  displayInit();

  if (lvglLock(-1)) {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_teko_medium_26, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Touche l'ecran (coins + centre)");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lblCoords = lv_label_create(scr);
    lv_obj_set_style_text_font(lblCoords, &lv_font_teko_bold_38, 0);
    lv_obj_set_style_text_color(lblCoords, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_label_set_text(lblCoords, "x=--  y=--");
    lv_obj_align(lblCoords, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_clear_flag(lblCoords, LV_OBJ_FLAG_CLICKABLE);

    // Marqueur : petit carre rouge qui saute a l'endroit touche
    marker = lv_obj_create(scr);
    lv_obj_remove_style_all(marker);
    lv_obj_set_size(marker, 20, 20);
    lv_obj_set_style_bg_color(marker, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(marker, 10, 0);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_CLICKABLE); // sinon il vole le point touche des qu'il apparait dessus (cf. piege PRESS_LOST)
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);

    // LV_EVENT_PRESSING = declenche en continu pendant que le doigt reste pose
    // (pas seulement au premier contact) -- utile pour voir si le tactile
    // suit bien un mouvement, pas juste un tap ponctuel.
    lv_obj_add_event_cb(scr, update_touch_ui, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, update_touch_ui, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, update_touch_ui, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, update_touch_ui, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(scr, update_touch_ui, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE); // sans ca, un glissement est interprete comme un defilement, pas un suivi de position

    lvglUnlock();
  }

  Serial.println("Test tactile pret -- touche l'ecran.");
}

void loop() {
}
