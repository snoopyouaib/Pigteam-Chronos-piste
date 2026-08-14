#include <stdio.h>
#include "i2c_bsp.h"

#define TEST_I2C_PORT I2C_NUM_0

// Broches confirmees au bring-up du 1.91 (GPIO39/40) -- NE VALENT PAS
// pour le 2.41 : d'apres le pinout Waveshare du 2.41, TP_SDA/IMU_SDA/
// RTC_SDA et TP_SCL/IMU_SCL/RTC_SCL sont regroupes sur GPIO47/48 (bus
// partage tactile+IMU+RTC+expandeur EXIO, comme sur le 1.91 mais sur
// des broches differentes). A confirmer au banc.
#define I2C_MASTER_SCL_IO 48
#define I2C_MASTER_SDA_IO 47

void I2C_master_Init(void)
{
  i2c_config_t conf =
  {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = I2C_MASTER_SDA_IO,
    .scl_io_num = I2C_MASTER_SCL_IO,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master = {
      .clk_speed = 100 * 1000, // teste a 100kHz au lieu de 300kHz -- le FT3168 semble sensible a la vitesse (cf. piege glissement tactile)
    },
    .clk_flags = 0,
  };
  ESP_ERROR_CHECK(i2c_param_config(TEST_I2C_PORT, &conf));
  ESP_ERROR_CHECK(i2c_driver_install(TEST_I2C_PORT, conf.mode, 0, 0, 0));
}

uint8_t I2C_write_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  uint8_t *pbuf = (uint8_t*)malloc(len+1);
  pbuf[0] = reg;
  for(uint8_t i = 0; i<len; i++)
  {
    pbuf[i+1] = buf[i];
  }
  ret = i2c_master_write_to_device(TEST_I2C_PORT,addr,pbuf,len+1,1000);
  free(pbuf);
  pbuf = NULL;
  return ret;
}

uint8_t I2C_read_buff(uint8_t addr,uint8_t reg,uint8_t *buf,uint8_t len)
{
  uint8_t ret;
  ret = i2c_master_write_read_device(TEST_I2C_PORT,addr,&reg,1,buf,len,1000);
  return ret;
}

uint8_t I2C_master_write_read_device(uint8_t addr,uint8_t *writeBuf,uint8_t writeLen,uint8_t *readBuf,uint8_t readLen)
{
  uint8_t ret;
  ret = i2c_master_write_read_device(TEST_I2C_PORT,addr,writeBuf,writeLen,readBuf,readLen,1000);
  return ret;
}

void I2C_scan(void)
{
  printf("[i2c_scan] debut (GPIO SDA=%d SCL=%d)\n", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
  int found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TEST_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) {
      printf("[i2c_scan] trouve : 0x%02X\n", addr);
      found++;
    }
  }
  printf("[i2c_scan] fin -- %d peripherique(s) trouve(s)\n", found);
}
