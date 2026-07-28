#include <Arduino.h>
#include "sd_card_bsp.h"

void setup() {
  Serial.begin(115200);
  delay(5000); // délai plus long que sur le 1.64 (5s au lieu de 3s), repris tel quel du demo Waveshare
  SD_card_Init();
}

void loop() {
}
