// Copyright 2026 Mechboards
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "qp.h"
#include "painter/generic/qp_surface.h"

extern painter_device_t      lcd;
extern painter_device_t      surface;
extern painter_font_handle_t pixellari_14;
extern painter_font_handle_t pixellari_18;
extern painter_font_handle_t pixellari_24;

void display_init_kb(void);
bool display_init_user(void);
void display_task_kb(void);
bool display_task_user(void);

// Robot UI helpers (no-ops unless ROBOT_UI_ENABLE is defined in the keyboard config).
void display_note_keypress(uint16_t keycode, uint8_t tap_count, bool right_hand); // call on the master after every key press
void display_sync_task(void);                // call from housekeeping_task_kb()
