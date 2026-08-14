#include <stdio.h>
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
void adc_bsp_init(void)
{
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
    // Facteur pont diviseur repris du 1.91 (x2) -- NON VERIFIE sur le
    // 2.41, le schema n'a pas ete consulte. A calibrer au banc : tape
    // 'b' sur le port serie (affiche tension/pourcentage) et compare a
    // une mesure au multimetre sur le connecteur batterie MX1.25, puis
    // ajuste ce facteur si l'ecart depasse quelques %.
    *value = 0.001 * vol * 2;
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
