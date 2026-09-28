/* Host-side tests for uniform speed in src/mouse.c.
 *
 * speed_x and speed_y move the pointer a share of the screen per count, so the same
 * setting moves it more pixels on a bigger screen, and on a Windows extra screen, where
 * Windows moves the cursor and the board only follows it, the board's idea of where it is
 * drifts from where it really is. The first a user sees of that is the wrap to the other
 * computer coming before the cursor has reached the outer edge. With uniform_speed set, a
 * count moves the pointer pointer_speed percent of a pixel on every screen, from each
 * screen's resolution, and a Windows extra screen is sent those pixels, so the board's
 * position and Windows' agree.
 *
 * Driven in boot protocol as test_mouse.c is. Defaults as src/defaults.c has them where a
 * case does not say otherwise: B on the left, A on the right, speed 16 by 28, and no
 * acceleration.
 *
 * Built and run by run.sh. Everything below the tests is a stand-in for the parts of the
 * firmware mouse.c links against but none of this exercises.
 */
#include <stdio.h>
#include <string.h>

#include "main.h"

static int failures = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
           ok ? "" : "  <- ", ok ? "" : (detail ? detail : ""));
    if (!ok)
        failures++;
}

/* ==================================================== *
 * What left the board
 * ==================================================== */

static mouse_report_t queued[256];     /* Reports handed to the local mouse queue, in order */
static int            nqueued;
static mouse_report_t forwarded[256];  /* Reports sent to the other board over UART, in order */
static int            nforwarded;

static hid_interface_t *mouse;

static void forget_output(void) {
    nqueued = nforwarded = 0;
}

/* One screen on each output, both running Linux, uniform speed as given at 100 percent,
   every screen 2560 by 1440, and the pointer on the left edge of output A. This board is A
   and has the mouse. */
static void reset_state(uint8_t uniform, uint32_t screens_a, uint32_t screens_b) {
    memset(&global_state, 0, sizeof(global_state));

    global_state.tud_connected = true;
    global_state.active_output = OUTPUT_A;
    global_state.board_role    = OUTPUT_A;
    global_state.pointer_x     = MIN_SCREEN_COORD;
    global_state.pointer_y     = MIN_SCREEN_COORD;

    for (int out = 0; out < NUM_SCREENS; out++) {
        output_t *output      = &global_state.config.output[out];
        output->number        = out;
        output->screen_count  = (out == OUTPUT_A) ? screens_a : screens_b;
        output->screen_index  = 1;
        output->speed_x       = 16;
        output->speed_y       = 28;
        output->border.bottom = MAX_SCREEN_COORD;
        output->os            = LINUX;

        for (int s = 0; s < MAX_SCREENS_PER_OUTPUT; s++)
            global_state.config.screen_size[out][s] = (screen_size_t){2560, 1440};
    }

    global_state.config.output[OUTPUT_A].pos = RIGHT;
    global_state.config.output[OUTPUT_B].pos = LEFT;
    global_state.config.uniform_speed        = uniform;
    global_state.config.pointer_speed        = 100;

    mouse = &global_state.iface[0][0];
    memset(mouse, 0, sizeof(*mouse));
    mouse->protocol = HID_PROTOCOL_BOOT;

    forget_output();
}

static void move_xy(int8_t dx, int8_t dy) {
    uint8_t report[3] = {0, (uint8_t)dx, (uint8_t)dy};
    process_mouse_report(report, sizeof(report), 1, mouse);
}

/* Move by a total of count, in reports of at most 100. */
static void move_by(int count, int vertical) {
    while (count) {
        int step = count > 100 ? 100 : (count < -100 ? -100 : count);

        move_xy(vertical ? 0 : (int8_t)step, vertical ? (int8_t)step : 0);
        count -= step;
    }
}

#define X      (global_state.pointer_x)
#define Y      (global_state.pointer_y)
#define ACTIVE (global_state.active_output)

/* Near enough: within a coordinate or two of rounding. */
static int near(int value, int want) {
    return value >= want - 2 && value <= want + 2;
}

/* Put the pointer on a Windows extra screen of output B, the way arriving there leaves it:
   B active, relative mode on. */
static void on_windows_extra_screen(uint32_t screen, int16_t x) {
    global_state.config.output[OUTPUT_B].os           = WINDOWS;
    global_state.config.output[OUTPUT_B].screen_index = screen;
    global_state.active_output                        = OUTPUT_B;
    global_state.relative_mouse                       = true;
    global_state.pointer_x                            = x;
}

/* Where Windows has its cursor on one screen of width pixels, a pixel per count at its
   default speed with Enhance pointer precision off, stopped at the screen's edges. Only the
   relative reports count; the park report on the way out is absolute. */
static int windows_x;

static void windows_follows(const mouse_report_t *log, int from, int to, int width) {
    for (int i = from; i < to; i++) {
        if (log[i].mode != RELATIVE)
            continue;

        windows_x += log[i].x;

        if (windows_x < 0)
            windows_x = 0;
        else if (windows_x > width - 1)
            windows_x = width - 1;
    }
}

int main(void) {
    char detail[96];

    printf("\n  off, the default\n\n");

    reset_state(0, 1, 1);
    move_xy(10, 10);
    snprintf(detail, sizeof(detail), "x %d y %d", X, Y);
    check("Speed X and Y still decide, a share of the screen per count", X == 160 && Y == 280, detail);

    reset_state(0, 1, 2);
    on_windows_extra_screen(2, MAX_SCREEN_COORD / 2);
    move_xy(-10, 0);
    snprintf(detail, sizeof(detail), "sent %d", nforwarded ? forwarded[0].x : 0);
    check("and a Windows extra screen is sent the move as it came",
          nforwarded == 1 && forwarded[0].x == -10, detail);

    printf("\n  on, a count is a pixel on every screen\n\n");

    reset_state(1, 1, 1);
    move_by(2559, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("2559 counts cross a 2560 screen from edge to edge", near(X, MAX_SCREEN_COORD), detail);
    move_by(1439, 1);
    snprintf(detail, sizeof(detail), "y %d", Y);
    check("and 1439 its height, the same speed both ways", near(Y, MAX_SCREEN_COORD), detail);

    /* 1280 of the 2559 pixels from edge to edge. */
    const int half = (int)(1280.0 * MAX_SCREEN_COORD / 2559 + 0.5);

    reset_state(1, 1, 1);
    move_by(1280, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("halfway after half as many", near(X, half), detail);

    reset_state(1, 1, 1);
    global_state.config.screen_size[OUTPUT_A][0] = (screen_size_t){1920, 1080};
    move_by(1919, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("a 1920 screen takes 1919, so a count goes as far on either", near(X, MAX_SCREEN_COORD),
          detail);

    reset_state(1, 1, 1);
    global_state.config.pointer_speed = 200;
    move_by(1280, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("200 percent takes half the counts", near(X, MAX_SCREEN_COORD), detail);

    reset_state(1, 1, 1);
    global_state.mouse_zoom = true;
    move_by(4 * 1280, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("slow mouse takes four times as many", near(X, half), detail);

    reset_state(1, 1, 1);
    global_state.config.pointer_speed = 25;
    for (int i = 0; i < 4; i++)
        move_xy(1, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("a quarter of a pixel per count adds up instead of being lost", near(X, 13), detail);

    printf("\n  on, a Windows extra screen\n\n");

    reset_state(1, 1, 2);
    global_state.config.pointer_speed = 150;
    on_windows_extra_screen(2, MAX_SCREEN_COORD / 2);
    move_xy(-10, 0);
    snprintf(detail, sizeof(detail), "sent %d", nforwarded ? forwarded[0].x : 0);
    check("it is sent the pixels, 15 for 10 counts at 150 percent",
          nforwarded == 1 && forwarded[0].x == -15, detail);

    forget_output();
    for (int i = 0; i < 4; i++)
        move_xy(-1, 0);
    {
        int sum = 0;

        for (int i = 0; i < nforwarded; i++)
            sum += forwarded[i].x;

        snprintf(detail, sizeof(detail), "sent %d %d %d %d", forwarded[0].x, forwarded[1].x,
                 forwarded[2].x, forwarded[3].x);
        check("and one count at a time the half pixels add up, 6 for 4", sum == -6, detail);
    }

    /* The case that was reported: moving out across the far screen, the wrap to the other
       computer came while Windows' cursor was still short of the edge. Now the report that
       takes Windows' cursor to the edge is the one that wraps. */
    for (int uniform = 1; uniform >= 0; uniform--) {
        int wrapped_at = -1, last = 0;

        reset_state((uint8_t)uniform, 1, 2);
        global_state.config.wrap_around = 1;
        on_windows_extra_screen(2, MAX_SCREEN_COORD);
        windows_x = 2560 - 1;

        for (int i = 0; i < 200 && ACTIVE == OUTPUT_B; i++) {
            forget_output();
            move_xy(-50, 0);
            last = windows_x;
            windows_follows(forwarded, 0, nforwarded, 2560);
            wrapped_at = (ACTIVE == OUTPUT_A) ? windows_x : -1;
        }

        snprintf(detail, sizeof(detail), "Windows at %d when it wrapped, %d before", wrapped_at, last);

        if (uniform)
            check("moving out, it wraps on the report that takes Windows' cursor to the edge",
                  wrapped_at == 0 && last > 0, detail);
        else
            check("where with it off it wrapped with the cursor 20 percent short of it",
                  wrapped_at > 400, detail);
    }

    /* Back the other way, the board crosses onto the main screen when Windows' cursor does. */
    {
        int crossed_at = -1;

        reset_state(1, 1, 2);
        on_windows_extra_screen(2, MIN_SCREEN_COORD);
        windows_x = 0;

        for (int i = 0; i < 100 && global_state.config.output[OUTPUT_B].screen_index == 2; i++) {
            int before = windows_x;

            forget_output();
            move_xy(50, 0);
            windows_follows(forwarded, 0, nforwarded, 1000000);

            if (global_state.config.output[OUTPUT_B].screen_index == 1)
                crossed_at = before;
        }

        snprintf(detail, sizeof(detail), "Windows at %d on the report before it crossed", crossed_at);
        check("moving back, it crosses to the main screen as Windows' cursor leaves the far one",
              crossed_at >= 2560 - 1 - 50 && crossed_at <= 2560 - 1, detail);
    }

    reset_state(1, 1, 2);
    on_windows_extra_screen(2, MAX_SCREEN_COORD / 2);
    global_state.pointer_y = MIN_SCREEN_COORD;
    move_by(1439, 1);
    snprintf(detail, sizeof(detail), "y %d", Y);
    check("up and down is followed the same way, 1439 counts for its height",
          near(Y, MAX_SCREEN_COORD), detail);

    reset_state(1, 1, 2);
    global_state.config.pointer_speed = 150;
    on_windows_extra_screen(2, MAX_SCREEN_COORD / 2);
    global_state.gaming_mode = true;
    move_xy(-10, 0);
    snprintf(detail, sizeof(detail), "sent %d", nforwarded ? forwarded[0].x : 0);
    check("gaming mode is still sent the raw move", nforwarded == 1 && forwarded[0].x == -10, detail);

    printf("\n  what it will not take from config\n\n");

    reset_state(1, 1, 1);
    global_state.config.screen_size[OUTPUT_A][0] = (screen_size_t){0, 0};
    move_by(1919, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("a screen of no size is read as the default 1920 by 1080", near(X, MAX_SCREEN_COORD),
          detail);

    reset_state(1, 1, 1);
    global_state.config.pointer_speed = 0;
    move_by(2559, 0);
    snprintf(detail, sizeof(detail), "x %d", X);
    check("and a speed of nothing as 100 percent, rather than a pointer that will not move",
          near(X, MAX_SCREEN_COORD), detail);

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}

/* ==================================================== *
 * Stand-ins for everything mouse.c reaches for
 * ==================================================== */

device_t global_state = {0};

bool queue_try_add(queue_t *queue, const void *value) {
    if (queue == &global_state.mouse_queue && nqueued < 256)
        memcpy(&queued[nqueued++], value, sizeof(mouse_report_t));

    return true;
}

bool queue_try_peek(queue_t *queue, void *value) { (void)queue; (void)value; return false; }
bool queue_try_remove(queue_t *queue, void *value) { (void)queue; (void)value; return false; }

void queue_packet(const uint8_t *data, enum packet_type_e packet_type, int length) {
    if (packet_type == MOUSE_REPORT_MSG && nforwarded < 256)
        memcpy(&forwarded[nforwarded++], data,
               length < (int)sizeof(mouse_report_t) ? (size_t)length : sizeof(mouse_report_t));
}

void send_value(const uint8_t value, enum packet_type_e packet_type) { (void)value; (void)packet_type; }

uint64_t time_us_64(void) { return 1000000; }

int32_t get_report_value(uint8_t *report, int len, report_val_t *val) {
    (void)report; (void)len; (void)val;
    return 0;
}

void set_active_output(device_t *state, uint8_t new_output) {
    state->active_output = new_output;
}

bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return false; }
bool tud_hid_n_ready(uint8_t instance) { (void)instance; return false; }
uint8_t tud_hid_n_get_protocol(uint8_t instance) { (void)instance; return 1; }
bool tud_mouse_report(uint8_t mode, uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan) {
    (void)mode; (void)buttons; (void)x; (void)y; (void)wheel; (void)pan; return true;
}
