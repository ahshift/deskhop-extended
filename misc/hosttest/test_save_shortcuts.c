/* Host-side tests for how a Save from the config page reaches the shortcut table.
 *
 * saveHandler in webconfig/templates/script.js writes every setting as a SET_VAL message of
 * its own, the shortcuts in table order, and then sends SAVE_CONFIG. The other board gets the
 * same stream through the proxy. handle_api_msgs stores each value, and
 * handle_save_config_msg rebuilds hotkeys[] from the lot and writes it to flash. The table
 * used to be rebuilt after every value instead, which checked each shortcut against rows the
 * page had not written yet, still on their old combinations: a swap, or a combination moved
 * to a row above the one giving it up, was cleared on the way while the page said it was
 * saved. So the messages go through the real handlers here, in the order the page sends them.
 *
 * Built and run by run.sh. Everything below the tests stands in for what handlers.c,
 * keyboard.c and protocol.c reach and none of this exercises. */
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

/* The api_field_map key of each entry in hotkeys[], in table order (src/protocol.c). Config
   mode has none, since it cannot be set. */
static const uint8_t KEY[NUM_HOTKEYS] = {90, 91, 92, 93, 94, 95, 96, 97, 99, 0, 101, 102};

/* What save_config last wrote, and how many times it ran. */
static uint32_t flashed[NUM_HOTKEYS];
static int saves = 0;

static void set_val(uint8_t key, uint32_t value) {
    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {[0] = key}};

    memcpy(&packet.data[1], &value, sizeof(value));
    handle_api_msgs(&packet, &global_state);
}

static void save(void) {
    uart_packet_t packet = {.type = SAVE_CONFIG_MSG};

    handle_save_config_msg(&packet, &global_state);
}

/* What the page sends on Save: every shortcut, changed or not, then the store message. */
static void page_save(const uint32_t *page) {
    for (int n = 0; n < NUM_HOTKEYS; n++)
        if (KEY[n])
            set_val(KEY[n], page[n]);

    save();
}

/* A board with nothing stored, so its table is on the combinations it was built with. */
static void fresh_board(void) {
    memset(global_state.config.hotkey_cfg, 0, sizeof(global_state.config.hotkey_cfg));
    global_state.config.hotkey_toggle = HOTKEY_TOGGLE;
    hotkeys_apply_config(&global_state);

    memset(flashed, 0, sizeof(flashed));
    saves = 0;
}

/* Which entry answers a report holding exactly this combination, as an index, or -1. */
static int answers(uint32_t packed) {
    hid_keyboard_report_t report = {.modifier = HOTKEY_MOD(packed),
                                    .keycode  = {HOTKEY_KEY1(packed), HOTKEY_KEY2(packed)}};
    hotkey_combo_t *hit = check_all_hotkeys(&report, &global_state);

    return hit ? (int)(hit - hotkeys) : -1;
}

int main(void) {
    char detail[96];
    uint32_t page[NUM_HOTKEYS];

    /* Entries 2 to 5 as they are built. */
    const uint8_t  both = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT;
    const uint32_t switch_lock = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_K, 0);
    const uint32_t screen_lock = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_L, 0);
    const uint32_t gaming      = HOTKEY_PACK(both, HID_KEY_G, 0);
    const uint32_t pong        = HOTKEY_PACK(both, HID_KEY_S, 0);

    /* First call, and it has to be the one that captures the compiled-in table, so nothing
       may be stored before it. */
    fresh_board();

    printf("\n  what a Save keeps\n\n");

    /* Gaming mode and pong trade combinations. Whichever of the two is written first
       collides with the other's old one, which is all the board holds for it until the
       other is written. */
    memset(page, 0, sizeof(page));
    page[4] = pong;
    page[5] = gaming;
    page_save(page);
    snprintf(detail, sizeof(detail), "flash %08x %08x", flashed[4], flashed[5]);
    check("a swap reaches flash whole", flashed[4] == pong && flashed[5] == gaming, detail);
    snprintf(detail, sizeof(detail), "entries %d %d", answers(pong), answers(gaming));
    check("and each row answers the other's old combination",
          answers(pong) == 4 && answers(gaming) == 5, detail);

    /* Lock switching, entry 2, takes Right Ctrl + L from lock both screens, entry 3, which is
       turned off. Entry 2 is written first, while entry 3 still holds that combination. */
    fresh_board();
    memset(page, 0, sizeof(page));
    page[2] = screen_lock;
    page[3] = HOTKEY_OFF;
    page_save(page);
    snprintf(detail, sizeof(detail), "flash %08x %08x", flashed[2], flashed[3]);
    check("a combination moved to an earlier row is kept",
          flashed[2] == screen_lock && flashed[3] == HOTKEY_OFF, detail);
    snprintf(detail, sizeof(detail), "entry %d", answers(screen_lock));
    check("and answers on that row", answers(screen_lock) == 2, detail);

    /* The other way round always worked, since the row giving it up is written first. */
    fresh_board();
    memset(page, 0, sizeof(page));
    page[2] = HOTKEY_OFF;
    page[3] = switch_lock;
    page_save(page);
    snprintf(detail, sizeof(detail), "flash %08x %08x", flashed[2], flashed[3]);
    check("so is one moved to a later row",
          flashed[2] == HOTKEY_OFF && flashed[3] == switch_lock, detail);

    printf("\n  when it takes effect\n\n");

    /* The page sends nothing before Save, and on the board a shortcut that arrives on its
       own waits for Save too: the table changes once the set is complete. */
    fresh_board();
    const uint32_t mine = HOTKEY_PACK(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_J, 0);

    set_val(KEY[2], mine);
    snprintf(detail, sizeof(detail), "old answers %d, new answers %d", answers(switch_lock),
             answers(mine));
    check("a shortcut sent without Save is not in use yet",
          answers(switch_lock) == 2 && answers(mine) == -1, detail);

    save();
    snprintf(detail, sizeof(detail), "old answers %d, new answers %d", answers(switch_lock),
             answers(mine));
    check("Save puts it into use", answers(mine) == 2 && answers(switch_lock) == -1, detail);
    snprintf(detail, sizeof(detail), "flash %08x, %d saves", flashed[2], saves);
    check("and stores it", flashed[2] == mine && saves == 1, detail);

    printf("\n  what a Save refuses\n\n");

    /* Checked as a set, a clash is still a clash. Lock both screens asks for lock switching's
       combination while lock switching keeps it. */
    fresh_board();
    memset(page, 0, sizeof(page));
    page[3] = switch_lock;
    page_save(page);
    snprintf(detail, sizeof(detail), "flash %08x", flashed[3]);
    check("a combination another row keeps does not reach flash", flashed[3] == 0, detail);
    snprintf(detail, sizeof(detail), "entries %d %d", answers(switch_lock), answers(screen_lock));
    check("and both rows answer what they were built with",
          answers(switch_lock) == 2 && answers(screen_lock) == 3, detail);

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}

/* ---- what handlers.c, keyboard.c and protocol.c link against and none of the above reaches */

device_t global_state;
watchdog_hw_t watchdog_hw_stand_in;
const uint8_t ADDR_FW_RUNNING[4];

void save_config(device_t *state) {
    memcpy(flashed, state->config.hotkey_cfg, sizeof(flashed));
    saves++;
}

void blink_led(device_t *state) { (void)state; }
void restore_leds(device_t *state) { (void)state; }
uint8_t toggle_led(void) { return 0; }
void send_value(const uint8_t value, enum packet_type_e type) { (void)value; (void)type; }
void queue_packet(const uint8_t *data, enum packet_type_e type, int length) {
    (void)data; (void)type; (void)length;
}
void wipe_config(void) {}
void load_config(device_t *state) { (void)state; }
void reboot(void) {}
void reset_config_timer(device_t *state) { (void)state; }
void queue_mouse_report(mouse_report_t *report, device_t *state) { (void)report; (void)state; }
void set_remote_mouse_buttons(device_t *state, uint8_t buttons) { (void)state; (void)buttons; }
void reset_usb_boot(uint32_t gpio_activity_pin_mask, uint32_t disable_interface_mask) {
    (void)gpio_activity_pin_mask; (void)disable_interface_mask;
}
int32_t extract_kbd_data(uint8_t *raw, int len, uint8_t itf, hid_interface_t *iface,
                         hid_keyboard_report_t *out) {
    (void)raw; (void)len; (void)itf; (void)iface; (void)out; return 0;
}
bool queue_try_add(queue_t *q, const void *v) { (void)q; (void)v; return true; }
bool queue_try_peek(queue_t *q, void *v) { (void)q; (void)v; return false; }
bool queue_try_remove(queue_t *q, void *v) { (void)q; (void)v; return false; }
uint64_t time_us_64(void) { return 0; }
void write_raw_packet(uint8_t *dst, uart_packet_t *packet) { (void)dst; (void)packet; }
bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return false; }
bool tud_hid_n_ready(uint8_t instance) { (void)instance; return false; }
uint8_t tud_hid_n_get_protocol(uint8_t instance) { (void)instance; return 1; }
bool tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len) {
    (void)instance; (void)report_id; (void)report; (void)len; return true;
}
bool tud_hid_keyboard_report(uint8_t report_id, uint8_t modifier, const uint8_t *keycode) {
    (void)report_id; (void)modifier; (void)keycode; return true;
}
