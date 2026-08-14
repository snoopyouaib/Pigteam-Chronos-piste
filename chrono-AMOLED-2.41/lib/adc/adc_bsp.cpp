#include <stdio.h>
#include <Arduino.h>
#include "adc_bsp.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 2.41 : la broche batterie n'est PAS la meme que sur le 1.91.
// Pinout officiel Waveshare ESP32-S3-Touch-AMOLED-2.41 :
//   BAT_ADC = GPIO17 = ADC2_CHANNEL_6 (le 1.91 utilisait ADC1_CHANNEL_0
//   / GPIO1, qui sur le 2.41 est une broche libre non connectee a la
//   batterie -- d'ou le "NO BAT" permanent avant ce correctif, meme
//   sur USB : la lecture flottante tombait sous le seuil bas de la
//   courbe batteryVoltageToPercent()).
//
// A SURVEILLER : ADC2 partage historiquement une ressource materielle
// avec le WiFi sur les puces ESP32 -- sur S3 la restriction est censee
// etre levee, mais webServerManager.begin() lance un point d'acces en
// continu dans ce firmware. Si les lectures deviennent erratiques une
// fois le WiFi actif (a l'usage reel, AP toujours actif), ce sera le
// premier suspect.
#define ADC_Calibrate         //en calibrate

#ifdef ADC_Calibrate
  static adc_cali_handle_t cali_handle;
#endif
  static adc_oneshot_unit_handle_t adc2_handle;
// GPIO16 = BAT_Control : commande le circuit d'alimentation batterie
// (documente par Waveshare -- BAT_GPIO_Init/BAT_ON/BAT_OFF). Sans
// l'activer explicitement (BAT_ON), le rail batterie/diviseur lu par
// l'ADC reste deconnecte meme avec une vraie batterie branchee -- sur
// USB seul, ADC lit une entree flottante proche de 0V -> "NO BAT"
// meme avec batterie physiquement presente. A appeler une fois dans
// adc_bsp_init(), avant toute lecture.
void BAT_GPIO_Init(void) {
  pinMode(16, OUTPUT);
}
void BAT_ON(void) {
  digitalWrite(16, HIGH);
}
void BAT_OFF(void) {
  digitalWrite(16, LOW);
}

void adc_bsp_init(void)
{
  // BAT_GPIO_Init()/BAT_ON() sont deja appeles tout au debut de
  // setup() (main.cpp) pour le maintien d'alimentation -- pas la
  // peine de les rappeler ici, digitalWrite(16, HIGH) est deja actif.
#ifdef ADC_Calibrate
  adc_cali_curve_fitting_config_t cali_config =
  {
    .unit_id = ADC_UNIT_2,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12, //4096
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));
#endif
  adc_oneshot_unit_init_cfg_t init_config2 = {
    .unit_id = ADC_UNIT_2, //ADC2 -- BAT_ADC (GPIO17) sur le 2.41
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config2, &adc2_handle));
  adc_oneshot_chan_cfg_t config = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc2_handle, ADC_CHANNEL_6, &config));
}
void adc_get_value(float *value,int *data)
{
  int adcdata;
#ifdef ADC_Calibrate
  int vol = 0;
#endif
  esp_err_t err;
  err = adc_oneshot_read(adc2_handle,ADC_CHANNEL_6,&adcdata);
  if(err == ESP_OK)
  {
#ifdef ADC_Calibrate
    adc_cali_raw_to_voltage(cali_handle,adcdata,&vol);
    // Facteur du pont diviseur : x2.94. Deux points de calibration au
    // multimetre donnent des facteurs legerement differents (2.90 pour
    // 3.85-3.94V, 2.98 pour 4.15V) -- non-linearite reelle de l'ADC,
    // pas juste du bruit de mesure. x2.94 = moyenne des deux, choisie
    // pour bien coller pres de la pleine charge (partie la plus raide
    // de la courbe batteryVoltageToPercent() : 15 points de % pour
    // seulement 0.15V d'ecart entre 4.0 et 4.15V, donc un petit ecart
    // de tension y devient un gros ecart de % affiche).
    *value = 0.001 * vol * 2.94;
#else
    *value = ((float)adcdata * 3.3/4096) * 2;
#endif
    *data = adcdata;
  }
  else
  {
    *value = 0;
    *data = 0;
  }
}
