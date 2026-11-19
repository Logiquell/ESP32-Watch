#include "screen.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "screen";

#define BUTTON_GPIO      GPIO_NUM_0   /* BOOT button, active low */
#define POLL_MS          100
#define FADE_OUT_MS      300
#define FADE_IN_MS       120

/* While the screen is dark, LVGL barely needs to run: redraw rarely, check touch at 10 Hz. */
#define DARK_REFR_MS     1000
#define DARK_TOUCH_MS    100

static bool s_awake = true;
static bool s_btn_was_down;
static void (*s_on_wake)(void);
static lv_anim_t s_fade;
static int s_fade_var; /* lv_anim needs a non-NULL target */

/* Power management: while the screen is on we block light sleep; while it's dark we allow it.
   CPU speed is left to ESP-IDF: it runs at 240 MHz whenever a task is busy (drawing, flip
   animation) and drops to 40 MHz when idle. (Holding 240 MHz for the whole time the screen
   was on made screen-on time the biggest battery cost.) */
static esp_pm_lock_handle_t s_lock_nosleep;
static uint32_t s_refr_period_awake;
static uint32_t s_touch_period_awake;

bool screen_is_awake(void) { return s_awake; }

/* ---- AMOLED panel sleep -------------------------------------------------------------
 * Brightness 0 only darkens the pixels; the panel's driver chip keeps running and draws
 * real current. While the screen is off we send DISPLAY OFF + SLEEP IN, and SLEEP OUT +
 * DISPLAY ON before lighting it again. The panel keeps its frame memory while asleep.
 *
 * The BSP doesn't expose the panel handles, but esp_lvgl_port stores them at the start of
 * the display's driver data. We mirror just those leading fields (esp_lvgl_port 2.x,
 * src/lvgl9/esp_lvgl_port_disp.c, lvgl_port_display_ctx_t).
 */
typedef struct {
    int disp_type;                      /* lvgl_port_disp_type_t */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} port_disp_head_t;

#define LCD_OPCODE_WRITE_CMD 0x02       /* QSPI framing used by the SH8601/CO5300 driver */
#define LCD_CMD_SLPIN_       0x10
#define LCD_CMD_SLPOUT_      0x11
#define SLEEP_SETTLE_MS      120        /* panel needs 120 ms after SLEEP IN/OUT */

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static int64_t s_panel_slept_at_us;
static bool s_panel_asleep;

static void panel_handles_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    const port_disp_head_t *h = disp ? lv_display_get_driver_data(disp) : NULL;
    if (h && h->disp_type >= 0 && h->disp_type < 8 && h->io_handle && h->panel_handle) {
        s_panel_io = h->io_handle;
        s_panel = h->panel_handle;
    } else {
        ESP_LOGW(TAG, "panel handles not found: panel will not be put to sleep");
    }
}

static void panel_cmd(uint8_t cmd)
{
    esp_lcd_panel_io_tx_param(s_panel_io, (LCD_OPCODE_WRITE_CMD << 24) | (cmd << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    if (!s_panel_io || sleep == s_panel_asleep) {
        return;
    }
    if (sleep) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        panel_cmd(LCD_CMD_SLPIN_);
        s_panel_slept_at_us = esp_timer_get_time();
    } else {
        /* SLEEP OUT must not follow SLEEP IN too closely */
        int64_t since_ms = (esp_timer_get_time() - s_panel_slept_at_us) / 1000;
        if (since_ms < SLEEP_SETTLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(SLEEP_SETTLE_MS - since_ms));
        }
        panel_cmd(LCD_CMD_SLPOUT_);
        vTaskDelay(pdMS_TO_TICKS(SLEEP_SETTLE_MS));
        esp_lcd_panel_disp_on_off(s_panel, true);
    }
    s_panel_asleep = sleep;
}

static void pm_locks(bool hold)
{
    if (!s_lock_nosleep) {
        return; /* power management not available */
    }
    if (hold) {
        esp_pm_lock_acquire(s_lock_nosleep);
    } else {
        esp_pm_lock_release(s_lock_nosleep);
    }
}

static void lvgl_timers_dark(bool dark)
{
    lv_timer_t *refr = lv_display_get_refr_timer(lv_display_get_default());
    lv_indev_t *indev = bsp_display_get_input_dev();
    lv_timer_t *touch = indev ? lv_indev_get_read_timer(indev) : NULL;
    if (refr) {
        lv_timer_set_period(refr, dark ? DARK_REFR_MS : s_refr_period_awake);
    }
    if (touch) {
        lv_timer_set_period(touch, dark ? DARK_TOUCH_MS : s_touch_period_awake);
    }
}

static void set_brightness(void *var, int32_t pct)
{
    (void)var;
    bsp_display_brightness_set((int)pct);
}

/* Fade-out finished: now it's safe to slow LVGL down and let the chip sleep. */
static void fade_out_done(lv_anim_t *a)
{
    (void)a;
    if (!s_awake) {
        panel_sleep(true);
        lvgl_timers_dark(true);
        pm_locks(false);
    }
}

static void fade_to(int from, int to, uint32_t ms, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&s_fade_var, set_brightness);
    lv_anim_init(&s_fade);
    lv_anim_set_var(&s_fade, &s_fade_var);
    lv_anim_set_exec_cb(&s_fade, set_brightness);
    lv_anim_set_values(&s_fade, from, to);
    lv_anim_set_duration(&s_fade, ms);
    lv_anim_set_completed_cb(&s_fade, done);
    lv_anim_start(&s_fade);
}

static void go_to_sleep(void)
{
    if (!s_awake) {
        return;
    }
    s_awake = false;
    ESP_LOGI(TAG, "sleep");
    fade_to(CONFIG_WATCH_BRIGHTNESS, 0, FADE_OUT_MS, fade_out_done);
}

static void wake_up(void)
{
    if (s_awake) {
        return;
    }
    bool was_dark = lv_anim_get(&s_fade_var, set_brightness) == NULL; /* fade-out completed */
    s_awake = true;
    if (was_dark) {
        pm_locks(true);
        lvgl_timers_dark(false);
        panel_sleep(false);
    }
    ESP_LOGI(TAG, "wake");
    lv_display_trigger_activity(NULL); /* restart the idle countdown */
    if (s_on_wake) {
        s_on_wake(); /* refresh content before it becomes visible */
    }
    lv_obj_invalidate(lv_screen_active()); /* full redraw after sleep, in case the panel lost anything */
    fade_to(0, CONFIG_WATCH_BRIGHTNESS, FADE_IN_MS, NULL);
}

/* Runs in the LVGL task. */
static void poll(lv_timer_t *timer)
{
    (void)timer;

    /* BOOT button: press toggles sleep / wake */
    bool down = gpio_get_level(BUTTON_GPIO) == 0;
    if (down && !s_btn_was_down) {
        if (s_awake) {
            go_to_sleep();
        } else {
            wake_up();
        }
    }
    s_btn_was_down = down;

    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    if (s_awake) {
        if (idle_ms > CONFIG_WATCH_SCREEN_TIMEOUT_S * 1000U) {
            go_to_sleep();
        }
    } else if (idle_ms < 2 * POLL_MS) {
        wake_up(); /* touch while asleep */
    }
}

static void pm_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management unavailable (%s)", esp_err_to_name(err));
        return;
    }
    /*
     * Tickless idle on ESP32-S3 isolates all GPIOs during light sleep (errata workaround).
     * Keep these connected:
     *  - display/touch control outputs, so the panel isn't reset or clocked by a floating line
     *  - touch interrupt (driven by the touch chip) and the shared I2C bus, so sleep entry/exit
     *    can't create false touch-interrupt edges or glitch an I2C transfer
     */
    const gpio_num_t keep[] = {BSP_LCD_RST, BSP_LCD_TOUCH_RST, BSP_LCD_PCLK, BSP_LCD_CS,
                               BSP_LCD_TOUCH_INT, BSP_I2C_SDA, BSP_I2C_SCL};
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        gpio_sleep_sel_dis(keep[i]);
    }

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "screen_on", &s_lock_nosleep);
    pm_locks(true); /* screen starts on */
    ESP_LOGI(TAG, "light sleep enabled while screen is off");
#endif
}

void screen_init(void (*on_wake)(void))
{
    s_on_wake = on_wake;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    /* LVGL 9 has no period getter; both timers default to LV_DEF_REFR_PERIOD. */
    s_refr_period_awake = LV_DEF_REFR_PERIOD;
    s_touch_period_awake = LV_DEF_REFR_PERIOD;

    pm_init();
    panel_handles_init();

    bsp_display_brightness_set(CONFIG_WATCH_BRIGHTNESS);
    lv_timer_create(poll, POLL_MS, NULL);
}
