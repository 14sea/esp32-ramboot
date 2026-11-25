#include "kvm_keys.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "class/hid/hid.h"

static const uint8_t s_ascii[128][2] = { HID_ASCII_TO_KEYCODE };   /* {需要 Shift, 鍵碼} */

typedef struct {
    const char *name;
    uint8_t code;
} name_code_t;

static const name_code_t s_mods[] = {
    {"ctrl", KEYBOARD_MODIFIER_LEFTCTRL},   {"control", KEYBOARD_MODIFIER_LEFTCTRL},
    {"lctrl", KEYBOARD_MODIFIER_LEFTCTRL},  {"rctrl", KEYBOARD_MODIFIER_RIGHTCTRL},
    {"shift", KEYBOARD_MODIFIER_LEFTSHIFT}, {"lshift", KEYBOARD_MODIFIER_LEFTSHIFT},
    {"rshift", KEYBOARD_MODIFIER_RIGHTSHIFT},
    {"alt", KEYBOARD_MODIFIER_LEFTALT},     {"lalt", KEYBOARD_MODIFIER_LEFTALT},
    {"ralt", KEYBOARD_MODIFIER_RIGHTALT},   {"altgr", KEYBOARD_MODIFIER_RIGHTALT},
    {"gui", KEYBOARD_MODIFIER_LEFTGUI},     {"win", KEYBOARD_MODIFIER_LEFTGUI},
    {"super", KEYBOARD_MODIFIER_LEFTGUI},   {"meta", KEYBOARD_MODIFIER_LEFTGUI},
    {"lgui", KEYBOARD_MODIFIER_LEFTGUI},    {"rgui", KEYBOARD_MODIFIER_RIGHTGUI},
};

static const name_code_t s_keys[] = {
    {"enter", HID_KEY_ENTER},         {"return", HID_KEY_ENTER},
    {"esc", HID_KEY_ESCAPE},          {"escape", HID_KEY_ESCAPE},
    {"backspace", HID_KEY_BACKSPACE}, {"bs", HID_KEY_BACKSPACE},
    {"tab", HID_KEY_TAB},             {"space", HID_KEY_SPACE},
    {"minus", HID_KEY_MINUS},         {"equal", HID_KEY_EQUAL},
    {"lbracket", HID_KEY_BRACKET_LEFT}, {"rbracket", HID_KEY_BRACKET_RIGHT},
    {"backslash", HID_KEY_BACKSLASH}, {"semicolon", HID_KEY_SEMICOLON},
    {"quote", HID_KEY_APOSTROPHE},    {"apostrophe", HID_KEY_APOSTROPHE},
    {"grave", HID_KEY_GRAVE},         {"backtick", HID_KEY_GRAVE},
    {"comma", HID_KEY_COMMA},         {"period", HID_KEY_PERIOD},
    {"dot", HID_KEY_PERIOD},          {"slash", HID_KEY_SLASH},
    {"capslock", HID_KEY_CAPS_LOCK},  {"printscreen", HID_KEY_PRINT_SCREEN},
    {"prtsc", HID_KEY_PRINT_SCREEN},  {"sysrq", HID_KEY_PRINT_SCREEN},
    {"scrolllock", HID_KEY_SCROLL_LOCK}, {"pause", HID_KEY_PAUSE},
    {"break", HID_KEY_PAUSE},         {"insert", HID_KEY_INSERT},
    {"ins", HID_KEY_INSERT},          {"home", HID_KEY_HOME},
    {"pgup", HID_KEY_PAGE_UP},        {"pageup", HID_KEY_PAGE_UP},
    {"delete", HID_KEY_DELETE},       {"del", HID_KEY_DELETE},
    {"end", HID_KEY_END},             {"pgdn", HID_KEY_PAGE_DOWN},
    {"pagedown", HID_KEY_PAGE_DOWN},  {"right", HID_KEY_ARROW_RIGHT},
    {"left", HID_KEY_ARROW_LEFT},     {"down", HID_KEY_ARROW_DOWN},
    {"up", HID_KEY_ARROW_UP},         {"numlock", HID_KEY_NUM_LOCK},
    {"menu", HID_KEY_APPLICATION},    {"app", HID_KEY_APPLICATION},
    {"kpenter", HID_KEY_KEYPAD_ENTER}, {"kpplus", HID_KEY_KEYPAD_ADD},
    {"kpminus", HID_KEY_KEYPAD_SUBTRACT}, {"kpmul", HID_KEY_KEYPAD_MULTIPLY},
    {"kpdiv", HID_KEY_KEYPAD_DIVIDE}, {"kpdot", HID_KEY_KEYPAD_DECIMAL},
};

static bool lookup(const name_code_t *t, size_t n, const char *s, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(t[i].name, s) == 0) { *out = t[i].code; return true; }
    }
    return false;
}

bool kvm_ascii_chord(char c, kvm_chord_t *out)
{
    unsigned char u = (unsigned char)c;
    if (u >= 128 || s_ascii[u][1] == 0) return false;
    out->mod = s_ascii[u][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    out->key = s_ascii[u][1];
    return true;
}

/* 單個鍵名（不含 '+'） */
static bool parse_key(const char *s, kvm_chord_t *c)
{
    uint8_t code;
    if (lookup(s_mods, sizeof(s_mods) / sizeof(s_mods[0]), s, &code)) { c->mod |= code; return true; }
    if (lookup(s_keys, sizeof(s_keys) / sizeof(s_keys[0]), s, &code)) { c->key = code; return true; }
    size_t n = strlen(s);
    if ((s[0] == 'f' || s[0] == 'F') && n >= 2 && n <= 3 && isdigit((unsigned char)s[1])) {
        int f = atoi(s + 1);
        if (f >= 1 && f <= 12) { c->key = HID_KEY_F1 + (f - 1); return true; }
        if (f >= 13 && f <= 24) { c->key = HID_KEY_F13 + (f - 13); return true; }
        return false;
    }
    if (strncasecmp(s, "kp", 2) == 0 && n == 3 && isdigit((unsigned char)s[2])) {
        c->key = (s[2] == '0') ? HID_KEY_KEYPAD_0 : HID_KEY_KEYPAD_1 + (s[2] - '1');
        return true;
    }
    if (n == 1) {                       /* 單個字符：按美式佈局換算 */
        kvm_chord_t a;
        if (!kvm_ascii_chord(isupper((unsigned char)s[0]) ? (char)tolower((unsigned char)s[0]) : s[0], &a)) return false;
        c->mod |= a.mod;
        c->key = a.key;
        return true;
    }
    return false;
}

bool kvm_parse_chord(const char *s, kvm_chord_t *out)
{
    char buf[48];
    size_t n = strlen(s);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, s, n + 1);
    kvm_chord_t c = {0};
    /* "+" 本身也是一個鍵（shift+=）：按 '+' 分割，但連續的 "++" 或結尾的 "+" 當鍵名 */
    char *p = buf;
    while (*p) {
        char *q = p + 1;                /* 鍵名至少 1 個字符，所以從 p+1 開始找分隔符 */
        while (*q && *q != '+') q++;
        char save = *q;
        *q = '\0';
        if (c.key != 0) return false;   /* 普通鍵必須是最後一個 */
        if (!parse_key(p, &c)) return false;
        if (save == '\0') break;
        p = q + 1;
        if (*p == '\0') return false;   /* 結尾多了一個 '+' */
    }
    *out = c;
    return true;
}
