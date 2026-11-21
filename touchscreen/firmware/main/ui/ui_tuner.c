#include "ui_tuner.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "seed_link.h"
#include "tuner_note.h"
#include "ui_theme.h"

// Geometry, for the 1024 x 600 panel.
#define METER_W        860
#define METER_H        130
#define METER_PAD      30    // room for the -50 / +50 labels inside the meter's own area
#define METER_CENTRE   (METER_PAD + METER_W / 2)
#define TRACK_Y        66
#define PX_PER_CENT    ((float)METER_W / 100.0f)
#define NEEDLE_W       10
#define NEEDLE_H       64    // clear of the tick labels under the track
// The note: LVGL's largest built-in font drawn at twice its size (LV_SCALE_NONE is 256). Enabled
// in sdkconfig.defaults; an older sdkconfig without it gets the 24 px font at 4x, softer.
#if LV_FONT_MONTSERRAT_48
#define NOTE_FONT      (&lv_font_montserrat_48)
#define NOTE_SCALE     512
#else
#define NOTE_FONT      (&lv_font_montserrat_24)
#define NOTE_SCALE     1024
#endif
#define REFRESH_MS     30
#define CLOSE_WARN_CENTS 15.0f  // amber within this, red beyond

static lv_obj_t *s_page;
static lv_obj_t *s_note, *s_octave, *s_status, *s_readout, *s_needle, *s_zone;
static lv_obj_t *s_level_bar, *s_level_text, *s_mute_sw, *s_mute_knob, *s_mock;
static lv_timer_t *s_timer;
static bool s_mute = true;  // "Mute while tuning": on by default, kept while the board runs
static tuner_smooth_t s_smooth;
static uint32_t s_seq;

// ---------------------------------------------------------------------------------------------
// Small helpers (the page builds its own, so it depends on nothing but LVGL and the theme)

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t color, int32_t radius) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, color, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static void set_text(lv_obj_t *l, const char *text) {
    if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

static void set_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t part, bool text) {
    if (text) lv_obj_set_style_text_color(o, c, part);
    else lv_obj_set_style_bg_color(o, c, part);
}

static void set_hidden(lv_obj_t *o, bool hidden) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_switch(bool on) {
    lv_obj_set_style_bg_color(s_mute_sw, on ? UI_COLOR_ACCENT : UI_COLOR_TRACK, 0);
    lv_obj_align(s_mute_knob, LV_ALIGN_LEFT_MID, on ? 23 : 3, 0);
}

// ---------------------------------------------------------------------------------------------
// Events

static void done_cb(lv_event_t *e) {
    (void)e;
    ui_tuner_close();
}

static void mute_cb(lv_event_t *e) {
    (void)e;
    s_mute = !s_mute;
    set_switch(s_mute);
    seed_link_set_tuner(true, s_mute);
}

// ---------------------------------------------------------------------------------------------
// Refresh

static void show_note(const tuner_view_t *v, bool connected, bool supported) {
    char text[48];
    if (!connected || !supported) {
        set_text(s_note, "-");
        set_text(s_octave, "");
        set_text(s_status, connected ? "This Seed firmware has no tuner" : "Seed not connected");
        set_color(s_status, UI_COLOR_MUTE, 0, true);
        set_color(s_note, UI_COLOR_TEXT_MUTED, 0, true);
        set_text(s_readout, "");
        set_hidden(s_needle, true);
        return;
    }
    if (!v->note) {
        set_text(s_note, "-");
        set_text(s_octave, "");
        set_text(s_status, "Play one string");
        set_color(s_status, UI_COLOR_TEXT_MUTED, 0, true);
        set_color(s_note, UI_COLOR_TEXT_MUTED, 0, true);
        set_text(s_readout, "");
        set_hidden(s_needle, true);
        return;
    }

    const float c = v->cents;
    // A held note (the string has faded) in grey: still there to read, plainly not live.
    const lv_color_t tone = v->held      ? UI_COLOR_OFF
                            : v->in_tune ? UI_COLOR_OK
                            : fabsf(c) <= CLOSE_WARN_CENTS ? UI_COLOR_WARN
                                                           : UI_COLOR_MUTE;
    snprintf(text, sizeof(text), "%d", v->octave);
    if (strcmp(lv_label_get_text(s_note), v->name) != 0 || strcmp(lv_label_get_text(s_octave), text) != 0) {
        lv_label_set_text(s_note, v->name);
        lv_label_set_text(s_octave, text);
        // The octave beside the note as drawn: twice its laid-out width ("E" is narrower than "C#").
        lv_obj_update_layout(s_note);
        lv_obj_update_layout(s_octave);
        const int32_t half_drawn = lv_obj_get_width(s_note) * NOTE_SCALE / 256 / 2;
        lv_obj_align(s_octave, LV_ALIGN_CENTER, half_drawn + 10 + lv_obj_get_width(s_octave) / 2, 30);
    }
    set_color(s_note, v->held ? UI_COLOR_TEXT_MUTED : v->in_tune ? UI_COLOR_OK : UI_COLOR_TEXT, 0, true);
    set_text(s_status, v->in_tune ? "In tune" : c < 0 ? "Tune up" : "Tune down");
    set_color(s_status, tone, 0, true);

    // Cents as a whole number ("+0" never shows: 0 has no sign), and the frequency.
    const int whole = (int)lroundf(c);
    if (whole == 0) snprintf(text, sizeof(text), "0 cents     %.1f Hz", (double)v->hz);
    else snprintf(text, sizeof(text), "%+d %s     %.1f Hz", whole, whole == 1 || whole == -1 ? "cent" : "cents", (double)v->hz);
    set_text(s_readout, text);

    const float clamped = c < -50.0f ? -50.0f : c > 50.0f ? 50.0f : c;
    lv_obj_set_x(s_needle, (int32_t)lroundf(METER_CENTRE + clamped * PX_PER_CENT) - NEEDLE_W / 2);
    set_color(s_needle, tone, 0, false);
    set_hidden(s_needle, false);
}

static void show_level(const seed_link_tuner_t *t) {
    char text[16];
    if (!t->fresh) {
        lv_bar_set_value(s_level_bar, 0, LV_ANIM_OFF);
        set_text(s_level_text, "-");
        return;
    }
    // -60 dBFS (the detector's floor) to 0 dBFS across the bar.
    const float db = t->level_dbfs < -60.0f ? -60.0f : t->level_dbfs > 0.0f ? 0.0f : t->level_dbfs;
    lv_bar_set_value(s_level_bar, (int32_t)lroundf((db + 60.0f) * 100.0f / 60.0f), LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%.0f dB", (double)t->level_dbfs);
    set_text(s_level_text, t->level_dbfs > -120.0f ? text : "-");
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    seed_link_tuner_t t;
    seed_link_get_tuner(&t);
    const uint32_t now = lv_tick_get();
    tuner_view_t v;
    if (t.supported && t.seq != s_seq) {
        s_seq = t.seq;
        tuner_smooth_feed(&s_smooth, t.freq_hz, now, &v);
    } else {
        tuner_smooth_view(&s_smooth, now, &v);
    }
    show_note(&v, t.connected, t.supported);
    show_level(&t);
    set_hidden(s_mock, !(t.connected && t.mock));
}

// ---------------------------------------------------------------------------------------------
// Building

static void build_meter(lv_obj_t *page) {
    lv_obj_t *m = lv_obj_create(page);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, METER_W + 2 * METER_PAD, METER_H);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // The in-tune zone, then the track and the ticks over it.
    const int32_t zone_w = (int32_t)lroundf(2.0f * TUNER_IN_TUNE_CENTS * PX_PER_CENT);
    s_zone = rect(m, zone_w, 44, UI_COLOR_OK, 8);
    lv_obj_set_style_bg_opa(s_zone, LV_OPA_30, 0);
    lv_obj_set_pos(s_zone, METER_CENTRE - zone_w / 2, TRACK_Y - 22);
    lv_obj_t *track = rect(m, METER_W, 6, UI_COLOR_TRACK, 3);
    lv_obj_set_pos(track, METER_PAD, TRACK_Y - 3);
    for (int c = -50; c <= 50; c += 5) {
        const bool major = c % 25 == 0;
        const int32_t h = c == 0 ? 40 : major ? 28 : 14;
        lv_obj_t *tick = rect(m, major ? 3 : 2, h, major ? UI_COLOR_TEXT_MUTED : UI_COLOR_BORDER, 1);
        lv_obj_set_pos(tick, (int32_t)lroundf(METER_CENTRE + c * PX_PER_CENT) - 1, TRACK_Y - h / 2);
        if (major) {
            char text[8];
            if (c == 0) snprintf(text, sizeof(text), "0");
            else snprintf(text, sizeof(text), "%+d", c);
            lv_obj_t *l = label(m, text, &lv_font_montserrat_14, UI_COLOR_TEXT_MUTED);
            lv_obj_update_layout(l);
            lv_obj_set_pos(l, (int32_t)lroundf(METER_CENTRE + c * PX_PER_CENT) - lv_obj_get_width(l) / 2, TRACK_Y + 30);
        }
    }
    lv_obj_t *flat = label(m, "FLAT", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_set_pos(flat, METER_PAD, 0);
    lv_obj_t *sharp = label(m, "SHARP", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_align(sharp, LV_ALIGN_TOP_RIGHT, -METER_PAD, 0);

    s_needle = rect(m, NEEDLE_W, NEEDLE_H, UI_COLOR_OK, NEEDLE_W / 2);
    lv_obj_set_y(s_needle, TRACK_Y - NEEDLE_H / 2);
    lv_obj_set_style_shadow_width(s_needle, 14, 0);
    lv_obj_set_style_shadow_color(s_needle, UI_COLOR_BACKDROP, 0);
    lv_obj_set_style_shadow_opa(s_needle, LV_OPA_60, 0);
    lv_obj_add_flag(s_needle, LV_OBJ_FLAG_HIDDEN);
}

static void build(void) {
    s_page = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_page, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_CLICKABLE); // taps stop here, never reach the page below
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    // Header: title on the left; mute and Done on the right.
    lv_obj_t *bar = rect(s_page, LV_PCT(100), 76, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_DIVIDER, 0);
    lv_obj_t *title = label(bar, "TUNER", &lv_font_montserrat_24, UI_COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 36, -10);
    lv_obj_t *sub = label(bar, "Standard pitch, A4 = 440 Hz", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 36, 16);
    s_mock = label(bar, "MOCK SEED", &lv_font_montserrat_12, UI_COLOR_WARN);
    lv_obj_align(s_mock, LV_ALIGN_LEFT_MID, 260, 16);
    lv_obj_add_flag(s_mock, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *done = lv_button_create(bar);
    lv_obj_set_size(done, 120, 46);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_set_style_bg_color(done, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(done, 10, 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_l = label(done, "Done", &lv_font_montserrat_16, UI_COLOR_ON_ACCENT);
    lv_obj_center(done_l);

    s_mute_sw = rect(bar, 46, 26, UI_COLOR_TRACK, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(s_mute_sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mute_sw, 12);
    lv_obj_add_event_cb(s_mute_sw, mute_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(s_mute_sw, LV_ALIGN_RIGHT_MID, -176, 0);
    s_mute_knob = rect(s_mute_sw, 20, 20, lv_color_white(), LV_RADIUS_CIRCLE);
    set_switch(s_mute);
    lv_obj_t *mute_l = label(bar, "Mute while tuning", &lv_font_montserrat_14, UI_COLOR_TEXT);
    lv_obj_align_to(mute_l, s_mute_sw, LV_ALIGN_OUT_LEFT_MID, -14, 0);

    // The note, drawn large (NOTE_SCALE), its octave beside it.
    lv_obj_t *note_box = lv_obj_create(s_page);
    lv_obj_remove_style_all(note_box);
    lv_obj_set_size(note_box, 400, 140);
    lv_obj_align(note_box, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_clear_flag(note_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_note = label(note_box, "-", NOTE_FONT, UI_COLOR_TEXT_MUTED);
    lv_obj_set_style_transform_scale(s_note, NOTE_SCALE, 0);
    lv_obj_set_style_transform_pivot_x(s_note, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_note, LV_PCT(50), 0);
    lv_obj_center(s_note);
    s_octave = label(note_box, "", &lv_font_montserrat_24, UI_COLOR_TEXT_DIM);
    lv_obj_align(s_octave, LV_ALIGN_CENTER, 92, 30);

    s_status = label(s_page, "Play one string", &lv_font_montserrat_24, UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 254);

    build_meter(s_page);

    s_readout = label(s_page, "", &lv_font_montserrat_16, UI_COLOR_TEXT_DIM);
    lv_obj_align(s_readout, LV_ALIGN_TOP_MID, 0, 486);

    // Footer: the input level, so a silent page can be told from a guitar that isn't reaching the Seed.
    lv_obj_t *foot = lv_obj_create(s_page);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_PCT(100), 56);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *in_l = label(foot, "INPUT", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_align(in_l, LV_ALIGN_LEFT_MID, 36, 0);
    s_level_bar = lv_bar_create(foot);
    lv_obj_set_size(s_level_bar, 220, 8);
    lv_bar_set_range(s_level_bar, 0, 100);
    lv_obj_set_style_bg_color(s_level_bar, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_level_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x5c8ee0), LV_PART_INDICATOR); // the IN meter's blue
    lv_obj_align_to(s_level_bar, in_l, LV_ALIGN_OUT_RIGHT_MID, 14, 0);
    s_level_text = label(foot, "-", &lv_font_montserrat_12, UI_COLOR_TEXT_DIM);
    lv_obj_align_to(s_level_text, s_level_bar, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_t *hint = label(foot, "Pick one string at a time and let it ring.", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -36, 0);
}

// ---------------------------------------------------------------------------------------------
// Public

void ui_tuner_open(void) {
    if (s_page) return;
    tuner_smooth_reset(&s_smooth);
    seed_link_tuner_t t;
    seed_link_get_tuner(&t);
    s_seq = t.seq; // a reading left over from an earlier session isn't shown
    build();
    seed_link_set_tuner(true, s_mute);
    s_timer = lv_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL);
}

void ui_tuner_close(void) {
    if (!s_page) return;
    seed_link_set_tuner(false, false);
    lv_timer_delete(s_timer);
    s_timer = NULL;
    lv_obj_delete(s_page);
    s_page = NULL;
}

bool ui_tuner_is_open(void) { return s_page != NULL; }
