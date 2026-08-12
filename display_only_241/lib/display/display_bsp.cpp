#include <Arduino.h>
#include "display_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lcd_rm690b0.h"
#include "touch_bsp.h"

static const char *TAG = "display_bsp";
static SemaphoreHandle_t lvgl_mux = NULL;

#define LCD_HOST    SPI2_HOST

#define LCD_BIT_PER_PIXEL       (16)

// Pinout QSPI-Touch-AMOLED-2.41 (wiki Waveshare, verifie 12/08) --
// AUCUN de ces GPIO ne correspond a ceux du 1.91 : c'est un bus QSPI
// different (CS9/CLK10/D0-D3=11-14/RST21), pas juste un remap partiel.
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (-1)

#define EXAMPLE_LCD_H_RES              600
#define EXAMPLE_LCD_V_RES              450

#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES/4)
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

// Table d'init RM690B0 -- PREMIERE PASSE A VALIDER AU BANC, pas encore
// testee sur le vrai 2.41. Reprend le squelette de commandes DCS
// partagees par toute cette famille QSPI AMOLED (le driver lui-meme
// est generique, cf. esp_lcd_rm690b0.h) :
//   0x11 sleep-out, 0x36 MADCTL (orientation -- 0x00 par defaut ici,
//   0xF0 sur le 1.91 etait deja specifique a son montage physique,
//   donc a re-determiner empiriquement pour le 2.41), 0x3A COLMOD
//   (0x55 = RGB565), 0x2A/0x2B CASET/RASET bornes sur 600x450
//   (599=0x0257, 449=0x01C1) au lieu de 536x240, 0x51 luminosite,
//   0x29 display-on.
static const rm690b0_lcd_init_cmd_t lcd_init_cmds[] = {
  {0x11, (uint8_t []){0x00}, 0, 120},
  {0x36, (uint8_t []){0x00}, 1, 0},
  {0x3A, (uint8_t []){0x55}, 1, 0},  //16bits-RGB565
  {0x2A, (uint8_t []){0x00,0x00,0x02,0x57}, 4, 0},
  {0x2B, (uint8_t []){0x00,0x00,0x01,0xC1}, 4, 0},
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x51, (uint8_t []){0xFF}, 1, 0},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_driver);
  return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int offsetx1 = area->x1;
  const int offsetx2 = area->x2;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  // Swap manuel des octets de chaque pixel -- ni LV_COLOR_16_SWAP (LVGL)
  // ni le bit BGR du MADCTL n'ont d'effet observable sur ce montage, le
  // swap semble se produire plus bas, cote transport ESP-IDF
  // (esp_lcd_panel_io_spi), independamment de ces deux reglages. On le
  // force ici, au seul endroit qu'on controle totalement avant l'envoi
  // reel a l'ecran. Confirme par calcul : gris LVGL (0x9E9E9E) une fois
  // ses 2 octets inverses donne (240,112,224) -- un rose vif, exactement
  // ce qui etait observe.
  uint16_t* buf16 = (uint16_t*)color_map;
  uint32_t pixelCount = lv_area_get_width(area) * lv_area_get_height(area);
  for (uint32_t i = 0; i < pixelCount; i++) {
    buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
  }

  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void example_lvgl_update_cb(lv_disp_drv_t *drv)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  switch (drv->rotated)
  {
    case LV_DISP_ROT_NONE:
      esp_lcd_panel_swap_xy(panel_handle, false);
      esp_lcd_panel_mirror(panel_handle, true, false);
      break;
    case LV_DISP_ROT_90:
      esp_lcd_panel_swap_xy(panel_handle, true);
      esp_lcd_panel_mirror(panel_handle, true, true);
      break;
    case LV_DISP_ROT_180:
      esp_lcd_panel_swap_xy(panel_handle, false);
      esp_lcd_panel_mirror(panel_handle, false, true);
      break;
    case LV_DISP_ROT_270:
      esp_lcd_panel_swap_xy(panel_handle, true);
      esp_lcd_panel_mirror(panel_handle, false, false);
      break;
  }
}

static void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  uint16_t x1 = area->x1, x2 = area->x2;
  uint16_t y1 = area->y1, y2 = area->y2;
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint16_t tp_x, tp_y;
  uint8_t win = getTouch(&tp_x, &tp_y);
  if (win) {
    data->point.x = tp_x;
    data->point.y = tp_y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

bool lvglLock(int timeout_ms)
{
  assert(lvgl_mux && "displayInit() must be called first");
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvglUnlock()
{
  assert(lvgl_mux && "displayInit() must be called first");
  xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
  ESP_LOGI(TAG, "Starting LVGL task");
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  while (1) {
    if (lvglLock(-1)) {
      task_delay_ms = lv_timer_handler();
      lvglUnlock();
    }
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

void displayInit()
{
  static lv_disp_draw_buf_t disp_buf;
  static lv_disp_drv_t disp_drv;

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
  gpio_config_t bk_gpio_config = {
      .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT,
      .mode = GPIO_MODE_OUTPUT,
  };
  ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
#endif

  ESP_LOGI(TAG, "Initialize SPI bus");
  const spi_bus_config_t buscfg = RM690B0_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                               EXAMPLE_PIN_NUM_LCD_DATA0,
                                                               EXAMPLE_PIN_NUM_LCD_DATA1,
                                                               EXAMPLE_PIN_NUM_LCD_DATA2,
                                                               EXAMPLE_PIN_NUM_LCD_DATA3,
                                                               EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_panel_io_handle_t io_handle = NULL;
  const esp_lcd_panel_io_spi_config_t io_config = RM690B0_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                              example_notify_lvgl_flush_ready,
                                                                              &disp_drv);
  rm690b0_vendor_config_t vendor_config = {
      .init_cmds = lcd_init_cmds,
      .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
      .flags = { .use_qspi_interface = 1 },
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

  esp_lcd_panel_handle_t panel_handle = NULL;
  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = LCD_BIT_PER_PIXEL,
      .vendor_config = &vendor_config,
  };
  ESP_LOGI(TAG, "Install RM690B0 panel driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_rm690b0(io_handle, &panel_config, &panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // ===== DIAGNOSTIC TEMPORAIRE (12/08) =====
  // Remplissage plein ecran en brut, en appelant esp_lcd_panel_draw_bitmap
  // directement -- SANS passer par LVGL. But : isoler si la table d'init
  // RM690B0 + les pins QSPI font vraiment sortir des pixels sur l'ecran,
  // independamment de tout probleme cote LVGL (flush_cb, byte-swap,
  // buffers, etc.).
  //
  // v1 (echec) : un seul appel avec les 600x450x2 = 540000 octets d'un
  // coup -> "spi transmit (queue) color failed" x3 (une par couleur).
  // Transaction DMA unique trop grosse. v2 : decoupe en bandes de
  // EXAMPLE_LVGL_BUF_HEIGHT lignes (meme taille de chunk que le flush
  // LVGL normal, ~134 Ko -- taille dont on sait deja qu'elle passe,
  // aucune erreur SPI observee apres le message [3/5]).
  // A RETIRER une fois l'ecran confirme fonctionnel.
  {
    const int band_h = EXAMPLE_LVGL_BUF_HEIGHT;
    size_t band_bytes = (size_t)EXAMPLE_LCD_H_RES * band_h * (LCD_BIT_PER_PIXEL / 8);
    uint16_t *fill_buf = (uint16_t*)heap_caps_malloc(band_bytes, MALLOC_CAP_SPIRAM);
    if (fill_buf) {
      uint16_t colors[3] = {0xF800, 0x07E0, 0x001F}; // rouge, vert, bleu (RGB565)
      const char *names[3] = {"ROUGE", "VERT", "BLEU"};
      for (int c = 0; c < 3; c++) {
        for (size_t i = 0; i < band_bytes / 2; i++) fill_buf[i] = colors[c];
        ESP_LOGI(TAG, "[diag] remplissage plein ecran par bandes : %s", names[c]);
        for (int y = 0; y < EXAMPLE_LCD_V_RES; y += band_h) {
          int y2 = y + band_h;
          if (y2 > EXAMPLE_LCD_V_RES) y2 = EXAMPLE_LCD_V_RES;
          esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, EXAMPLE_LCD_H_RES, y2, fill_buf);
          if (err != ESP_OK) {
            ESP_LOGE(TAG, "[diag] echec draw_bitmap bande y=%d : %s", y, esp_err_to_name(err));
          }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
      }
      heap_caps_free(fill_buf);
    } else {
      ESP_LOGE(TAG, "[diag] echec alloc buffer de remplissage (PSRAM)");
    }
  }
  // ===== FIN DIAGNOSTIC TEMPORAIRE =====

  // NOTE : pas de Touch_Init() ici -- deja appele dans setup() avant
  // displayInit(), une fois le bus I2C partage monte par I2C_master_Init().

  ESP_LOGI(TAG, "Initialize LVGL library");
  lv_init();
  // MALLOC_CAP_DMA (comme sur le 1.91) ne pioche que dans la SRAM
  // interne, trop petite pour un buffer 600x450 (536x240 sur le 1.91
  // tenait, 600x450 non -- assert "buf2" au boot, RAM DMA interne
  // epuisee). Sur l'ESP32-S3, le DMA QSPI adresse directement la
  // PSRAM (contrairement a l'ESP32 classique) : on bascule les
  // buffers LVGL en MALLOC_CAP_SPIRAM, largement suffisant avec les
  // 8 Mo octal du 2.41 (memory_type = qio_opi dans platformio.ini).
  lv_color_t *buf1 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(buf1);
  lv_color_t *buf2 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(buf2);
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);

  ESP_LOGI(TAG, "Register display driver to LVGL");
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.drv_update_cb = example_lvgl_update_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &example_increase_lvgl_tick,
      .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.disp = disp;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  lvgl_mux = xSemaphoreCreateMutex();
  assert(lvgl_mux);
  xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
}
