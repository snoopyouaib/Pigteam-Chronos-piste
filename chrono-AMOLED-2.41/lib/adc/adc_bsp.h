#ifndef ADC_BSP_H
#define ADC_BSP_H

void adc_bsp_init(void);
void adc_get_value(float *value,int *data);
void BAT_GPIO_Init(void);
void BAT_ON(void);
void BAT_OFF(void);
#endif