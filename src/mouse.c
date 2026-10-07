/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#include "main.h"
#include <math.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5
#define WRAP_STEP_ONTO 16
#define WRAP_STEP_ACROSS 1000
#define WRAP_STEPS_PER_SCREEN 16
#define ACCEL_POINTS 7

/* Safety margin when landing on the other PC (~12% inside screen) to prevent ping-pong bounce */
#define ENTRY_MARGIN 4000

uint16_t get_jump_threshold(output_t *output, enum screen_pos_e direction) {
    const uint16_t NO_JUMP_THRESHOLD = 0;

    if (global_state.config.wrap_around && output->pos == direction &&
        output->screen_index >= output->screen_count)
        return global_state.config.jump_threshold;

    if (output->screen_index > 1)
        return NO_JUMP_THRESHOLD;

    if (output->pos == direction && output->screen_index == 1)
        return NO_JUMP_THRESHOLD;

    return global_state.config.jump_threshold;
}

/* Check if our upcoming mouse movement would result in having to switch outputs */
enum screen_pos_e is_screen_switch_needed(output_t *output, int position, int offset) {
    enum screen_pos_e direction = (offset < 0) ? LEFT : RIGHT;

    if (offset == 0)
        return NONE;

    uint16_t threshold = get_jump_threshold(output, direction);

    /* Enforce an 800-count push resistance at the border so hopping requires a deliberate push */
    if (threshold < 800)
        threshold = 800;

    if (position + offset < MIN_SCREEN_COORD - threshold)
        return LEFT;

    if (position + offset > MAX_SCREEN_COORD + threshold)
        return RIGHT;

    return NONE;
}

/* Move mouse coordinate 'position' by 'offset', but don't fall off the screen */
int32_t move_and_keep_on_screen(int position, int offset) {
    if (position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;
    else if (position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;

    return position + offset;
}

/* Implement basic mouse acceleration based on actual 2D movement magnitude */
float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
        {2, 1},
        {5, 1.1},
        {15, 1.4},
        {30, 1.9},
        {45, 2.6},
        {60, 3.4},
        {70, 4.0},
    };

    if (offset_x == 0 && offset_y == 0)
        return 1.0;

    if (!global_state.config.enable_acceleration)
        return 1.0;

    const float movement_magnitude = sqrtf((float)(offset_x * offset_x) + (float)(offset_y * offset_y));

    if (movement_magnitude <= acceleration[0].value)
        return acceleration[0].factor;

    if (movement_magnitude >= acceleration[ACCEL_POINTS-1].value)
        return acceleration[ACCEL_POINTS-1].factor;

    const struct curve *lower = NULL;
    const struct curve *upper = NULL;

    for (int i = 0; i < ACCEL_POINTS-1; i++) {
        if (movement_magnitude < acceleration[i + 1].value) {
            lower = &acceleration[i];
            upper = &acceleration[i + 1];
            break;
        }
    }

    if (lower == NULL || upper == NULL)
        return 1.0;

    const float interpolation_pos = (movement_magnitude - lower->value) /
                                  (upper->value - lower->value);

    return lower->factor + interpolation_pos * (upper->factor - lower->factor);
}

static float uniform_pixels_per_count(device_t *state, uint8_t reduce_speed) {
    uint16_t percent = state->config.pointer_speed;

    if (percent < POINTER_SPEED_MIN || percent > POINTER_SPEED_MAX)
        percent = POINTER_SPEED;

    return percent / 100.0f / (1 << reduce_speed);
}

static screen_size_t current_screen_size(device_t *state) {
    uint32_t      index = state->config.output[state->active_output].screen_index;
    screen_size_t size  = {SCREEN_WIDTH, SCREEN_HEIGHT};

    if (index >= 1 && index <= MAX_SCREENS_PER_OUTPUT) {
        screen_size_t stored = state->config.screen_size[state->active_output][index - 1];

        if (stored.width >= SCREEN_SIZE_MIN)
            size.width = stored.width;

        if (stored.height >= SCREEN_SIZE_MIN)
            size.height = stored.height;
    }

    return size;
}

static int uniform_offset(device_t *state, int axis, int32_t *move, float pixels_per_count, uint16_t size) {
    float px = *move * pixels_per_count;

    if (state->relative_mouse && !state->gaming_mode) {
        float want = px + state->uniform_rest_px[axis];
        float sent = roundf(want);

        if (sent > INT16_MAX)
            sent = INT16_MAX;
        else if (sent < -INT16_MAX)
            sent = -INT16_MAX;

        state->uniform_rest_px[axis] = (fabsf(want - sent) < 1.0f) ? want - sent : 0.0f;
        *move = (int32_t)sent;
        px    = sent;
    }

    float units  = px * MAX_SCREEN_COORD / (size - 1) + state->uniform_rest_units[axis];
    int   offset = (int)roundf(units);

    state->uniform_rest_units[axis] = units - offset;
    return offset;
}

/* Returns LEFT if need to jump left, RIGHT if right, NONE otherwise */
enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current    = &state->config.output[state->active_output];
    uint8_t reduce_speed = 0;
    int offset_x, offset_y;

    if (state->mouse_zoom)
        reduce_speed = MOUSE_ZOOM_SCALING_FACTOR;

    float acceleration_factor = calculate_mouse_acceleration_factor(values->move_x, values->move_y);

    if (state->config.uniform_speed) {
        float         pixels = uniform_pixels_per_count(state, reduce_speed) * acceleration_factor;
        screen_size_t screen = current_screen_size(state);

        offset_x = uniform_offset(state, 0, &values->move_x, pixels, screen.width);
        offset_y = uniform_offset(state, 1, &values->move_y, pixels, screen.height);
    } else {
        /* Dampen integration by /2.5f so cursor coordinate reaches border across full desk width */
        offset_x = round((values->move_x * acceleration_factor * (current->speed_x >> reduce_speed)) / 2.5f);
        offset_y = round((values->move_y * acceleration_factor * (current->speed_y >> reduce_speed)) / 2.5f);
    }

    enum screen_pos_e switch_direction = is_screen_switch_needed(current, state->pointer_x, offset_x);

    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    return switch_direction;
}

void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)report, MOUSE_REPORT_MSG, MOUSE_REPORT_LENGTH);
    }
}

int16_t scale_y_coordinate(int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to   = &state->config.output[screen_to];

    int size_to   = to->border.bottom - to->border.top;
    int size_from = from->border.bottom - from->border.top;

    if (size_from == size_to)
        return state->pointer_y;

    if (size_from > size_to) {
        return to->border.top + ((size_to * state->pointer_y) / MAX_SCREEN_COORD);
    }

    if (state->pointer_y < from->border.top)
        return MIN_SCREEN_COORD;

    if (state->pointer_y > from->border.bottom)
        return MAX_SCREEN_COORD;

    return ((state->pointer_y - from->border.top) * MAX_SCREEN_COORD) / size_from;
}

static void reset_edge_tap(device_t *state) {
    state->edge_in_contact         = false;
    state->last_edge_tap_time      = 0;
    state->last_edge_tap_direction = NONE;
}

void switch_to_another_pc(
    device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;

    int16_t mouse_y = (*mouse_park_pos == 0) ? MIN_SCREEN_COORD :
                      (*mouse_park_pos == 1) ? MAX_SCREEN_COORD :
                                               state->pointer_y;

    mouse_report_t hidden_pointer = {.y = mouse_y, .x = MAX_SCREEN_COORD};

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);

    /* Land safely inside the new screen instead of directly on the border edge */
    state->pointer_x = (direction == LEFT) ? (MAX_SCREEN_COORD - ENTRY_MARGIN)
                                           : (MIN_SCREEN_COORD + ENTRY_MARGIN);
    state->pointer_y = scale_y_coordinate(output->number, 1 - output->number, state);

    reset_edge_tap(state);
    sync_pointer_position(state);
}

void sync_pointer_position(device_t *state) {
    uart_packet_t packet = {
        .type = POINTER_SYNC_MSG,
        .data16 = {
            [0] = (uint16_t)state->pointer_x,
            [1] = (uint16_t)state->pointer_y,
        },
    };

    queue_try_add(&state->uart_tx_queue, &packet);
}

void switch_virtual_desktop_macos(device_t *state, int direction) {
    mouse_report_t edge_position = {
        .x = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD,
        .y = MAX_SCREEN_COORD / 2,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    uint16_t move = (direction == LEFT) ? -MACOS_SWITCH_MOVE_X : MACOS_SWITCH_MOVE_X;
    mouse_report_t move_relative_one = {
        .x = move,
        .mode = RELATIVE,
        .buttons = 0,
    };

    output_mouse_report(&edge_position, state);

    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;
        case WINDOWS:
            state->relative_mouse = (new_index > 1);
            break;
        case LINUX:
        case ANDROID:
        case OTHER:
            break;
    }

    state->pointer_x     = (direction == RIGHT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    output->screen_index = new_index;
    reset_edge_tap(state);
}

void push_to_far_side(device_t *state, output_t *output, int direction) {
    int16_t sign = (direction == LEFT) ? -1 : 1;

    mouse_report_t edge   = {.x = state->pointer_x, .y = state->pointer_y, .mode = ABSOLUTE};
    mouse_report_t onto   = {.x = sign * WRAP_STEP_ONTO, .mode = RELATIVE};
    mouse_report_t across = {.x = sign * WRAP_STEP_ACROSS, .mode = RELATIVE};

    output_mouse_report(&edge, state);

    for (uint32_t i = 0; i < WRAP_STEPS_PER_SCREEN * (output->screen_count - 1); i++) {
        output_mouse_report(&onto, state);
        output_mouse_report(&across, state);
    }
}

void wrap_to_another_pc(device_t *state, output_t *output, int direction) {
    output_t *other = &state->config.output[1 - state->active_output];
    int back        = (direction == LEFT) ? RIGHT : LEFT;

    if (output->os == MACOS)
        for (uint32_t i = output->screen_index; i > 1; i--)
            switch_virtual_desktop_macos(state, back);

    output->screen_index = 1;
    switch_to_another_pc(state, output, 1 - state->active_output, direction);

    if (other->os == MACOS)
        for (uint32_t i = other->screen_index; i < other->screen_count; i++)
            switch_virtual_desktop_macos(state, back);
    else if (other->os == WINDOWS && other->screen_count > 1)
        push_to_far_side(state, other, back);

    other->screen_index   = other->screen_count;
    state->relative_mouse = (other->os == WINDOWS && other->screen_count > 1);
}

static bool edge_double_tap_ready(device_t *state, int direction) {
    if (!state->config.switch_double_tap_enable)
        return true;

    if (state->edge_in_contact)
        return false;

    state->edge_in_contact = true;

    uint64_t now       = time_us_64();
    uint64_t window_us = (uint64_t)state->config.switch_double_tap_ms * 1000;

    if (state->last_edge_tap_direction == direction && (now - state->last_edge_tap_time) <= window_us) {
        state->last_edge_tap_time      = 0;
        state->last_edge_tap_direction = NONE;
        return true;
    }

    state->last_edge_tap_time      = now;
    state->last_edge_tap_direction = direction;
    return false;
}

void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];

    if (state->switch_lock)
        return;

    /* 400ms debounce cooldown to prevent rapid ping-ponging across screens */
    static uint64_t last_switch_time = 0;
    uint64_t now = time_us_64();
    if (now - last_switch_time < 400000)
        return;

    /* Jump in direction of other computer */
    if (output->pos != direction) {
        if (output->screen_index == 1) {
            /* Block screen switching while holding any mouse button */
            if (state->mouse_buttons)
                return;

            if (!edge_double_tap_ready(state, direction))
                return;

            switch_to_another_pc(state, output, 1 - state->active_output, direction);
            last_switch_time = now;
        } else {
            switch_virtual_desktop(state, output, output->screen_index - 1, direction);
            last_switch_time = now;
        }
    } else if (output->screen_index < output->screen_count) {
        switch_virtual_desktop(state, output, output->screen_index + 1, direction);
        last_switch_time = now;
    } else if (state->config.wrap_around && !state->mouse_buttons) {
        if (!edge_double_tap_ready(state, direction))
            return;

        wrap_to_another_pc(state, output, direction);
        last_switch_time = now;
    }
}

static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    if (len <= uses_id)
        return false;

    if (uses_id) {
        if (*raw_report++ != src->report_id)
            return false;
        len--;
    }

    *dst = get_report_value(raw_report, len, src);
    return true;
}

void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t *mouse_report = (hid_mouse_report_t *)raw_report;
        values->buttons = (len >= 1) ? mouse_report->buttons : iface->mouse_buttons;
        values->move_x  = (len >= 2) ? mouse_report->x : 0;
        values->move_y  = (len >= 3) ? mouse_report->y : 0;
        values->wheel   = (len >= MOUSE_BOOT_REPORT_LEN) ? mouse_report->wheel : 0;
        values->pan     = (len > MOUSE_BOOT_REPORT_LEN) ? mouse_report->pan : 0;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    if (!extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = iface->mouse_buttons;
    }
}

mouse_report_t create_mouse_report(device_t *state, mouse_values_t *values) {
    mouse_report_t mouse_report = {
        .buttons = values->buttons,
        .x       = state->pointer_x,
        .y       = state->pointer_y,
        .wheel   = values->wheel,
        .pan     = values->pan,
        .mode    = ABSOLUTE,
    };

    if (state->relative_mouse || state->gaming_mode) {
        mouse_report.x = values->move_x;
        mouse_report.y = values->move_y;
        mouse_report.mode = RELATIVE;
    }

    return mouse_report;
}

static uint8_t combine_local_mouse_buttons(device_t *state) {
    uint8_t buttons = 0;
    for (int dev = 0; dev < MAX_DEVICES; dev++)
        for (int idx = 0; idx < MAX_INTERFACES; idx++)
            buttons |= state->iface[dev][idx].mouse_buttons;
    return buttons;
}

uint8_t refresh_local_mouse_buttons(device_t *state) {
    state->local_mouse_buttons = combine_local_mouse_buttons(state);
    state->mouse_buttons       = state->local_mouse_buttons | state->remote_mouse_buttons;
    return state->local_mouse_buttons;
}

void set_remote_mouse_buttons(device_t *state, uint8_t buttons) {
    state->remote_mouse_buttons = buttons;
    state->mouse_buttons        = state->local_mouse_buttons | state->remote_mouse_buttons;
}

void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    static bool init_done = false;
    if (!init_done) {
        state->gaming_mode = true;
        init_done = true;
    }

    extract_report_values(raw_report, len, state, &values, iface);
    uint8_t buttons = (uint8_t)values.buttons;

    if (values.move_x == 0 && values.move_y == 0 &&
        values.wheel == 0 && values.pan == 0 &&
        buttons == iface->mouse_buttons) {
        return;
    }

    uint8_t previous_local = state->local_mouse_buttons;
    iface->mouse_buttons = buttons;
    refresh_local_mouse_buttons(state);
    values.buttons       = state->mouse_buttons;

    if (state->local_mouse_buttons != previous_local)
        send_value(state->local_mouse_buttons, MOUSE_BUTTONS_MSG);

    enum screen_pos_e switch_direction = update_mouse_position(state, &values);

    mouse_report_t report = create_mouse_report(state, &values);
    output_mouse_report(&report, state);

    int16_t margin = state->config.switch_double_tap_margin;
    if (state->pointer_x > MIN_SCREEN_COORD + margin &&
        state->pointer_x < MAX_SCREEN_COORD - margin)
        state->edge_in_contact = false;

    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        sync_pointer_position(state);

    if (switch_direction != NONE)
        do_screen_switch(state, switch_direction);
}

/* ==================================================== *
 * Mouse Queue Section
 * ==================================================== */

void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    if (!state->tud_connected)
        return;

    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    if (tud_suspended())
        tud_remote_wakeup();

    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    if (report.mode == ABSOLUTE && tud_hid_n_get_protocol(ITF_NUM_HID) == HID_PROTOCOL_BOOT) {
        queue_try_remove(&state->mouse_queue, &report);
        return;
    }

    bool succeeded = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);

    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

void queue_mouse_report(mouse_report_t *report, device_t *state) {
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}
