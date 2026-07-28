#ifndef DISPLAY_BSP_H
#define DISPLAY_BSP_H

// ===================== Ecran AMOLED 1.91 (SH8601/RM67162) + LVGL =====================
//
// Extrait du 03_LVGL_V8_Test.ino Waveshare (tout etait dans le .ino
// d'origine, pas de lcd_bsp.c separe comme sur le 1.64) -- restructure
// en fonctions appelables depuis un firmware unifie :
//
// - displayInit() : initialise le bus QSPI, le panel SH8601, LVGL, le
//   timer de tick et la tache de rendu. A appeler UNE FOIS dans
//   setup(), APRES Touch_Init() (le tactile doit deja repondre sur le
//   bus I2C avant que LVGL n'enregistre son indev de lecture tactile).
// - lvglLock()/lvglUnlock() : a utiliser autour de tout appel a l'API
//   LVGL fait depuis en dehors de la tache LVGL elle-meme (ex: mise a
//   jour d'un label depuis loop()), les fonctions LVGL ne sont pas
//   thread-safe.
//
// Ne fait PAS lv_demo_widgets() -- contrairement au test de bring-up,
// c'est au code applicatif (main.cpp / futurs ecrans) de construire sa
// propre UI apres l'appel a displayInit().

void displayInit();
bool lvglLock(int timeout_ms);
void lvglUnlock();

#endif
