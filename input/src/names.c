/* Key and button name tables, mac keycode <-> USB HID usage. */
#include "internal.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const k_buttons[NAPPLE_BTN_COUNT_BASIC] = {"A",  "B",  "X",    "Y",     "StickL", "StickR", "L",     "R",
                                                              "ZL", "ZR", "Plus", "Minus", "Left",   "Up",     "Right", "Down"};
static const char *const k_elements[NAPPLE_PAD_COUNT] = {"south", "east", "west", "north",   "l3",    "r3",  "lb",     "rb",
                                                         "lt",    "rt",   "menu", "options", "dleft", "dup", "dright", "ddown"};

const char *napple_button_name(int b) { return b >= 0 && b < NAPPLE_BTN_COUNT_BASIC ? k_buttons[b] : "?"; }
const char *napple_pad_element_name(int e) { return e >= 0 && e < NAPPLE_PAD_COUNT ? k_elements[e] : "?"; }
int napple_button_from_name(const char *n) {
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++)
        if (!strcasecmp(n, k_buttons[i])) return i;
    return -1;
}
int napple_pad_element_from_name(const char *n) {
    for (int i = 0; i < NAPPLE_PAD_COUNT; i++)
        if (!strcasecmp(n, k_elements[i])) return i;
    return -1;
}

typedef struct {
    uint16_t mac;
    uint8_t usb;
    const char *name;
} KeyInfo;
static const KeyInfo k_keys[] = {
    {0, 4, "A"},
    {11, 5, "B"},
    {8, 6, "C"},
    {2, 7, "D"},
    {14, 8, "E"},
    {3, 9, "F"},
    {5, 10, "G"},
    {4, 11, "H"},
    {34, 12, "I"},
    {38, 13, "J"},
    {40, 14, "K"},
    {37, 15, "L"},
    {46, 16, "M"},
    {45, 17, "N"},
    {31, 18, "O"},
    {35, 19, "P"},
    {12, 20, "Q"},
    {15, 21, "R"},
    {1, 22, "S"},
    {17, 23, "T"},
    {32, 24, "U"},
    {9, 25, "V"},
    {13, 26, "W"},
    {7, 27, "X"},
    {16, 28, "Y"},
    {6, 29, "Z"},
    {18, 30, "1"},
    {19, 31, "2"},
    {20, 32, "3"},
    {21, 33, "4"},
    {23, 34, "5"},
    {22, 35, "6"},
    {26, 36, "7"},
    {28, 37, "8"},
    {25, 38, "9"},
    {29, 39, "0"},
    {36, 40, "Return"},
    {53, 41, "Esc"},
    {51, 42, "Delete"},
    {48, 43, "Tab"},
    {49, 44, "Space"},
    {27, 45, "-"},
    {24, 46, "="},
    {33, 47, "["},
    {30, 48, "]"},
    {42, 49, "\\"},
    {41, 51, ";"},
    {39, 52, "'"},
    {50, 53, "`"},
    {43, 54, ","},
    {47, 55, "."},
    {44, 56, "/"},
    {57, 57, "CapsLock"},
    {122, 58, "F1"},
    {120, 59, "F2"},
    {99, 60, "F3"},
    {118, 61, "F4"},
    {96, 62, "F5"},
    {97, 63, "F6"},
    {98, 64, "F7"},
    {100, 65, "F8"},
    {101, 66, "F9"},
    {109, 67, "F10"},
    {103, 68, "F11"},
    {111, 69, "F12"},
    {115, 74, "Home"},
    {116, 75, "PageUp"},
    {117, 76, "FwdDelete"},
    {119, 77, "End"},
    {121, 78, "PageDown"},
    {124, 79, "Right"},
    {123, 80, "Left"},
    {125, 81, "Down"},
    {126, 82, "Up"},
    {75, 84, "Keypad /"},
    {67, 85, "Keypad *"},
    {78, 86, "Keypad -"},
    {69, 87, "Keypad +"},
    {76, 88, "Keypad Enter"},
    {83, 89, "Keypad 1"},
    {84, 90, "Keypad 2"},
    {85, 91, "Keypad 3"},
    {86, 92, "Keypad 4"},
    {87, 93, "Keypad 5"},
    {88, 94, "Keypad 6"},
    {89, 95, "Keypad 7"},
    {91, 96, "Keypad 8"},
    {92, 97, "Keypad 9"},
    {82, 98, "Keypad 0"},
    {65, 99, "Keypad ."},
    /* modifiers: usage ids E0..E7 */
    {59, 0xE0, "Control"},
    {56, 0xE1, "Shift"},
    {58, 0xE2, "Option"},
    {55, 0xE3, "Command"},
    {62, 0xE4, "Right Control"},
    {60, 0xE5, "Right Shift"},
    {61, 0xE6, "Right Option"},
    {54, 0xE7, "Right Command"},
};
#define NKEYS (sizeof k_keys / sizeof *k_keys)

uint8_t napple_mac_to_hid_usage(uint16_t mac) {
    for (size_t i = 0; i < NKEYS; i++)
        if (k_keys[i].mac == mac) return k_keys[i].usb;
    return 0;
}

/* HidKeyboardModifier: Control 0, Shift 1, LeftAlt 2, RightAlt 3, Gui 4, CapsLock 8 */
uint64_t napple_mac_modifier_bit(uint16_t mac) {
    switch (mac) {
        case 59:
        case 62: return 1u << 0;
        case 56:
        case 60: return 1u << 1;
        case 58: return 1u << 2;
        case 61: return 1u << 3;
        case 55:
        case 54: return 1u << 4;
        case 57: return 1u << 8;
        default: return 0;
    }
}

const char *napple_keycode_name(uint16_t mac) {
    if (mac == NAPPLE_KEY_NONE) return "-";
    for (size_t i = 0; i < NKEYS; i++)
        if (k_keys[i].mac == mac) return k_keys[i].name;
    static _Thread_local char buf[16];
    snprintf(buf, sizeof buf, "Key %u", mac);
    return buf;
}
