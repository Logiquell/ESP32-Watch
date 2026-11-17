#include "face_lcd.h"

#include <stdio.h>
#include "lvgl.h"

/* ---- Palette -------------------------------------------------------------------- */
#define COL_BEZEL     0x000000  /* pure black: AMOLED pixels off */
#define COL_BEZEL_TXT 0x6E6E6E
#define COL_PANEL     0x3F8A45  /* green backlit LCD glass (dimmer than bare grey LCD) */
#define COL_PANEL_EDGE 0x1A3A1D
#define COL_SEG       0x07130A  /* lit segment: near-black green */
#define GHOST_OPA     LV_OPA_20 /* unlit segments still faintly visible, like a real LCD */

/* ---- Layout (screen 410 x 502) --------------------------------------------------- */
#define PANEL_W 372
#define PANEL_H 372

/* ---- Lit / ghost helpers --------------------------------------------------------- */
static void seg_style(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_SEG), 0);
    lv_obj_set_style_bg_opa(o, GHOST_OPA, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* Only restyle when the state changes, so a steady face causes no redraws. */
static void set_lit(lv_obj_t *o, bool lit)
{
    lv_opa_t want = lit ? LV_OPA_COVER : GHOST_OPA;
    if (lv_obj_get_style_bg_opa(o, 0) != want) {
        lv_obj_set_style_bg_opa(o, want, 0);
    }
}

static void set_text_lit(lv_obj_t *label, bool lit)
{
    lv_opa_t want = lit ? LV_OPA_COVER : GHOST_OPA;
    if (lv_obj_get_style_text_opa(label, 0) != want) {
        lv_obj_set_style_text_opa(label, want, 0);
    }
}

static lv_obj_t *lcd_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_SEG), 0);
    lv_obj_set_style_text_opa(l, GHOST_OPA, 0);
    return l;
}

/* ---- Seven-segment digit ----------------------------------------------------------- */
/* Segment order: a (top), b (top-right), c (bottom-right), d (bottom), e (bottom-left),
   f (top-left), g (middle). */
typedef struct {
    lv_obj_t *seg[7];
    int8_t value; /* currently shown pattern index, -1 = none */
} digit_t;

static const uint8_t DIGIT_MASK[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
#define MASK_DASH 0x40 /* g only */

static void digit_create(digit_t *d, lv_obj_t *parent, int x, int y, int w, int h, int t)
{
    const int gap = t / 4 + 1;          /* gap between segment ends, like real LCD glass */
    const int vl = (h - 3 * t) / 2;     /* vertical segment length */
    const int hl = w - 2 * t;           /* horizontal segment length */
    const struct { int x, y, w, h; } r[7] = {
        {t + gap,     0,              hl - 2 * gap, t},              /* a */
        {w - t,       t + gap,        t,            vl - 2 * gap},   /* b */
        {w - t,       2 * t + vl + gap, t,          vl - 2 * gap},   /* c */
        {t + gap,     2 * (t + vl),   hl - 2 * gap, t},              /* d */
        {0,           2 * t + vl + gap, t,          vl - 2 * gap},   /* e */
        {0,           t + gap,        t,            vl - 2 * gap},   /* f */
        {t + gap,     t + vl,         hl - 2 * gap, t},              /* g */
    };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *s = lv_obj_create(parent);
        seg_style(s);
        lv_obj_set_pos(s, x + r[i].x, y + r[i].y);
        lv_obj_set_size(s, r[i].w, r[i].h);
        lv_obj_set_style_radius(s, t / 2, 0);
        d->seg[i] = s;
    }
    d->value = -1;
}

static void digit_show_mask(digit_t *d, int key, uint8_t mask)
{
    if (d->value == key) {
        return;
    }
    d->value = key;
    for (int i = 0; i < 7; i++) {
        set_lit(d->seg[i], mask & (1 << i));
    }
}

static void digit_set(digit_t *d, int v)
{
    digit_show_mask(d, v, DIGIT_MASK[v % 10]);
}

/* ---- Face elements ----------------------------------------------------------------- */
static digit_t s_hh[2], s_mm[2], s_ss[2], s_mon[2], s_day[2];
static lv_obj_t *s_colon[2];
static lv_obj_t *s_date_dash;
static lv_obj_t *s_weekday[7];
static lv_obj_t *s_bat_bar[4];
static lv_obj_t *s_lbl_chg, *s_lbl_sync, *s_lbl_err;

static lv_obj_t *seg_block(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    seg_style(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void face_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BEZEL), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Bezel printing */
    lv_obj_t *top = lv_label_create(scr);
    lv_label_set_text(top, "DIGITAL  QUARTZ");
    lv_obj_set_style_text_font(top, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(top, lv_color_hex(COL_BEZEL_TXT), 0);
    lv_obj_set_style_text_letter_space(top, 2, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *bottom = lv_label_create(scr);
    lv_label_set_text(bottom, "LCD-410  ESP32-S3");
    lv_obj_set_style_text_font(bottom, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(bottom, lv_color_hex(COL_BEZEL_TXT), 0);
    lv_obj_set_style_text_letter_space(bottom, 2, 0);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -34);

    /* LCD panel */
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PANEL_W, PANEL_H);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 22, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(p, 5, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Row 1: weekday flags (all printed, today's lit) + date MM-DD */
    static const char *DAYS[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; i++) {
        s_weekday[i] = lcd_label(p, DAYS[i], &lv_font_unscii_16);
        lv_obj_set_pos(s_weekday[i], 20 + i * 26, 20);
    }

    const int dw = 22, dh = 40, dt = 5, dy = 46;
    int x = 20;
    digit_create(&s_mon[0], p, x, dy, dw, dh, dt); x += dw + 6;
    digit_create(&s_mon[1], p, x, dy, dw, dh, dt); x += dw + 6;
    s_date_dash = seg_block(p, x, dy + (dh - dt) / 2, 12, dt, dt / 2); x += 12 + 6;
    digit_create(&s_day[0], p, x, dy, dw, dh, dt); x += dw + 6;
    digit_create(&s_day[1], p, x, dy, dw, dh, dt);

    /* Seconds, top-right */
    const int sw = 30, sh = 54, st = 7;
    digit_create(&s_ss[0], p, PANEL_W - 24 - 2 * sw - 8, 24, sw, sh, st);
    digit_create(&s_ss[1], p, PANEL_W - 24 - sw, 24, sw, sh, st);

    /* Row 2: big HH:MM */
    const int bw = 66, bh = 130, bt = 14, gap = 10, colon_w = 26;
    const int total = 4 * bw + 2 * gap + colon_w + 2 * gap;
    int bx = (PANEL_W - total) / 2;
    const int by = 112;
    digit_create(&s_hh[0], p, bx, by, bw, bh, bt); bx += bw + gap;
    digit_create(&s_hh[1], p, bx, by, bw, bh, bt); bx += bw + gap;
    s_colon[0] = seg_block(p, bx + (colon_w - bt) / 2, by + bh / 3 - bt / 2, bt, bt, 3);
    s_colon[1] = seg_block(p, bx + (colon_w - bt) / 2, by + 2 * bh / 3 - bt / 2, bt, bt, 3);
    bx += colon_w + gap;
    digit_create(&s_mm[0], p, bx, by, bw, bh, bt); bx += bw + gap;
    digit_create(&s_mm[1], p, bx, by, bw, bh, bt);

    lv_obj_t *h24 = lcd_label(p, "24H", &lv_font_unscii_16);
    lv_obj_set_style_text_opa(h24, LV_OPA_COVER, 0);
    lv_obj_set_pos(h24, 20, by + bh + 14);

    /* Row 3: battery bars + CHG, sync status */
    const int ry = PANEL_H - 70;
    lv_obj_t *bat = lcd_label(p, "BAT", &lv_font_unscii_16);
    lv_obj_set_style_text_opa(bat, LV_OPA_COVER, 0);
    lv_obj_set_pos(bat, 20, ry + 6);
    for (int i = 0; i < 4; i++) {
        s_bat_bar[i] = seg_block(p, 74 + i * 22, ry, 16, 28, 2);
    }
    s_lbl_chg = lcd_label(p, "CHG", &lv_font_unscii_16);
    lv_obj_set_pos(s_lbl_chg, 74 + 4 * 22 + 8, ry + 6);

    s_lbl_err = lcd_label(p, "ERR", &lv_font_unscii_16);
    lv_obj_set_pos(s_lbl_err, PANEL_W - 24 - 2 * 8 * 4 - 12, ry + 6);
    s_lbl_sync = lcd_label(p, "SYNC", &lv_font_unscii_16);
    lv_obj_set_pos(s_lbl_sync, PANEL_W - 24 - 8 * 4 - 4, ry + 6);
}

void face_set_time(const struct tm *tm, bool colon_on)
{
    digit_set(&s_hh[0], tm->tm_hour / 10);
    digit_set(&s_hh[1], tm->tm_hour % 10);
    digit_set(&s_mm[0], tm->tm_min / 10);
    digit_set(&s_mm[1], tm->tm_min % 10);
    digit_set(&s_ss[0], tm->tm_sec / 10);
    digit_set(&s_ss[1], tm->tm_sec % 10);
    set_lit(s_colon[0], colon_on);
    set_lit(s_colon[1], colon_on);

    int mon = tm->tm_mon + 1;
    digit_set(&s_mon[0], mon / 10);
    digit_set(&s_mon[1], mon % 10);
    digit_set(&s_day[0], tm->tm_mday / 10);
    digit_set(&s_day[1], tm->tm_mday % 10);
    set_lit(s_date_dash, true);

    for (int i = 0; i < 7; i++) {
        set_text_lit(s_weekday[i], i == tm->tm_wday);
    }
}

void face_set_battery(const battery_info_t *b, bool blink)
{
    int bars = 0;
    bool known = b->valid && b->present && b->percent >= 0;
    if (known) {
        bars = (b->percent + 24) / 25; /* 1-25% = 1 bar ... 76-100% = 4 bars */
        if (b->percent <= 15 && !b->charging && !blink) {
            bars = 0; /* low battery: bars blink */
        }
    }
    for (int i = 0; i < 4; i++) {
        set_lit(s_bat_bar[i], i < bars);
    }
    set_text_lit(s_lbl_chg, b->valid && b->charging);
}

void face_set_sync(wifi_time_state_t state, bool blink)
{
    bool busy = state == WIFI_TIME_CONNECTING || state == WIFI_TIME_SYNCING;
    set_text_lit(s_lbl_sync, state == WIFI_TIME_SYNCED || (busy && blink));
    set_text_lit(s_lbl_err, state == WIFI_TIME_FAILED);
}
