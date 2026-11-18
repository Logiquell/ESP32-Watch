/*
 * Vintage flip-clock face with radium-paint numerals.
 *
 * Two split-flap cards (HH and MM) on black. When a value changes, the top flap falls:
 * the old top half squashes down onto the hinge, then the new bottom half swings down
 * into place. Numerals are pale radium green with a soft luminous halo, drawn as several
 * faint offset copies behind the sharp digits (a real blur would band on this display,
 * which renders in 20-line strips).
 *
 * Font: Ubuntu Medium TTF, embedded from the LVGL package and rendered with Tiny TTF.
 */
#include "sdkconfig.h"
#if CONFIG_WATCH_FACE_FLIP

#include "face.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"

extern const uint8_t ttf_start[] asm("_binary_Ubuntu_Medium_ttf_start");
extern const uint8_t ttf_end[] asm("_binary_Ubuntu_Medium_ttf_end");

/* ---- Look ------------------------------------------------------------------------- */
#define COL_BG         0x000000  /* AMOLED black */
#define COL_CARD_HI    0x1E201C  /* card face, top of each half */
#define COL_CARD_LO    0x111310  /* card face, bottom of each half */
#define COL_AXLE       0x2C2E2A
#define COL_RADIUM     0xD8F8BC  /* painted numerals */
#define COL_GLOW       0x7CFF52  /* luminous halo */
#define COL_DIM        0x9CC985  /* small text */
#define GLOW_OPA       LV_OPA_20 /* per halo copy; copies overlap near the strokes */
#define GLOW_R         4         /* halo reach in px */

/* ---- Layout (screen 410 x 502) ---------------------------------------------------- */
#define CARD_W     364
#define CARD_H     176
#define CARD_X     ((410 - CARD_W) / 2)
#define CARD_HH_Y  72
#define CARD_MM_Y  (CARD_HH_Y + CARD_H + 14)
#define SPLIT      4                         /* gap between the two halves of a card */
#define HALF_H     ((CARD_H - SPLIT) / 2)
#define DIGIT_PX   160
#define FLIP_MS    170                       /* per half of the flip */

static lv_font_t *s_font_big, *s_font_date, *s_font_status;
static int32_t s_label_y; /* numeral label y within a card, centred on the card */
static bool s_skip_anim = true;

/* ---- Radium glow ------------------------------------------------------------------ */
static void glow_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, GLOW_R + 1);
        return;
    }
    /* LV_EVENT_DRAW_MAIN, pre-processed: paint the halo before the label draws its text */
    const char *text = lv_label_get_text(obj);
    if (text == NULL || text[0] == '\0') {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = lv_obj_get_style_text_font(obj, 0);
    d.color = lv_color_hex(COL_GLOW);
    d.opa = GLOW_OPA;
    d.align = lv_obj_get_style_text_align(obj, 0);
    d.text = text;
    d.text_local = 1; /* draw units may run after this callback returns */

    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    const int r = GLOW_R, q = GLOW_R * 7 / 10;
    const int16_t off[8][2] = {{-r, 0}, {r, 0}, {0, -r}, {0, r}, {-q, -q}, {q, -q}, {-q, q}, {q, q}};
    for (int i = 0; i < 8; i++) {
        lv_area_t b = a;
        lv_area_move(&b, off[i][0], off[i][1]);
        lv_draw_label(layer, &d, &b);
    }
}

static lv_obj_t *radium_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_RADIUM), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(l, glow_cb, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(l, glow_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    return l;
}

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* ---- Flip card -------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *flap_top, *flap_bot;                 /* animated halves */
    lv_obj_t *l_top, *l_bot, *l_flap_top, *l_flap_bot;
    char shown[4];                                 /* value the card is settling on */
    char pending[4];                               /* next value if one arrives mid-flip */
    bool flipping;
} flip_card_t;

static flip_card_t s_hh, s_mm;

static lv_obj_t *card_half(lv_obj_t *card, bool top, lv_obj_t **label)
{
    lv_obj_t *h = lv_obj_create(card);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, CARD_W, HALF_H);
    lv_obj_set_pos(h, 0, top ? 0 : HALF_H + SPLIT);
    lv_obj_set_style_radius(h, 12, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(top ? COL_CARD_HI : COL_CARD_HI), 0);
    lv_obj_set_style_bg_grad_color(h, lv_color_hex(COL_CARD_LO), 0);
    lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = radium_label(h, s_font_big);
    lv_obj_set_width(l, CARD_W);
    /* Same numeral position in both halves; each half clips its own part of it. */
    lv_obj_set_pos(l, 0, top ? s_label_y : s_label_y - (HALF_H + SPLIT));
    *label = l;
    return h;
}

static void card_create(flip_card_t *c, lv_obj_t *parent, int y)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, CARD_X, y);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card_half(card, true, &c->l_top);
    card_half(card, false, &c->l_bot);
    c->flap_top = card_half(card, true, &c->l_flap_top);
    c->flap_bot = card_half(card, false, &c->l_flap_bot);
    lv_obj_set_style_transform_pivot_x(c->flap_top, CARD_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(c->flap_top, HALF_H, 0);  /* hinge = its bottom edge */
    lv_obj_set_style_transform_pivot_x(c->flap_bot, CARD_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(c->flap_bot, 0, 0);       /* hinge = its top edge */
    lv_obj_add_flag(c->flap_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->flap_bot, LV_OBJ_FLAG_HIDDEN);

    /* Hinge axles at both ends of the split */
    for (int side = 0; side < 2; side++) {
        lv_obj_t *ax = lv_obj_create(card);
        lv_obj_remove_style_all(ax);
        lv_obj_set_size(ax, 8, 18);
        lv_obj_set_pos(ax, side ? CARD_W - 8 : 0, HALF_H + SPLIT / 2 - 9);
        lv_obj_set_style_radius(ax, 3, 0);
        lv_obj_set_style_bg_opa(ax, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ax, lv_color_hex(COL_AXLE), 0);
    }
    c->shown[0] = '\0';
    c->pending[0] = '\0';
}

static void flap_scale_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale_y(obj, v, 0);
}

static void flip_start(flip_card_t *c, const char *next);

static void flip_done(lv_anim_t *a)
{
    flip_card_t *c = lv_anim_get_user_data(a);
    lv_label_set_text(c->l_bot, c->shown);
    lv_obj_add_flag(c->flap_bot, LV_OBJ_FLAG_HIDDEN);
    c->flipping = false;
    if (c->pending[0] && strcmp(c->pending, c->shown) != 0) {
        char next[4];
        strcpy(next, c->pending);
        c->pending[0] = '\0';
        flip_start(c, next);
    }
}

/* Old top flap has fallen onto the hinge: swing the new bottom half down. */
static void flip_half_way(lv_anim_t *a)
{
    flip_card_t *c = lv_anim_get_user_data(a);
    lv_obj_add_flag(c->flap_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_transform_scale_y(c->flap_bot, 0, 0);
    lv_obj_remove_flag(c->flap_bot, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, c->flap_bot);
    lv_anim_set_exec_cb(&b, flap_scale_cb);
    lv_anim_set_values(&b, 0, LV_SCALE_NONE);
    lv_anim_set_duration(&b, FLIP_MS);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_user_data(&b, c);
    lv_anim_set_completed_cb(&b, flip_done);
    lv_anim_start(&b);
}

static void flip_start(flip_card_t *c, const char *next)
{
    char old[4];
    strcpy(old, c->shown);
    strcpy(c->shown, next);
    c->flipping = true;

    lv_label_set_text(c->l_top, next);       /* revealed behind the falling flap */
    lv_label_set_text(c->l_flap_top, old);   /* the flap that falls */
    lv_label_set_text(c->l_flap_bot, next);  /* swings down to cover the old bottom */
    lv_obj_set_style_transform_scale_y(c->flap_top, LV_SCALE_NONE, 0);
    lv_obj_remove_flag(c->flap_top, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c->flap_top);
    lv_anim_set_exec_cb(&a, flap_scale_cb);
    lv_anim_set_values(&a, LV_SCALE_NONE, 0);
    lv_anim_set_duration(&a, FLIP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_user_data(&a, c);
    lv_anim_set_completed_cb(&a, flip_half_way);
    lv_anim_start(&a);
}

static void card_set(flip_card_t *c, const char *v, bool animate)
{
    if (c->flipping) {
        if (!animate) {
            lv_anim_delete(c->flap_top, flap_scale_cb);
            lv_anim_delete(c->flap_bot, flap_scale_cb);
            lv_obj_add_flag(c->flap_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(c->flap_bot, LV_OBJ_FLAG_HIDDEN);
            c->flipping = false;
            c->pending[0] = '\0';
        } else {
            strncpy(c->pending, v, sizeof(c->pending) - 1);
            return;
        }
    }
    if (strcmp(c->shown, v) == 0) {
        return;
    }
    if (!animate || c->shown[0] == '\0') {
        strcpy(c->shown, v);
        lv_label_set_text(c->l_top, v);
        lv_label_set_text(c->l_bot, v);
        return;
    }
    flip_start(c, v);
}

/* ---- Small text ------------------------------------------------------------------- */
static lv_obj_t *s_date, *s_batt;

static lv_obj_t *dim_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

/* ---- Face API --------------------------------------------------------------------- */
void face_create(void)
{
    const size_t ttf_size = ttf_end - ttf_start;
    s_font_big = lv_tiny_ttf_create_data(ttf_start, ttf_size, DIGIT_PX);
    s_font_date = lv_tiny_ttf_create_data(ttf_start, ttf_size, 30);
    s_font_status = lv_tiny_ttf_create_data(ttf_start, ttf_size, 22);

    /* Centre the numerals' cap height (≈0.69 em for Ubuntu) on the card's split. */
    const int32_t baseline_from_top = s_font_big->line_height - s_font_big->base_line;
    s_label_y = CARD_H / 2 - baseline_from_top + (DIGIT_PX * 69 / 100) / 2;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_date = dim_label(scr, s_font_date); /* no halo: it competed with the numerals */
    lv_obj_set_style_text_letter_space(s_date, 3, 0);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 22);

    card_create(&s_hh, scr, CARD_HH_Y);
    card_create(&s_mm, scr, CARD_MM_Y);

    /* Centred: the screen's rounded corners clip text placed near the bottom edges. */
    s_batt = dim_label(scr, s_font_status);
    lv_obj_set_width(s_batt, CARD_W);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_batt, LV_ALIGN_BOTTOM_MID, 0, -22);
}

void face_skip_animation(void)
{
    s_skip_anim = true;
}

void face_set_time(const struct tm *tm, bool colon_on)
{
    (void)colon_on;
    char hh[4], mm[4], date[24];
    snprintf(hh, sizeof(hh), "%02d", tm->tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tm->tm_min);
    card_set(&s_hh, hh, !s_skip_anim);
    card_set(&s_mm, mm, !s_skip_anim);
    s_skip_anim = false;

    strftime(date, sizeof(date), "%a %d %b", tm);
    for (char *p = date; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p -= 'a' - 'A';
        }
    }
    set_text_if_changed(s_date, date);
}

void face_set_battery(const battery_info_t *b, bool blink)
{
    char txt[24] = "";
    if (b->valid && b->present && b->percent >= 0) {
        bool low = b->percent <= 15 && !b->charging;
        if (low && !blink) {
            txt[0] = '\0'; /* low battery: blink */
        } else {
            snprintf(txt, sizeof(txt), "BATT %d%%%s", b->percent, b->charging ? "  CHG" : "");
        }
    } else if (b->valid && b->usb_power) {
        strcpy(txt, "USB");
    }
    set_text_if_changed(s_batt, txt);
}

void face_set_sync(wifi_time_state_t state, bool blink)
{
    (void)state; /* this face doesn't show sync status */
    (void)blink;
}

void face_show_low_battery(void)
{
    card_set(&s_hh, "LO", false);
    card_set(&s_mm, "", false);
}

#endif /* CONFIG_WATCH_FACE_FLIP */
