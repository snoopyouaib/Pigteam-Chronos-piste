#ifndef SPLASH_PIGTEAM_H
#define SPLASH_PIGTEAM_H

#include "lvgl.h"

// Image splash PigTeam, 536x240, convertie depuis le format Piskel/
// ARGB8888 d'origine (splash_amoled_pigteam.c fourni) vers RGB565
// natif LVGL. Utilisation : lv_img_set_src(img_obj, &splash_pigteam_img);
extern const lv_img_dsc_t splash_pigteam_img;

#endif
