#include "screen.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "screen";

#define BUTTON_GPIO      GPIO_NUM_0   /* BOOT button, active low */
#define POLL_MS          100
#define FADE_OUT_MS      300
#define FADE_IN_MS       120

static bool s_awake = true;
static bool s_btn_was_down;
static void (*s_on_wake)(void);
static lv_anim_t s_fade;
static int s_fade_var; /* lv_anim needs a non-NULL target */

bool screen_is_awake(void) { return s_awake; }

static void set_brightness(void *var, int32_t pct)
{
    (void)var;
    bsp_display_brightness_set((int)pct);
}

static void fade_to(int from, int to, uint32_t ms)
{
    lv_anim_delete(&s_fade_var, set_brightness);
    lv_anim_init(&s_fade);
    lv_anim_set_var(&s_fade, &s_fade_var);
    lv_anim_set_exec_cb(&s_fade, set_brightness);
    lv_anim_set_values(&s_fade, from, to);
    lv_anim_set_duration(&s_fade, ms);
    lv_anim_start(&s_fade);
}

static void go_to_sleep(void)
{
    if (!s_awake) {
        return;
    }
    s_awake = false;
    ESP_LOGI(TAG, "sleep");
    fade_to(CONFIG_WATCH_BRIGHTNESS, 0, FADE_OUT_MS);
}

static void wake_up(void)
{
    if (s_awake) {
        return;
    }
    s_awake = true;
    ESP_LOGI(TAG, "wake");
    lv_display_trigger_activity(NULL); /* restart the idle countdown */
    if (s_on_wake) {
        s_on_wake(); /* refresh content before it becomes visible */
    }
    fade_to(0, CONFIG_WATCH_BRIGHTNESS, FADE_IN_MS);
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

void screen_init(void (*on_wake)(void))
{
    s_on_wake = on_wake;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    bsp_display_brightness_set(CONFIG_WATCH_BRIGHTNESS);
    lv_timer_create(poll, POLL_MS, NULL);
}
