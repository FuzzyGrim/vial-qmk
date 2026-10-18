// Copyright 2025 Dasky (@daskygit)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quantum.h"
#include "common/display_lcd.h"

void keyboard_post_init_kb(void) {
    display_init_kb();
    keyboard_post_init_user();
}

void post_process_record_kb(uint16_t keycode, keyrecord_t *record) {
    // Runs on the master half only, after the key was handled (tap/hold is decided by then).
    if (record->event.pressed && IS_KEYEVENT(record->event)) {
        display_note_keypress(keycode, record->tap.count, record->event.key.row >= MATRIX_ROWS / 2);
    }
    post_process_record_user(keycode, record);
}

void housekeeping_task_kb(void) {
    display_sync_task();
    housekeeping_task_user();
}
