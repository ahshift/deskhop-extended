/* Host-side tests for wrap-around in src/mouse.c.
 *
 * Only the border between the two computers used to switch outputs; the outer edge of the
 * outermost screen, the one facing away from the other computer, stopped the pointer. With
 * wrap_around set, the outer edge switches outputs too and the pointer comes in at the far
 * edge of the other computer's outermost screen. These are the rules that jump follows:
 * where it lands, what holds it back like a border switch, and how the cursors of an output
 * with several screens are moved across them, a Mac one screen at a time and Windows by a
 * walk of relative steps, checked against a model of how Windows moves its cursor for screens
 * of any width.
 *
 * Driven in boot protocol, as test_mouse.c is, so a report is just buttons and a move.
 * Defaults as src/defaults.c has them: B on the left, A on the right, speed 16 and no
 * acceleration, so a move of 10 is 160 in screen coordinates.
 *
 * Built and run by run.sh. Everything below the tests is a stand-in for the parts of the
 * firmware mouse.c links against but none of this exercises.
 */
#include <stdio.h>
#include <string.h>

#include "main.h"

static int failures = 0;

static void check(const char *name, int ok) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", name);
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
static uint64_t       now;             /* What time_us_64 answers */

static hid_interface_t *mouse;

static void forget_output(void) {
    nqueued = nforwarded = 0;
}

/* One screen on each output unless a case says otherwise, both running Linux, wrap_around as
   given, and the pointer in the middle of output A. This board is A and has the mouse. */
static void reset_state(uint8_t wrap_around, uint32_t screens_a, uint32_t screens_b) {
    memset(&global_state, 0, sizeof(global_state));

    global_state.tud_connected = true;
    global_state.active_output = OUTPUT_A;
    global_state.board_role    = OUTPUT_A;
    global_state.pointer_x     = MAX_SCREEN_COORD / 2;
    global_state.pointer_y     = MAX_SCREEN_COORD / 2;

    for (int out = 0; out < NUM_SCREENS; out++) {
        output_t *output      = &global_state.config.output[out];
        output->number        = out;
        output->screen_count  = (out == OUTPUT_A) ? screens_a : screens_b;
        output->screen_index  = 1;
        output->speed_x       = 16;
        output->speed_y       = 16;
        output->border.bottom = MAX_SCREEN_COORD;
        output->os            = LINUX;
    }

    global_state.config.output[OUTPUT_A].pos = RIGHT;
    global_state.config.output[OUTPUT_B].pos = LEFT;
    global_state.config.wrap_around          = wrap_around;

    mouse = &global_state.iface[0][0];
    memset(mouse, 0, sizeof(*mouse));
    mouse->protocol = HID_PROTOCOL_BOOT;

    forget_output();
    now = 1000000;
}

static void move(int8_t dx, uint8_t buttons) {
    uint8_t report[3] = {buttons, (uint8_t)dx, 0};
    process_mouse_report(report, sizeof(report), 1, mouse);
}

/* Put the pointer on an output at a given x, as if it had got there by itself. */
static void place(uint8_t output, int16_t x) {
    global_state.active_output = output;
    global_state.pointer_x     = x;
}

/* The same, on one of that output's screens, with relative mode as moving there leaves it. */
static void place_on(uint8_t output, uint32_t screen, int16_t x) {
    output_t *out = &global_state.config.output[output];

    place(output, x);
    out->screen_index           = screen;
    global_state.relative_mouse = (out->os == WINDOWS && screen > 1);
}

#define ACTIVE      (global_state.active_output)
#define X           (global_state.pointer_x)
#define SCREEN(out) (global_state.config.output[out].screen_index)
#define RELATIVE_ON (global_state.relative_mouse)

/* One step between two Mac screens, as switch_virtual_desktop_macos sends it: the edge in the
   middle of the screen it is on, then five nudges of 10 across. */
static int is_mac_step(const mouse_report_t *log, int at, int direction) {
    int16_t edge  = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    int16_t nudge = (direction == LEFT) ? -10 : 10;

    if (log[at].mode != ABSOLUTE || log[at].x != edge || log[at].y != MAX_SCREEN_COORD / 2)
        return 0;

    for (int i = 1; i <= 5; i++)
        if (log[at + i].mode != RELATIVE || log[at + i].x != nudge)
            return 0;

    return 1;
}

/* Where Windows leaves the cursor after the reports a wrap sent it, as far as can be told from
   how it behaves. Absolute coordinates land on the main screen. A relative move goes where it
   points if that is on a screen, and otherwise stops at the edge of the screen the cursor is on,
   the same stop that holds a cursor where two screens of different heights do not meet.

   Measured outward from the border: the main screen covers the first widths[0] pixels, the next
   screen out the widths[1] after those, and so on, all at the height the walk runs along. Outward
   is the way the walk heads, left for an output on the left. speed is Windows' pointer speed with
   acceleration off, 1.0 being its default. True if the cursor ends on the last screen's outer
   edge. */
static int lands_on_far_edge(const mouse_report_t *log, int n, const int *widths, int screens,
                             double speed, int outward) {
    double total = 0, at = 0;

    for (int s = 0; s < screens; s++)
        total += widths[s];

    for (int i = 0; i < n; i++) {
        if (log[i].mode == ABSOLUTE) {
            double x = (double)log[i].x / MAX_SCREEN_COORD;

            at = ((outward == LEFT) ? 1.0 - x : x) * (widths[0] - 1);
            continue;
        }

        double to = at + ((outward == LEFT) ? -log[i].x : log[i].x) * speed;

        if (to >= 0 && to <= total - 1) {
            at = to;
            continue;
        }

        /* Off every screen, so it stops at an edge of the screen it is on. */
        int    on   = 0;
        double from = 0;

        while (on < screens - 1 && at >= from + widths[on])
            from += widths[on++];

        at = (to < from) ? from : from + widths[on] - 1;
    }

    return at >= total - 1.5;
}

/* Keep moving one way and write down every output and screen the pointer passes through, as
   "A2B2B1..." - enough of them to go around more than once. */
static void trail(int8_t dx, char *seen, int want) {
    int     n          = 0;
    uint8_t last       = ACTIVE;
    uint32_t last_screen = SCREEN(ACTIVE);

    for (int i = 0; i < 400 && n < want * 2; i++) {
        move(dx, 0);

        if (ACTIVE != last || SCREEN(ACTIVE) != last_screen) {
            last        = ACTIVE;
            last_screen = SCREEN(ACTIVE);
            seen[n++]   = (ACTIVE == OUTPUT_A) ? 'A' : 'B';
            seen[n++]   = (char)('0' + last_screen);
        }
    }
    seen[n] = 0;
}

int main(void) {
    char seen[64];

    printf("\n  off, the default\n\n");

    reset_state(0, 1, 1);
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("A's outer edge stops the pointer", ACTIVE == OUTPUT_A && X == MAX_SCREEN_COORD);

    reset_state(0, 1, 1);
    place(OUTPUT_B, MIN_SCREEN_COORD);
    move(-10, 0);
    check("and so does B's", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);

    reset_state(0, 2, 1);
    global_state.config.output[OUTPUT_A].os = MACOS;
    place_on(OUTPUT_A, 2, MAX_SCREEN_COORD);
    move(10, 0);
    check("so does the outer edge of A's last screen",
          ACTIVE == OUTPUT_A && SCREEN(OUTPUT_A) == 2 && X == MAX_SCREEN_COORD);

    reset_state(0, 1, 1);
    place(OUTPUT_A, MIN_SCREEN_COORD);
    move(-10, 0);
    check("the border switches as it always did", ACTIVE == OUTPUT_B && X == MAX_SCREEN_COORD);

    printf("\n  on, one screen each\n\n");

    reset_state(1, 1, 1);
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("past A's outer edge comes in at B's outer edge", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);

    reset_state(1, 1, 1);
    place(OUTPUT_B, MIN_SCREEN_COORD);
    move(-10, 0);
    check("past B's outer edge comes in at A's", ACTIVE == OUTPUT_A && X == MAX_SCREEN_COORD);

    reset_state(1, 1, 1);
    place(OUTPUT_A, MIN_SCREEN_COORD);
    move(-10, 0);
    check("the border still switches", ACTIVE == OUTPUT_B && X == MAX_SCREEN_COORD);

    /* Keep going right and the outputs take turns, each time coming in at the left edge. */
    reset_state(1, 1, 1);
    place(OUTPUT_A, MIN_SCREEN_COORD);
    trail(127, seen, 4);
    check("moving right goes around, B and A in turn", !strcmp(seen, "B1A1B1A1"));
    check("and comes in at the left edge", X < MAX_SCREEN_COORD / 2);

    reset_state(1, 1, 1);
    place(OUTPUT_B, MAX_SCREEN_COORD);
    trail(-127, seen, 4);
    check("moving left goes around too", !strcmp(seen, "A1B1A1B1"));

    /* The jump is switch_to_another_pc, so the output left behind gets the parking report
       a border switch gives it: right edge, at the park position, bottom here. */
    reset_state(1, 1, 1);
    global_state.config.output[OUTPUT_A].mouse_park_pos = 1;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("A is parked on the way out, as at the border",
          ACTIVE == OUTPUT_B && nqueued >= 2 && queued[nqueued - 1].x == MAX_SCREEN_COORD
              && queued[nqueued - 1].y == MAX_SCREEN_COORD);

    /* And the height goes through the same border calibration. */
    reset_state(1, 1, 1);
    global_state.config.output[OUTPUT_B].border.top    = 8192;
    global_state.config.output[OUTPUT_B].border.bottom = 24576;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    global_state.pointer_y = MIN_SCREEN_COORD;
    move(10, 0);
    check("the height is scaled as at the border", ACTIVE == OUTPUT_B && global_state.pointer_y == 8192);

    printf("\n  a Mac with more than one screen\n\n");

    /* Leaving from A2: the Mac cursor steps back onto A1 and is parked there, which is where
       every other way out of A leaves it. */
    reset_state(1, 2, 1);
    global_state.config.output[OUTPUT_A].os = MACOS;
    place_on(OUTPUT_A, 2, MAX_SCREEN_COORD);
    forget_output();
    move(10, 0);
    check("leaving from A2 comes in at B's outer edge", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);
    check("after one step back to A1 and the parking report",
          nqueued == 8 && is_mac_step(queued, 1, LEFT) && queued[7].mode == ABSOLUTE
              && queued[7].x == MAX_SCREEN_COORD);
    check("so A is on its main screen again", SCREEN(OUTPUT_A) == 1);

    /* Arriving on a Mac with three screens: two steps out, onto B3. */
    reset_state(1, 1, 3);
    global_state.config.output[OUTPUT_B].os = MACOS;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    forget_output();
    move(10, 0);
    check("arriving on a three-screen Mac lands on its last screen",
          ACTIVE == OUTPUT_B && SCREEN(OUTPUT_B) == 3 && X == MIN_SCREEN_COORD);
    check("two steps out, and nothing relative left on",
          nforwarded == 12 && is_mac_step(forwarded, 0, LEFT) && is_mac_step(forwarded, 6, LEFT)
              && !RELATIVE_ON);
    move(10, 0);
    check("the next report is absolute, just inside B3's outer edge",
          nforwarded == 13 && forwarded[12].mode == ABSOLUTE && forwarded[12].x == MIN_SCREEN_COORD + 160);

    /* A Mac left on an extra screen some other way, the switch shortcut say, only has the
       rest of the way to go. */
    reset_state(1, 1, 3);
    global_state.config.output[OUTPUT_B].os           = MACOS;
    global_state.config.output[OUTPUT_B].screen_index = 2;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    forget_output();
    move(10, 0);
    check("a Mac already on B2 takes one step to B3",
          SCREEN(OUTPUT_B) == 3 && nforwarded == 6 && is_mac_step(forwarded, 0, LEFT));

    printf("\n  Windows with more than one screen\n\n");

    /* Windows puts absolute coordinates on its main screen only, so the cursor is placed on
       that screen's edge at its height and walked the rest of the way, in pairs of a short step
       and a long one. This first desk is the one it was found on: one screen on the right and two
       Windows screens on the left. */
    reset_state(1, 1, 2);
    global_state.config.output[OUTPUT_B].os = WINDOWS;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    global_state.pointer_y = 1000;
    forget_output();
    move(10, 0);
    check("arriving on two-screen Windows lands on its far screen, in relative mode",
          ACTIVE == OUTPUT_B && SCREEN(OUTPUT_B) == 2 && RELATIVE_ON);
    {
        int ok = nforwarded == 1 + 2 * 16 && forwarded[0].mode == ABSOLUTE
                 && forwarded[0].x == MIN_SCREEN_COORD && forwarded[0].y == 1000;

        for (int i = 1; i < 1 + 2 * 16; i++)
            ok &= forwarded[i].mode == RELATIVE && forwarded[i].y == 0
                  && forwarded[i].x == ((i % 2) ? -16 : -1000);

        check("placed on the main screen's edge at its height, then 16 pairs of 16 and 1000 left", ok);
    }

    reset_state(1, 1, 3);
    global_state.config.output[OUTPUT_B].os = WINDOWS;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    global_state.pointer_y = 1000;
    forget_output();
    move(10, 0);
    check("arriving on three-screen Windows lands on its last screen, in relative mode",
          ACTIVE == OUTPUT_B && SCREEN(OUTPUT_B) == 3 && RELATIVE_ON);
    check("with twice the pairs for two screens to cross", nforwarded == 1 + 2 * 32);
    move(10, 0);
    check("and the next report is relative",
          nforwarded == 2 + 2 * 32 && forwarded[1 + 2 * 32].mode == RELATIVE);

    /* From there back to the border as the virtual desktop code already does it: relative
       until the main screen, absolute on it, and over the border to A. */
    {
        int relative_on_b2 = 0, absolute_on_b1 = 0;

        for (int i = 0; i < 100 && ACTIVE == OUTPUT_B; i++) {
            move(127, 0);
            if (ACTIVE == OUTPUT_B && SCREEN(OUTPUT_B) == 2 && RELATIVE_ON)
                relative_on_b2 = 1;
            if (ACTIVE == OUTPUT_B && SCREEN(OUTPUT_B) == 1 && !RELATIVE_ON)
                absolute_on_b1 = 1;
        }
        check("moving right crosses B2 relative and B1 absolute", relative_on_b2 && absolute_on_b1);
        check("then the border takes it to A", ACTIVE == OUTPUT_A && X == MIN_SCREEN_COORD && !RELATIVE_ON);
    }

    printf("\n  Windows, whatever its screens measure\n\n");

    /* The walk replayed against how Windows moves a cursor, for screens of any width and pointer
       speeds either side of the default. No single move may jump a whole screen, since one that
       would end past the far side stops where it started: that is what the old walk did. */
    {
        const struct {
            int screens, widths[3];
            double speed;
            const char *name;
        } desks[] = {
            {2, {2560, 2560}, 1.0, "two 2560 screens, the desk it was found on"},
            {2, {1920, 7680}, 1.0, "a 7680 screen out past a 1920 one"},
            {2, {1920, 7680}, 0.5, "the same at half the default speed"},
            {2, {3840, 800}, 3.5, "an 800 screen out past a 3840 one at the fastest speed"},
            {2, {2560, 3840}, 0.25, "a 3840 screen at a quarter of the default speed"},
            {3, {2560, 1280, 3840}, 1.0, "three screens of three widths"},
        };

        for (size_t d = 0; d < sizeof(desks) / sizeof(desks[0]); d++) {
            reset_state(1, 1, desks[d].screens);
            global_state.config.output[OUTPUT_B].os = WINDOWS;
            place(OUTPUT_A, MAX_SCREEN_COORD);
            forget_output();
            move(10, 0);
            check(desks[d].name, lands_on_far_edge(forwarded, nforwarded, desks[d].widths,
                                                   desks[d].screens, desks[d].speed, LEFT));
        }
    }
    {
        const int widths[2] = {2560, 2560};
        mouse_report_t old[9] = {{.x = MIN_SCREEN_COORD, .mode = ABSOLUTE}};

        for (int i = 1; i < 9; i++)
            old[i] = (mouse_report_t){.x = -MAX_SCREEN_COORD, .mode = RELATIVE};

        check("where eight pushes of the full range never left the main screen",
              !lands_on_far_edge(old, 9, widths, 2, 1.0, LEFT));
    }

    /* And the other way round: going left past B's outer edge onto Windows screens on the right,
       which this board drives itself. */
    reset_state(1, 2, 1);
    global_state.config.output[OUTPUT_A].os = WINDOWS;
    place(OUTPUT_B, MIN_SCREEN_COORD);
    forget_output();
    move(-10, 0);
    {
        const int widths[2] = {2560, 5120};

        check("going left onto two Windows screens on the right lands on the far one",
              ACTIVE == OUTPUT_A && SCREEN(OUTPUT_A) == 2
                  && lands_on_far_edge(queued, nqueued, widths, 2, 1.0, RIGHT));
    }

    /* Leaving from a Windows extra screen: the parking report is absolute, so it lands on the
       main screen by itself, and relative mode does not follow the pointer to B. */
    reset_state(1, 2, 1);
    global_state.config.output[OUTPUT_A].os = WINDOWS;
    place_on(OUTPUT_A, 2, MAX_SCREEN_COORD);
    forget_output();
    move(10, 0);
    check("leaving Windows from A2 parks A with an absolute report",
          ACTIVE == OUTPUT_B && nqueued == 2 && queued[0].mode == RELATIVE
              && queued[1].mode == ABSOLUTE && queued[1].x == MAX_SCREEN_COORD);
    check("A is on its main screen again and relative mode is off", SCREEN(OUTPUT_A) == 1 && !RELATIVE_ON);

    printf("\n  going around with more than one screen\n\n");

    /* A is a Mac with two screens, B Windows with two: B2 B1 | A1 A2. */
    reset_state(1, 2, 2);
    global_state.config.output[OUTPUT_A].os = MACOS;
    global_state.config.output[OUTPUT_B].os = WINDOWS;
    place_on(OUTPUT_A, 1, MIN_SCREEN_COORD);
    trail(127, seen, 6);
    check("moving right passes A2, B2, B1, A1, A2, B2", !strcmp(seen, "A2B2B1A1A2B2"));
    check("with relative mode on for B2", RELATIVE_ON);

    reset_state(1, 2, 2);
    global_state.config.output[OUTPUT_A].os = MACOS;
    global_state.config.output[OUTPUT_B].os = WINDOWS;
    place_on(OUTPUT_B, 1, MAX_SCREEN_COORD);
    trail(-127, seen, 6);
    check("moving left passes B2, A2, A1, B1, B2, A2", !strcmp(seen, "B2A2A1B1B2A2"));
    check("with relative mode off on the Mac", !RELATIVE_ON);

    printf("\n  what holds it back, as at the border\n\n");

    reset_state(1, 1, 1);
    global_state.config.jump_threshold = 500;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("a push short of the jump threshold stays put", ACTIVE == OUTPUT_A);
    move(40, 0);
    check("a push past it goes", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);

    /* On A1 of two, going outward is a move to A2 and has no threshold; on A2 it is the wrap. */
    reset_state(1, 2, 1);
    global_state.config.output[OUTPUT_A].os = MACOS;
    global_state.config.jump_threshold      = 500;
    place_on(OUTPUT_A, 1, MAX_SCREEN_COORD);
    move(10, 0);
    check("A1 on to A2 needs no threshold", ACTIVE == OUTPUT_A && SCREEN(OUTPUT_A) == 2);
    place_on(OUTPUT_A, 2, MAX_SCREEN_COORD);
    move(10, 0);
    check("A2's outer edge wants it", ACTIVE == OUTPUT_A && SCREEN(OUTPUT_A) == 2);
    move(40, 0);
    check("and wraps past it", ACTIVE == OUTPUT_B);

    reset_state(1, 1, 1);
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 1);
    check("a held button holds it back", ACTIVE == OUTPUT_A);
    move(10, 0);
    check("and it goes once the button is let go", ACTIVE == OUTPUT_B);

    reset_state(1, 1, 1);
    global_state.switch_lock = true;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("switch lock holds it back", ACTIVE == OUTPUT_A);

    reset_state(1, 1, 1);
    global_state.gaming_mode = true;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("so does gaming mode", ACTIVE == OUTPUT_A);

    printf("\n  with the edge double tap on\n\n");

    /* The first push only arms it. Pulled back past the margin and pushed again inside the
       window, it goes. */
    reset_state(1, 1, 1);
    global_state.config.switch_double_tap_enable = 1;
    global_state.config.switch_double_tap_ms     = 300;
    global_state.config.switch_double_tap_margin = 1000;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("one push at the outer edge does not wrap", ACTIVE == OUTPUT_A);
    move(10, 0);
    check("nor does leaning on it", ACTIVE == OUTPUT_A);
    move(-127, 0);
    now += 100000;
    move(127, 0);
    move(10, 0);
    check("a second push inside the window does", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);

    reset_state(1, 1, 1);
    global_state.config.switch_double_tap_enable = 1;
    global_state.config.switch_double_tap_ms     = 300;
    global_state.config.switch_double_tap_margin = 1000;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    move(-127, 0);
    now += 400000;
    move(127, 0);
    move(10, 0);
    check("one after the window has closed only arms it again", ACTIVE == OUTPUT_A);

    /* A push at the border and one at the outer edge are two different edges, and the
       second must not complete the first. */
    reset_state(1, 1, 1);
    global_state.config.switch_double_tap_enable = 1;
    global_state.config.switch_double_tap_ms     = 300;
    global_state.config.switch_double_tap_margin = 1000;
    place(OUTPUT_A, MIN_SCREEN_COORD);
    move(-10, 0);
    global_state.pointer_x = MAX_SCREEN_COORD / 2;
    move(1, 0);
    global_state.pointer_x = MAX_SCREEN_COORD;
    now += 100000;
    move(10, 0);
    check("a push at the border and one at the outer edge are not a double tap", ACTIVE == OUTPUT_A);

    printf("\n  the mouse on the other board\n\n");

    /* This board is B, the active output is A, so everything goes over the link until the
       wrap brings the pointer home. */
    reset_state(1, 1, 1);
    global_state.board_role = OUTPUT_B;
    place(OUTPUT_A, MAX_SCREEN_COORD);
    move(10, 0);
    check("reports for A go over the link", nforwarded >= 2 && nqueued == 0);
    check("and the wrap brings the pointer to B", ACTIVE == OUTPUT_B && X == MIN_SCREEN_COORD);
    move(10, 0);
    check("where the next report is queued here", nqueued == 1 && queued[0].x == MIN_SCREEN_COORD + 160);

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}

/* ==================================================== *
 * Stand-ins for everything mouse.c reaches for
 * ==================================================== */

device_t global_state = {0};

/* The UART queue carries pointer syncs too; only the mouse queue is what the host sees. */
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

uint64_t time_us_64(void) { return now; }

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
