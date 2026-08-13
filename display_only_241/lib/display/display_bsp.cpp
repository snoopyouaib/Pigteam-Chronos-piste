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

// ===================== Ecran AMOLED 2.41 (RM690B0) + LVGL =====================
//
// Reecrit le 12/08 pour coller EXACTEMENT a la demo officielle Waveshare
// (09_LVGL_Test.ino, fournie par l'utilisateur -- fichiers esp_lcd_sh8601.c/h,
// esp_lcd_touch_ft5x06.c/h). Nos tentatives precedentes (table d'init
// "generique" inventee, CASET/RASET pleine echelle, MADCTL=0x00, activation
// d'un IO-expander TCA9554 pour AMOLED_EN) etaient TOUTES fausses par
// rapport a ce que fait reellement Waveshare pour ce board precis :
//   - Aucun TCA9554/EXIO n'est implique dans l'alimentation de l'ecran sur
//     le 2.41 (contrairement a d'autres boards Waveshare AMOLED comme le
//     1.75/1.8/1.43 qui ont un IO-expander) -- fausse piste suivie a tort
//     par analogie avec ces autres boards.
//   - Le panneau physique est nativement 450 (large) x 600 (haut) en
//     portrait ; la table d'init officielle adresse CASET sur 16..465
//     (450 pixels avec un offset materiel de 16, PAS 0..599) et RASET sur
//     0..599 pleine echelle. Le mode paysage 600x450 qu'on veut est obtenu
//     par MADCTL=0x30 (bit MV, echange ligne/colonne cote controleur) --
//     le flush_cb doit alors ajouter ce meme offset de +16 sur l'axe qui
//     devient "CASET" apres l'echange (l'axe Y de LVGL ici).
//   - Sequence de deverrouillage vendor (0xFE page-select) avant les
//     commandes normales -- absente de nos tentatives precedentes.
//   - AUCUN swap manuel d'octets dans le flush_cb officiel (contrairement
//     au flush_cb herite du 1.91, qui en faisait un pour un probleme
//     specifique a CE montage -- pas necessairement applicable ici).

static const char *TAG = "display_bsp";
static SemaphoreHandle_t lvgl_mux = NULL;

#define LCD_HOST    SPI2_HOST

#define LCD_BIT_PER_PIXEL       (16)

// Pinout confirme par la demo officielle Waveshare (identique a ce qu'on
// avait deja via le wiki -- CS9/CLK10/D0-D3=11-14/RST21).
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
// V2 (12/08) : GPIO21 n'est PLUS le reset de l'ecran sur cette revision
// materielle (etiquette "V2") -- le reset passe par le TCA9554
// (cf. lib/expander/expander_bsp.h, expanderResetOled()), appele
// explicitement dans main.cpp AVANT displayInit(). -1 = pas de GPIO
// de reset direct, le driver esp_lcd_new_panel_rm690b0 ne pilotera
// donc aucune broche ici (son propre reset logiciel via commande
// serait de toute facon court-circuite par le vrai reset materiel
// deja effectue en amont).
#define EXAMPLE_PIN_NUM_LCD_RST           (-1)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (-1)

// Mode paysage (AMOLED_Rotate == Rotate_90 dans la demo officielle) --
// c'est le mode qu'on veut, coherent avec le reste du firmware PigTeam.
#define EXAMPLE_LCD_H_RES              600
#define EXAMPLE_LCD_V_RES              450

// V_RES/10 (comme la demo officielle), PAS /4 comme on avait mis --
// buffers assez petits (600*45*2 = 54000o) pour tenir en SRAM interne
// DMA-capable (MALLOC_CAP_DMA), pas besoin de PSRAM ni des complications
// d'alignement qui allaient avec.
#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES/10)
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

// Table d'init reprise a l'identique de la demo officielle Waveshare
// (09_LVGL_Test.ino, cas AMOLED_Rotate == Rotate_90).
static const rm690b0_lcd_init_cmd_t lcd_init_cmds[] = {
  {0xFE, (uint8_t []){0x20}, 1, 0},
  {0x26, (uint8_t []){0x0A}, 1, 0},
  {0x24, (uint8_t []){0x80}, 1, 0},

  {0xFE, (uint8_t []){0x00}, 1, 0},
  {0x3A, (uint8_t []){0x55}, 1, 0},
  {0xC2, (uint8_t []){0x00}, 1, 10},
  {0x35, (uint8_t []){0x00}, 0, 0},
  {0x51, (uint8_t []){0x00}, 1, 10},
  {0x11, (uint8_t []){0x00}, 0, 80},
  {0x2A, (uint8_t []){0x00,0x10,0x00,0xD1}, 4, 0}, // valeurs V2 exactes (differentes du V1)
  {0x2B, (uint8_t []){0x00,0x00,0x00,0x57}, 4, 0}, // idem
  {0x29, (uint8_t []){0x00}, 0, 10},
  {0x36, (uint8_t []){0x30}, 1, 0}, // MADCTL -- mode paysage (bit MV)
  {0x51, (uint8_t []){0xFF}, 1, 0}, // luminosite max
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
  // +16 sur Y -- meme offset materiel que celui adresse par CASET
  // (16..465) dans la table d'init, cf. commentaire en tete de fichier.
  const int offsetx1 = area->x1;
  const int offsetx2 = area->x2;
  const int offsety1 = area->y1 + 16;
  const int offsety2 = area->y2 + 16;

  // Swap manuel des octets de chaque pixel -- meme constat que sur le
  // 1.91 (cf. chrono-AMOLED/lib/display/display_bsp.cpp) : ni
  // LV_COLOR_16_SWAP (LVGL) ni le bit BGR du MADCTL n'ont d'effet
  // observable sur ce type de montage QSPI AMOLED, le swap semble se
  // produire plus bas, cote transport ESP-IDF (esp_lcd_panel_io_spi),
  // independamment de ces deux reglages. On le force ici, au seul
  // endroit qu'on controle totalement avant l'envoi reel a l'ecran.
  uint16_t* buf16 = (uint16_t*)color_map;
  uint32_t pixelCount = lv_area_get_width(area) * lv_area_get_height(area);
  for (uint32_t i = 0; i < pixelCount; i++) {
    buf16[i] = (uint16_t)((buf16[i] >> 8) | (buf16[i] << 8));
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

  // NOTE : pas de Touch_Init() ici -- deja appele dans setup() avant
  // displayInit(), une fois le bus I2C partage monte par I2C_master_Init().

  ESP_LOGI(TAG, "Initialize LVGL library");
  lv_init();
  // Buffers assez petits (V_RES/10) pour tenir en SRAM interne DMA-capable
  // -- retour a MALLOC_CAP_DMA comme la demo officielle, plus besoin de
  // PSRAM pour ca (l'assert buf2 qu'on avait eu venait d'un buffer /4
  // bien plus gros que necessaire).
  lv_color_t *buf1 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1);
  lv_color_t *buf2 = (lv_color_t*)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
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
