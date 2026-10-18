// Copyright 2026 Mechboards
// SPDX-License-Identifier: GPL-2.0-or-later

#include <printf.h>
#include <ctype.h>
#include <string.h>
#include "qp.h"
#include "qp_font/pixellari18.qff.h"
#include "qp_font/pixellari24.qff.h"
#include "display_lcd.h"
#include "version.h"
#include "lib/lib8tion/lib8tion.h"
#include "rgb_matrix.h"
#include "backlight/backlight.h"
#include "robot_sprites.h"
#ifdef ROBOT_UI_ENABLE
#    include "transactions.h"
#endif

// ---------------------------------------------------------------------------
// Robot UI (define ROBOT_UI_ENABLE in the keyboard config.h to turn it on).
//   left screen : layer, uptime + backspace rate, WPM graph, modifier indicators, OS
//   right screen: layer 0          -> typing robot with WPM and session TOP speed
//                 layers 1, 2 ...  -> history of the last typed keys
//                 ROBOT_RGB_LAYER  -> RGB / backlight info
// Needs SPLIT_WPM_ENABLE, SPLIT_MODS_ENABLE and SPLIT_TRANSACTION_IDS_KB = KB_SYNC_ROBOT.
// Every value below can be overridden in config.h.
// ---------------------------------------------------------------------------
#ifndef ROBOT_RGB_LAYER
#    define ROBOT_RGB_LAYER 3 // layer index that shows the RGB info screen on the right display
#endif
#ifndef ROBOT_FAST_WPM
#    define ROBOT_FAST_WPM 60 // at or above this WPM the robot types with both hands + sparks (2 animated frames)
#endif
#ifndef ROBOT_SLEEP_MS
#    define ROBOT_SLEEP_MS 30000 // no key press for this long -> robot falls asleep
#endif
#ifndef ROBOT_PRESS_MS
#    define ROBOT_PRESS_MS 140 // how long the "hand down" pose is held per key press
#endif
#ifndef ROBOT_TICK_MS
#    define ROBOT_TICK_MS 60 // right display refresh interval while the robot is shown
#endif
#ifndef ROBOT_HAPPY_MS
#    define ROBOT_HAPPY_MS 1800 // how long the robot celebrates a new top speed
#endif
#ifndef ROBOT_RECORD_MIN_WPM
#    define ROBOT_RECORD_MIN_WPM 30 // top speeds below this do not count as a celebration
#endif
#ifndef ROBOT_RECORD_COOLDOWN_MS
#    define ROBOT_RECORD_COOLDOWN_MS 8000 // minimum time between two celebrations
#endif
#ifndef ROBOT_FAST_FRAME_MS
#    define ROBOT_FAST_FRAME_MS 120 // how long each of the two fast-typing frames is shown
#endif
#ifndef LEFT_GRAPH_MAX_WPM
#    define LEFT_GRAPH_MAX_WPM 120 // WPM that fills the whole graph on the left display
#endif
#define ROBOT_SCALE 4
#define ROBOT_X ((135 - ROBOT_W * ROBOT_SCALE) / 2)
#define ROBOT_Y 40
#define ROBOT_STATS_Y 172 // WPM / TOP labels; the numbers sit 18 px below
#define HIST_N 18         // keys remembered for the history screen (3 columns x 6 rows)

#define WPM_CHART_WIDTH 135
#ifdef ROBOT_UI_ENABLE
#    define WPM_CHART_HEIGHT 60 // chart sits below the UPTIME / BKSP numbers (WPM / TOP live on the right display)
#else
#    define WPM_CHART_HEIGHT 70
#endif

#define LCD_HEIGHT 240
#define LCD_WIDTH 135
#define LCD_SPLASH_DURATION 7000

#ifndef LCD_OFFSET_X
#    define LCD_OFFSET_X 52
#endif
#ifndef LCD_OFFSET_Y
#    define LCD_OFFSET_Y 40
#endif

__attribute__((aligned(4))) static uint8_t surface_buffer[SURFACE_REQUIRED_BUFFER_BYTE_SIZE(LCD_WIDTH, LCD_HEIGHT, 16)];

painter_device_t      lcd;
painter_device_t      surface __attribute__((used));
painter_font_handle_t pixellari_18;
painter_font_handle_t pixellari_24;

static hsv_t ui_hsv = {129, 189, 181};
static hsv_t last_hsv = {0, 0, 255};

static deferred_token display_task_token __attribute__((used));
static bool splash_active;
static uint32_t splash_start;

#ifdef ROBOT_UI_ENABLE
// Robot UI state.
static uint8_t       session_top_wpm;           // highest WPM since power-up (not saved)
static volatile uint16_t robot_keys;            // key presses since power-up (master: counted, slave: synced)
static volatile uint8_t  robot_last_hand;       // 0 = left half pressed last, 1 = right half
static volatile uint16_t typed_keys;            // keys that could be shown in the history (master: counted, slave: synced)
static volatile uint16_t backspace_keys;        // how many of those were Backspace
static uint32_t      stats_uptime_drawn = 0xFFFFFFFF;
static int8_t        stats_bsp_drawn = -2;
static volatile uint8_t  key_history[HIST_N];   // newest first, HID keycodes, 0 = empty (master: recorded, slave: synced)
typedef enum { SCREEN_ROBOT, SCREEN_HISTORY, SCREEN_RGB } right_screen_t;
static right_screen_t right_screen = SCREEN_ROBOT; // what the right display is showing
static uint8_t       robot_wpm_drawn = 0xFF;
static uint8_t       robot_top_drawn = 0xFF;
static uint16_t      history_keys_drawn = 0xFFFF;
static uint8_t       robot_pose = ROBOT_POSE_IDLE;
static uint8_t       robot_pose_drawn = 0xFF;
static uint16_t      robot_keys_seen;
static uint32_t      robot_last_key_time;
static uint32_t      robot_press_until;
static uint32_t      robot_happy_until;
static uint32_t      robot_record_time;
static bool          robot_record_valid;
static uint32_t      robot_blink_until;
static uint32_t      robot_next_blink;
#endif


//----------------------------------------------------------
// RGB Matrix naming
#undef RGB_MATRIX_EFFECT
#ifdef RGB_MATRIX_MODE_NAME_ENABLE
const char *rgb_matrix_get_mode_name(uint8_t mode) {
    switch (mode) {
        case RGB_MATRIX_NONE:
            return "NONE";

#    define RGB_MATRIX_EFFECT(name, ...) \
        case RGB_MATRIX_##name:          \
            return #name;
#    include "rgb_matrix_effects.inc"
#    undef RGB_MATRIX_EFFECT

#    ifdef COMMUNITY_MODULES_ENABLE
#        define RGB_MATRIX_EFFECT(name, ...)         \
            case RGB_MATRIX_COMMUNITY_MODULE_##name: \
                return #name;
#        include "rgb_matrix_community_modules.inc"
#        undef RGB_MATRIX_EFFECT
#    endif // COMMUNITY_MODULES_ENABLE

#    if defined(RGB_MATRIX_CUSTOM_KB) || defined(RGB_MATRIX_CUSTOM_USER)
#        define RGB_MATRIX_EFFECT(name, ...) \
            case RGB_MATRIX_CUSTOM_##name:   \
                return #name;

#        ifdef RGB_MATRIX_CUSTOM_KB
#            include "rgb_matrix_kb.inc"
#        endif // RGB_MATRIX_CUSTOM_KB

#        ifdef RGB_MATRIX_CUSTOM_USER
#            include "rgb_matrix_user.inc"
#        endif // RGB_MATRIX_CUSTOM_USER

#        undef RGB_MATRIX_EFFECT
#    endif // RGB_MATRIX_CUSTOM_KB || RGB_MATRIX_CUSTOM_USER

        default:
            return "UNKNOWN";
    }
}
#    undef RGB_MATRIX_EFFECT
#endif // RGB_MATRIX_MODE_NAME_ENABLE

void drawtext_right_recolor(painter_device_t device, uint16_t y, uint8_t width, painter_font_handle_t font, const char *str, uint8_t hue_fg, uint8_t sat_fg, uint8_t val_fg, uint8_t hue_bg, uint8_t sat_bg, uint8_t val_bg) {
    qp_drawtext_recolor(surface, width - qp_textwidth(font, str), y, font, str, hue_fg, sat_fg, val_fg, hue_bg, sat_bg, val_bg);
}

void drawtext_centered_recolor(painter_device_t device, uint16_t x, uint16_t y, uint8_t width, painter_font_handle_t font, const char *str, uint8_t hue_fg, uint8_t sat_fg, uint8_t val_fg, uint8_t hue_bg, uint8_t sat_bg, uint8_t val_bg) {
    qp_drawtext_recolor(surface, (x + (width / 2)) - qp_textwidth(font, str) / 2, y, font, str, hue_fg, sat_fg, val_fg, hue_bg, sat_bg, val_bg);
}

void drawtext_centered(painter_device_t device, uint16_t x, uint16_t y, uint8_t width, painter_font_handle_t font, const char *str) {
    drawtext_centered_recolor(device, x, y, width, font, str, 255, 0, 255, 0, 0, 0);
}

void drawtext_layer(uint16_t x, uint16_t y, uint8_t width, const char *str, uint8_t layer) {
    if (layer == get_highest_layer(layer_state)) {
        drawtext_centered_recolor(surface, x, y, width, pixellari_24, str, 255, 0, 255, ui_hsv.h, ui_hsv.s, ui_hsv.v);
    } else {
        drawtext_centered_recolor(surface, x, y, width, pixellari_24, str, 255, 0, 255, 0, 0, 0);
    }
}

void clear_display(void) {
    qp_rect(surface, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
}

void draw_lock_indicators(void) {
    led_t leds = host_keyboard_led_state();
    uint8_t caps_hue = leds.caps_lock ? ui_hsv.h : 0;
    uint8_t caps_sat = leds.caps_lock ? ui_hsv.s : 0;
    uint8_t caps_val = leds.caps_lock ? ui_hsv.v : 80;
    uint8_t num_hue  = leds.num_lock ? ui_hsv.h : 0;
    uint8_t num_sat  = leds.num_lock ? ui_hsv.s : 0;
    uint8_t num_val  = leds.num_lock ? ui_hsv.v : 80;

    drawtext_centered_recolor(surface, 0, 193, 38, pixellari_18, "CAP", caps_hue, caps_sat, caps_val, 0, 0, 0);
    drawtext_centered_recolor(surface, 97, 193, 38, pixellari_18, "NUM", num_hue, num_sat, num_val, 0, 0, 0);
}

void draw_layers(void) {
    drawtext_centered(surface, 0, 10, 135, pixellari_24, "LAYER");
    drawtext_layer(0, 45, 32, "1", 0);
    drawtext_layer(34, 45, 32, "2", 1);
    drawtext_layer(66, 45, 32, "3", 2);
    drawtext_layer(98, 45, 32, "4", 3);
}

void draw_os(bool init) {
#ifdef ROBOT_UI_ENABLE
    (void)init; // no "OS" header and no CAP/NUM: the OS name sits below the modifier row
#else
    if (init) {
        drawtext_centered(surface, 0, 190, 135, pixellari_24, "OS");
        draw_lock_indicators();
    }
#endif
    char *os_name;
    switch (detected_host_os()) {
        case OS_MACOS:
            os_name = "MacOS";
            break;
        case OS_IOS:
            os_name = "Apple";
            break;
        case OS_WINDOWS:
            os_name = "Windows";
            break;
        case OS_LINUX:
            os_name = "Linux";
            break;
        default:
            os_name = "Unsure";
            break;
    }
#ifdef ROBOT_UI_ENABLE
    const uint16_t os_y = 214;
#else
    const uint16_t os_y = 220;
#endif
    qp_rect(surface, 0, os_y, LCD_WIDTH - 1, os_y + pixellari_18->line_height, 0, 0, 0, true);
    drawtext_centered_recolor(surface, 0, os_y, 135, pixellari_18, os_name, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
}

#ifdef ROBOT_UI_ENABLE
// Backspace share of typed keys in percent, -1 until there are enough keys to be meaningful.
static int8_t bsp_percent(void) {
    if (typed_keys < 10) {
        return -1;
    }
    return (int8_t)(((uint32_t)backspace_keys * 100) / typed_keys);
}

// Two columns under the layer row: UPTIME since power-up and BKSP (backspace rate).
void draw_wpm_text(void) {
    char     buffer[16] = {0};
    uint32_t up         = timer_read32() / 1000;
    int8_t   bp         = bsp_percent();
    qp_rect(surface, 0, 72, LCD_WIDTH - 1, 72 + pixellari_18->line_height + pixellari_24->line_height, 0, 0, 0, true);
    drawtext_centered_recolor(surface, 0, 72, 67, pixellari_18, "UPTIME", 0, 0, 150, 0, 0, 0);
    drawtext_centered_recolor(surface, 68, 72, 67, pixellari_18, "BKSP", ui_hsv.h, ui_hsv.s, ui_hsv.v / 2, 0, 0, 0);
    if (up >= 3600) {
        snprintf(buffer, sizeof(buffer), "%luh%02lu", (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
    } else {
        snprintf(buffer, sizeof(buffer), "%lu:%02lu", (unsigned long)(up / 60), (unsigned long)(up % 60));
    }
    drawtext_centered(surface, 0, 90, 67, pixellari_24, buffer);
    if (bp < 0) {
        snprintf(buffer, sizeof(buffer), "--");
    } else {
        snprintf(buffer, sizeof(buffer), "%d%%", bp);
    }
    drawtext_centered_recolor(surface, 68, 90, 67, pixellari_24, buffer, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    stats_uptime_drawn = up;
    stats_bsp_drawn    = bp;
}

// Shift / Ctrl / Alt / Gui: filled while held (left or right key, or one-shot).
static const struct {
    const char *label;
    uint8_t     mask;
} mod_cells[4] = {{"SFT", MOD_MASK_SHIFT}, {"CTL", MOD_MASK_CTRL}, {"ALT", MOD_MASK_ALT}, {"GUI", MOD_MASK_GUI}};

#    define MOD_ROW_Y 186
#    define MOD_ROW_H 21

static uint8_t mods_drawn = 0xFF;

static void draw_mod_cell(uint8_t i, bool on) {
    const uint16_t x = i * 34;
    if (on) {
        qp_rect(surface, x, MOD_ROW_Y, x + 32, MOD_ROW_Y + MOD_ROW_H - 1, ui_hsv.h, ui_hsv.s, ui_hsv.v, true);
        drawtext_centered_recolor(surface, x, MOD_ROW_Y + 2, 33, pixellari_18, mod_cells[i].label, ui_hsv.h, ui_hsv.s, 0, ui_hsv.h, ui_hsv.s, ui_hsv.v);
    } else {
        qp_rect(surface, x, MOD_ROW_Y, x + 32, MOD_ROW_Y + MOD_ROW_H - 1, 0, 0, 0, true);
        qp_rect(surface, x, MOD_ROW_Y, x + 32, MOD_ROW_Y + MOD_ROW_H - 1, ui_hsv.h, ui_hsv.s, ui_hsv.v / 4, false);
        drawtext_centered_recolor(surface, x, MOD_ROW_Y + 2, 33, pixellari_18, mod_cells[i].label, ui_hsv.h, ui_hsv.s, ui_hsv.v / 3, 0, 0, 0);
    }
}

// force = redraw all four cells (after the screen was cleared or recoloured).
void draw_mods(bool force) {
    uint8_t mods = get_mods() | get_oneshot_mods();
    if (force) {
        mods_drawn = 0xFF;
    }
    for (uint8_t i = 0; i < 4; i++) {
        bool on = (mods & mod_cells[i].mask) != 0;
        if (force || on != ((mods_drawn & mod_cells[i].mask) != 0)) {
            draw_mod_cell(i, on);
        }
    }
    mods_drawn = mods;
}
#else
void draw_wpm_text(void) {
    char buffer[64] = {0};
    snprintf(buffer, sizeof(buffer), "WPM:%d", get_current_wpm());
    qp_rect(surface, 0, 80, LCD_WIDTH - 1, 80 + pixellari_24->line_height, 0, 0, 0, true);
    drawtext_centered(surface, 0, 80, 135, pixellari_24, buffer);
}
#endif

struct {
    uint8_t start;
    uint8_t values[WPM_CHART_WIDTH];
} wpm_chart;

void wpm_chart_next(void) {
    wpm_chart.start = (wpm_chart.start + 1) % WPM_CHART_WIDTH;
}

void wpm_chart_write_value(uint8_t value) {
    wpm_chart.values[wpm_chart.start] = value;
#ifdef ROBOT_UI_ENABLE
    uint16_t graph_value = (uint16_t)wpm_chart.values[wpm_chart.start] * 255 / LEFT_GRAPH_MAX_WPM;
    uint8_t  scaled_value = scale8(WPM_CHART_HEIGHT, graph_value > 255 ? 255 : graph_value);
#else
    uint8_t scaled_value              = scale8(WPM_CHART_HEIGHT, wpm_chart.values[wpm_chart.start]);
#endif
    qp_line(surface, wpm_chart.start, LCD_HEIGHT - 1 - 60, wpm_chart.start, (LCD_HEIGHT - 1 - 60) - WPM_CHART_HEIGHT, 0, 0, 0);
    qp_line(surface, wpm_chart.start, LCD_HEIGHT - 1 - 60, wpm_chart.start, (LCD_HEIGHT - 1 - 60) - scaled_value, ui_hsv.h, ui_hsv.s, ui_hsv.v);
    wpm_chart_next();
}

void wpm_chart_init(void) {
    memset(wpm_chart.values, 0, sizeof(wpm_chart.values));
    wpm_chart.start = 0;
}

// void draw_wpm_chart(bool init) {
//     if (init) {
//         wpm_chart_init();
//     }
//     qp_rect(surface, 0, LCD_HEIGHT - WPM_CHART_HEIGHT - 10, LCD_WIDTH - 1, LCD_HEIGHT - 1 - 10, 0, 0, 0, true);
//     for (uint8_t i = 0; i < WPM_CHART_WIDTH; i++) {
//         uint8_t location     = (wpm_chart.start + i) % WPM_CHART_WIDTH;
//         uint8_t scaled_value = scale8(WPM_CHART_HEIGHT, wpm_chart.values[location]);
//         qp_line(surface, i, LCD_HEIGHT - 1 - 10, i, (LCD_HEIGHT - 1 - 10) - scaled_value, ui_hsv.h, ui_hsv.s, ui_hsv.v);
//     }
// }

void draw_wpm_chart_2(bool init) {
    if (init) {
        wpm_chart_init();
    }
}

void wpm_layer_display_init(void) {
    clear_display();
    draw_layers();
    draw_wpm_text();
    draw_wpm_chart_2(true);
}

void draw_bar(uint8_t value, uint8_t max_value, uint8_t left, uint8_t top, uint8_t max_length, uint8_t height) {
    uint8_t bar_length = (((max_length << 8) / max_value) * value) >> 8;
    qp_rect(surface, left, top, left + max_length, top + height, 0, 0, 0, true);
    qp_rect(surface, left, top, left + max_length, top + height, ui_hsv.h, ui_hsv.s, ui_hsv.v, false);
    qp_rect(surface, left, top, left + bar_length, top + height, ui_hsv.h, ui_hsv.s, ui_hsv.v, true);
}

void draw_rgb_text(bool init) {
    char buffer[64] = {0};
    if (init) {
        snprintf(buffer, sizeof(buffer), "RGB");
        drawtext_centered(surface, 0, 10, 135, pixellari_24, buffer);

        qp_drawtext_recolor(surface, 0, 110, pixellari_18, "Hue:", 0, 0, 255, 0, 0, 0);
        qp_drawtext_recolor(surface, 0, 130, pixellari_18, "Sat:", 0, 0, 255, 0, 0, 0);
        qp_drawtext_recolor(surface, 0, 150, pixellari_18, "Val:", 0, 0, 255, 0, 0, 0);
    }

    qp_rect(surface, LCD_WIDTH / 2, 110, LCD_WIDTH - 1, 110 + pixellari_18->line_height, 0, 0, 0, true);
    snprintf(buffer, sizeof(buffer), "%d", rgb_matrix_get_hue());
    drawtext_right_recolor(surface, 110, LCD_WIDTH, pixellari_18, buffer, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    // snprintf(buffer, sizeof(buffer), "%d",rgb_matrix_get_sat());
    // drawtext_right_recolor(surface, 130, LCD_WIDTH, pixellari_18, buffer, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    // snprintf(buffer, sizeof(buffer), "%d",rgb_matrix_get_val());
    // drawtext_right_recolor(surface, 150, LCD_WIDTH, pixellari_18, buffer, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);

    draw_bar(rgb_matrix_get_sat(), 255, 45, 130, LCD_WIDTH - 1 - 45, pixellari_18->line_height);
    draw_bar(rgb_matrix_get_val(), RGB_MATRIX_MAXIMUM_BRIGHTNESS, 45, 150, LCD_WIDTH - 1 - 45, pixellari_18->line_height);

    char *mode_name = strdup(rgb_matrix_get_mode_name(rgb_matrix_get_mode()));
    if (mode_name != NULL) {
        int     len               = strlen(mode_name);
        int     underscore_loc[2] = {-1, -1};
        uint8_t underscore_pos    = 0;
        bool    capitalize_next   = true;
        for (int i = 0; i < len; i++) {
            if (mode_name[i] == '_') {
                if (underscore_pos < 2) {
                    mode_name[i]                   = '\0';
                    underscore_loc[underscore_pos] = i;
                    underscore_pos++;
                } else {
                    mode_name[i] = ' ';
                }
                capitalize_next = true;
            } else if (capitalize_next) {
                mode_name[i]    = mode_name[i] >= 'a' && mode_name[i] <= 'z' ? mode_name[i] - 'a' + 'A' : mode_name[i];
                capitalize_next = false;
            } else {
                mode_name[i] = mode_name[i] >= 'A' && mode_name[i] <= 'Z' ? mode_name[i] - 'A' + 'a' : mode_name[i];
            }
        }

        qp_rect(surface, 0, 30, LCD_WIDTH - 1, 80 + pixellari_18->line_height, 0, 0, 0, true);
        drawtext_centered_recolor(surface, 0, 40, 135, pixellari_18, &mode_name[0], ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
        if (underscore_loc[0] > 0) {
            drawtext_centered_recolor(surface, 0, 60, 135, pixellari_18, &mode_name[underscore_loc[0] + 1], ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
        }
        if (underscore_loc[1] > 0) {
            drawtext_centered_recolor(surface, 0, 80, 135, pixellari_18, &mode_name[underscore_loc[1] + 1], ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
        }
        free(mode_name);
    }
}

void draw_bl_text(bool init) {
    char buffer[64] = {0};
    if (init) {
        snprintf(buffer, sizeof(buffer), "LCD");
        drawtext_centered(surface, 0, 190, 135, pixellari_24, buffer);
        qp_drawtext_recolor(surface, 0, 220, pixellari_18, "Bri:", 0, 0, 255, 0, 0, 0);
    }

    draw_bar(get_backlight_level(), BACKLIGHT_LEVELS, 45, 220, LCD_WIDTH - 1 - 45, pixellari_18->line_height);
}

void rgb_bl_display_init(void) {
    clear_display();
    draw_rgb_text(true);
    draw_bl_text(true);
}

#ifdef ROBOT_UI_ENABLE
//----------------------------------------------------------
// Right display: typing-reactive robot.

// Palette index -> HSV. Index 1 follows the keyboard's RGB hue; white and pink stay fixed.
static hsv_t robot_color(uint8_t idx) {
    switch (idx) {
        case 1:
            return ui_hsv;
        case 2:
            return (hsv_t){ui_hsv.h, ui_hsv.s, (uint8_t)((uint16_t)ui_hsv.v * 32 / 100)};
        case 3:
            return (hsv_t){ui_hsv.h, ui_hsv.s, (uint8_t)((uint16_t)ui_hsv.v * 7 / 100)};
        case 4:
            return (hsv_t){0, 0, 255};
        case 5:
            return (hsv_t){234, 215, 255};
        default:
            return (hsv_t){ui_hsv.h, ui_hsv.s, (uint8_t)((uint16_t)ui_hsv.v * 55 / 100)};
    }
}

static void robot_draw(uint8_t pose) {
    qp_rect(surface, ROBOT_X, ROBOT_Y, ROBOT_X + ROBOT_W * ROBOT_SCALE - 1, ROBOT_Y + ROBOT_H * ROBOT_SCALE - 1, 0, 0, 0, true);
    const uint8_t *frame = robot_frames[pose];
    for (uint8_t y = 0; y < ROBOT_H; y++) {
        uint8_t x = 0;
        while (x < ROBOT_W) {
            uint8_t idx = frame[y * ROBOT_W + x];
            if (idx == 0) {
                x++;
                continue;
            }
            uint8_t x_end = x;
            while (x_end + 1 < ROBOT_W && frame[y * ROBOT_W + x_end + 1] == idx) {
                x_end++;
            }
            hsv_t c = robot_color(idx);
            qp_rect(surface, ROBOT_X + x * ROBOT_SCALE, ROBOT_Y + y * ROBOT_SCALE, ROBOT_X + (x_end + 1) * ROBOT_SCALE - 1, ROBOT_Y + (y + 1) * ROBOT_SCALE - 1, c.h, c.s, c.v, true);
            x = x_end + 1;
        }
    }
    robot_pose_drawn = pose;
}

// WPM and TOP numbers under the robot (redrawn only when a value changes).
static void robot_draw_stats(void) {
    char buffer[16] = {0};
    qp_rect(surface, 0, ROBOT_STATS_Y, LCD_WIDTH - 1, ROBOT_STATS_Y + pixellari_18->line_height + pixellari_24->line_height, 0, 0, 0, true);
    drawtext_centered_recolor(surface, 0, ROBOT_STATS_Y, 67, pixellari_18, "WPM", 0, 0, 150, 0, 0, 0);
    drawtext_centered_recolor(surface, 68, ROBOT_STATS_Y, 67, pixellari_18, "TOP", ui_hsv.h, ui_hsv.s, ui_hsv.v / 2, 0, 0, 0);
    uint8_t wpm = get_current_wpm();
    snprintf(buffer, sizeof(buffer), "%d", wpm);
    drawtext_centered(surface, 0, ROBOT_STATS_Y + 18, 67, pixellari_24, buffer);
    snprintf(buffer, sizeof(buffer), "%d", session_top_wpm);
    drawtext_centered_recolor(surface, 68, ROBOT_STATS_Y + 18, 67, pixellari_24, buffer, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    robot_wpm_drawn = wpm;
    robot_top_drawn = session_top_wpm;
}

// Decide which pose the robot should be in right now.
static void robot_update_pose(void) {
    uint32_t now = timer_read32();
    uint8_t  wpm = get_current_wpm();

    uint16_t keys = robot_keys;
    if (keys != robot_keys_seen) {
        robot_keys_seen     = keys;
        robot_last_key_time = now;
        robot_press_until   = now + ROBOT_PRESS_MS;
    }

    if (now < robot_happy_until) {
        robot_pose = ROBOT_POSE_HAPPY;
    } else if (wpm >= ROBOT_FAST_WPM && now - robot_last_key_time < 700) {
        robot_pose = ((now / ROBOT_FAST_FRAME_MS) & 1) ? ROBOT_POSE_FAST_B : ROBOT_POSE_FAST_A; // hands take turns
    } else if (now < robot_press_until) {
        robot_pose = robot_last_hand ? ROBOT_POSE_TYPE_R : ROBOT_POSE_TYPE_L;
    } else if (now - robot_last_key_time > ROBOT_SLEEP_MS) {
        robot_pose = ROBOT_POSE_SLEEP;
    } else {
        if (now >= robot_next_blink) {
            robot_blink_until = now + 150;
            robot_next_blink  = now + 2500 + (now * 2654435761u >> 20) % 3000; // 2.5 - 5.5 s
        }
        robot_pose = now < robot_blink_until ? ROBOT_POSE_BLINK : ROBOT_POSE_IDLE;
    }
}

static void robot_screen_init(void) {
    clear_display();
    robot_update_pose();
    robot_draw(robot_pose);
    robot_draw_stats();
}

static void robot_task(void) {
    robot_update_pose();
    if (robot_pose != robot_pose_drawn) {
        robot_draw(robot_pose);
    }
    if (get_current_wpm() != robot_wpm_drawn || session_top_wpm != robot_top_drawn) {
        robot_draw_stats();
    }
}

//----------------------------------------------------------
// Right display on layers 1 / 2: history of the last typed keys.

// Short label for a HID keycode; returns false for keys that are not shown.
static bool key_label(uint8_t kc, char *out) {
    static const char *const names[] = {
        [KC_ENTER - KC_ENTER] = "ENT", "ESC", "BSP", "TAB", "SPC", "-", "=", "[", "]", "\\", "#", ";", "'", "`", ",", ".", "/", "CAP",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "PRT", "SCR", "PAU", "INS", "HOM", "PGU", "DEL", "END", "PGD", "RGT", "LFT", "DWN", "UP",
    };
    if (kc >= KC_A && kc <= KC_Z) {
        out[0] = 'A' + (kc - KC_A);
        out[1] = 0;
    } else if (kc >= KC_1 && kc <= KC_0) {
        out[0] = kc == KC_0 ? '0' : '1' + (kc - KC_1);
        out[1] = 0;
    } else if (kc >= KC_ENTER && kc < KC_ENTER + sizeof(names) / sizeof(names[0])) {
        strcpy(out, names[kc - KC_ENTER]);
    } else {
        return false;
    }
    return true;
}

static void history_draw(void) {
    qp_rect(surface, 0, 40, LCD_WIDTH - 1, LCD_HEIGHT - 1, 0, 0, 0, true);
    for (uint8_t i = 0; i < HIST_N; i++) {
        char label[8];
        if (!key_label(key_history[i], label)) {
            continue;
        }
        const uint16_t x = (i % 3) * 45;
        const uint16_t y = 44 + (i / 3) * 32;
        if (i == 0) { // newest key: filled box
            qp_rect(surface, x + 1, y - 2, x + 43, y + 20, ui_hsv.h, ui_hsv.s, ui_hsv.v, true);
            drawtext_centered_recolor(surface, x, y, 45, pixellari_18, label, ui_hsv.h, ui_hsv.s, 0, ui_hsv.h, ui_hsv.s, ui_hsv.v);
        } else { // older keys fade out
            uint8_t v = 255 - i * 12;
            drawtext_centered_recolor(surface, x, y, 45, pixellari_18, label, 0, 0, v, 0, 0, 0);
        }
    }
    history_keys_drawn = robot_keys;
}

static void history_screen_init(void) {
    clear_display();
    drawtext_centered(surface, 0, 10, LCD_WIDTH, pixellari_24, "KEYS");
    history_draw();
}

static void history_task(void) {
    if (robot_keys != history_keys_drawn) {
        history_draw();
    }
}

// Track the session top speed; a new record (above ROBOT_RECORD_MIN_WPM) makes the robot happy.
static void top_wpm_update(void) {
    uint8_t wpm = get_current_wpm();
    // The WPM window is still filling up in the first seconds after power-up and reads far too high then.
    if (timer_read32() < (uint32_t)WPM_SAMPLE_SECONDS * 1000) {
        return;
    }
    if (wpm > session_top_wpm) {
        session_top_wpm = wpm;
        uint32_t now    = timer_read32();
        if (wpm >= ROBOT_RECORD_MIN_WPM && (!robot_record_valid || timer_elapsed32(robot_record_time) > ROBOT_RECORD_COOLDOWN_MS)) {
            robot_record_valid = true;
            robot_record_time  = now;
            robot_happy_until  = now + ROBOT_HAPPY_MS;
        }
    }
}

//----------------------------------------------------------
// Key press info for the robot: counted on the master half, sent to the other half.
typedef struct {
    uint16_t keys;
    uint8_t  right_hand;
    uint8_t  history[HIST_N];
    uint16_t typed;
    uint16_t backspaces;
} robot_sync_t;

static bool         robot_sync_dirty;

static void robot_sync_handler(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    if (in_len == sizeof(robot_sync_t)) {
        const robot_sync_t *d = (const robot_sync_t *)in_data;
        robot_last_hand       = d->right_hand;
        for (uint8_t i = 0; i < HIST_N; i++) {
            key_history[i] = d->history[i];
        }
        typed_keys     = d->typed;
        backspace_keys = d->backspaces;
        robot_keys     = d->keys;
    }
}

// Called on the master half after every key press is processed.
void display_note_keypress(uint16_t keycode, uint8_t tap_count, bool right_hand) {
    uint8_t kc = KC_NO;
    if (IS_QK_MOD_TAP(keycode) || IS_QK_LAYER_TAP(keycode)) {
        if (tap_count == 0) {
            return; // the key is being held, not typed
        }
        kc = QK_MOD_TAP_GET_TAP_KEYCODE(keycode);
    } else if (IS_QK_MODS(keycode)) {
        kc = QK_MODS_GET_BASIC_KEYCODE(keycode);
    } else if (IS_BASIC_KEYCODE(keycode)) {
        kc = keycode;
    }
    char label[8];
    if (kc != KC_NO && key_label(kc, label)) {
        for (uint8_t i = HIST_N - 1; i > 0; i--) {
            key_history[i] = key_history[i - 1];
        }
        key_history[0] = kc;
        typed_keys     = typed_keys + 1;
        if (kc == KC_BSPC) {
            backspace_keys = backspace_keys + 1;
        }
        if (typed_keys >= 60000) { // keep the ratio, avoid overflow
            typed_keys     = typed_keys / 2;
            backspace_keys = backspace_keys / 2;
        }
    }
    robot_last_hand  = right_hand ? 1 : 0;
    robot_keys       = robot_keys + 1;
    robot_sync_dirty = true;
}

// Call from housekeeping_task_kb(); sends the latest key info to the other half.
void display_sync_task(void) {
    if (!is_keyboard_master() || !robot_sync_dirty) {
        return;
    }
    static uint32_t next_try = 0;
    if ((int32_t)(timer_read32() - next_try) < 0) {
        return;
    }
    robot_sync_t d = {.keys = robot_keys, .right_hand = robot_last_hand, .typed = typed_keys, .backspaces = backspace_keys};
    for (uint8_t i = 0; i < HIST_N; i++) {
        d.history[i] = key_history[i];
    }
    if (transaction_rpc_send(KB_SYNC_ROBOT, sizeof(d), &d)) {
        robot_sync_dirty = false;
        next_try         = timer_read32() + 15; // at most ~65 updates per second
    } else {
        next_try = timer_read32() + 500; // other half not there: back off so key scanning is not slowed
    }
}
#else
void display_note_keypress(uint16_t keycode, uint8_t tap_count, bool right_hand) {}
void display_sync_task(void) {}
#endif // ROBOT_UI_ENABLE

#ifdef ROBOT_UI_ENABLE
// Which screen the right display wants for the current layer.
static right_screen_t right_screen_for_layer(void) {
    uint8_t layer = get_highest_layer(layer_state);
    if (layer == ROBOT_RGB_LAYER) {
        return SCREEN_RGB;
    }
    return layer == 0 ? SCREEN_ROBOT : SCREEN_HISTORY;
}
#endif

// Delay until the next display task run.
static uint32_t next_interval(void) {
#ifdef ROBOT_UI_ENABLE
    if (!is_keyboard_left() && right_screen != SCREEN_RGB) {
        return ROBOT_TICK_MS;
    }
#endif
    return 100;
}

uint32_t display_task_callback(uint32_t trigger_time, void *cb_arg) {
    display_task_kb();
    return next_interval();
}

void display_test(void) {
    qp_rect(surface, 0, 0, 134, 239, 255, 0, 255, true);
    qp_rect(surface, 64, 0, 134, 239, 255, 0, 0, true);

    // Draw 8px-wide rainbow filled rectangles down the left side of the display
    for (int i = 0; i < 239; i += 8) {
        qp_rect(surface, 0, i, 7, i + 7, i, 255, 255, true);
        qp_rect(surface, 127, i, 134, i + 7, i, 255, 255, true);
    }
}

__attribute__((weak)) bool display_init_user(void) {
    return true;
}

void display_ui_init(void) {
    if (is_keyboard_left()) {
        wpm_layer_display_init();
        draw_os(true);
#ifdef ROBOT_UI_ENABLE
        draw_mods(true);
#endif
    } else {
#ifdef ROBOT_UI_ENABLE
        right_screen = right_screen_for_layer();
        if (right_screen == SCREEN_RGB) {
            rgb_bl_display_init();
        } else if (right_screen == SCREEN_HISTORY) {
            history_screen_init();
        } else {
            robot_screen_init();
        }
#else
        rgb_bl_display_init();
#endif
    }
}

void display_splash_init(void) {
    clear_display();
    char  keyboard_name[] = PRODUCT;
    char *name_lines[3]   = {keyboard_name, NULL, NULL};
    uint8_t line_count    = 1;
    for (size_t i = 0; keyboard_name[i] != '\0' && line_count < 3; i++) {
        if (keyboard_name[i] == ' ') {
            keyboard_name[i]       = '\0';
            name_lines[line_count] = &keyboard_name[i + 1];
            line_count++;
        }
    }
    drawtext_centered(surface, 0, 26, LCD_WIDTH, pixellari_18, "PRODUCT");
    uint16_t name_top = 47 + (61 - line_count * pixellari_18->line_height) / 2;
    for (uint8_t i = 0; i < line_count; i++) {
        drawtext_centered_recolor(surface, 0, name_top + i * pixellari_18->line_height, LCD_WIDTH, pixellari_18, name_lines[i], ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    }
    char build_date[] = QMK_BUILDDATE;
    build_date[10]    = '\0';
    drawtext_centered(surface, 0, 135, LCD_WIDTH, pixellari_18, "COMPILE DATE");
    drawtext_centered_recolor(surface, 0, 155, LCD_WIDTH, pixellari_18, build_date, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
    drawtext_centered(surface, 0, 184, LCD_WIDTH, pixellari_18, "COMMIT");
    drawtext_centered_recolor(surface, 0, 204, LCD_WIDTH, pixellari_18, QMK_GIT_HASH, ui_hsv.h, ui_hsv.s, ui_hsv.v, 0, 0, 0);
}

__attribute__((weak)) painter_rotation_t get_display_rotation(void) {
    return QP_ROTATION_0;
}

void display_init_kb(void) {
    // Initialise the surface
    lcd = qp_st7789_make_spi_device(LCD_WIDTH, LCD_HEIGHT, VIK_CS, VIK_GPIO1, VIK_GPIO2, 4, 3);
    qp_init(lcd, get_display_rotation());
    qp_set_viewport_offsets(surface, LCD_OFFSET_X, LCD_OFFSET_Y);
    surface = qp_make_rgb565_surface(LCD_WIDTH, LCD_HEIGHT, surface_buffer);
    qp_init(surface, QP_ROTATION_0);
    // Load fonts
    pixellari_18 = qp_load_font_mem(font_pixellari18);
    pixellari_24 = qp_load_font_mem(font_pixellari24);
    if (!display_init_user()) {
        return;
    }

    last_hsv.h = rgb_matrix_get_hue();
    last_hsv.s = rgb_matrix_get_sat();

    ui_hsv = last_hsv;

    if (is_keyboard_master()) {
        splash_active = true;
        splash_start  = timer_read32();
        display_splash_init();
    } else {
        display_ui_init();
    }

    qp_surface_draw(surface, lcd, LCD_OFFSET_X, LCD_OFFSET_Y, false);

#ifdef ROBOT_UI_ENABLE
    transaction_register_rpc(KB_SYNC_ROBOT, robot_sync_handler);
#endif

    display_task_token = defer_exec(2000, display_task_callback, NULL);
}

__attribute__((weak)) bool display_task_user(void) {
    return true;
}

void display_task_kb(void) {
    if (!display_task_user()) {
        return;
    }

#ifdef ROBOT_UI_ENABLE
    top_wpm_update();
#endif

    if (splash_active) {
        if (timer_elapsed32(splash_start) < LCD_SPLASH_DURATION) {
            return;
        }
        splash_active = false;
        display_ui_init();
    }

    if (last_hsv.h != rgb_matrix_get_hue() || last_hsv.s != rgb_matrix_get_sat()) {
        last_hsv.h = rgb_matrix_get_hue();
        last_hsv.s = rgb_matrix_get_sat();
        ui_hsv      = last_hsv;
        display_ui_init();
    }

    static uint32_t timer = 0;

    if (is_keyboard_left()) {
        if (timer_elapsed(timer) > 100) {
            static uint32_t lastwpm = 0;
            static uint32_t currwpm = 0;

            timer   = timer_read();
            lastwpm = currwpm;
            currwpm = get_current_wpm();

#ifndef ROBOT_UI_ENABLE
            if (lastwpm != currwpm) {
                draw_wpm_text();
            }
            draw_lock_indicators();
#else
            (void)lastwpm;
            if (timer_read32() / 1000 != stats_uptime_drawn || bsp_percent() != stats_bsp_drawn) {
                draw_wpm_text();
            }
#endif
            wpm_chart_write_value(currwpm);
            draw_wpm_chart_2(false);
        }

        static layer_state_t currlay = 0;
        static layer_state_t lastlay = 0;

        currlay = get_highest_layer(layer_state);
        if (currlay != lastlay) {
            lastlay = currlay;
            draw_layers();
        }

#ifdef ROBOT_UI_ENABLE
        draw_mods(false);
#endif

        static os_variant_t last_os = OS_UNSURE;

        if (detected_host_os() != last_os) {
            draw_os(false);
            last_os = detected_host_os();
        }

    } else {
#ifdef ROBOT_UI_ENABLE
        if (right_screen_for_layer() != right_screen) {
            display_ui_init(); // switch between robot / key history / RGB info
        }
        if (right_screen != SCREEN_RGB) {
            if (right_screen == SCREEN_ROBOT) {
                robot_task();
            } else {
                history_task();
            }
            qp_surface_draw(surface, lcd, LCD_OFFSET_X, LCD_OFFSET_Y, false);
            return;
        }
#endif
        static uint64_t last_rgb = 0;
        if (rgb_matrix_config.raw != last_rgb) {
            last_rgb = rgb_matrix_config.raw;
            draw_rgb_text(false);
        }
        static uint8_t last_bl = 0;
        if (get_backlight_level() != last_bl) {
            last_bl = get_backlight_level();
            draw_bl_text(false);
        }
    }

    qp_surface_draw(surface, lcd, 52, 40, false);
}
