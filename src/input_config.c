#include "input_config.h"
#include "vflash.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BINDINGS 4
#define MAX_PADS 4
/* Left stick past this (of 32767) counts as a direction */
#define STICK_DEADZONE 16000

typedef struct {
    const char *name;     /* the key in flashem.ini */
    uint32_t button;      /* VFLASH_BTN_*, or 0 for a hotkey */
    uint32_t hotkey;      /* HOTKEY_*, or 0 */
    const char *keys;     /* default keyboard bindings (SDL key names, comma-separated) */
    const char *pad;      /* default gamepad bindings (SDL GameController names) */
} Action;

/* An action's current bindings */
typedef struct {
    SDL_Scancode key[MAX_BINDINGS];
    int nkeys;
    int padbutton[MAX_BINDINGS]; /* SDL_GameControllerButton, or 100 + axis for a trigger */
    int npad;
} Binding;

/* The keyboard defaults are the ones the frontend always had. The gamepad
 * defaults follow the libretro core's RetroPad layout, by position: east red,
 * south yellow, north green, west blue, Start OK, Back Exit. */
static const Action actions[] = {
    { "up",        VFLASH_BTN_UP,     0, "Up",        "dpup" },
    { "down",      VFLASH_BTN_DOWN,   0, "Down",      "dpdown" },
    { "left",      VFLASH_BTN_LEFT,   0, "Left",      "dpleft" },
    { "right",     VFLASH_BTN_RIGHT,  0, "Right",     "dpright" },
    { "red",       VFLASH_BTN_RED,    0, "Z",         "b" },
    { "yellow",    VFLASH_BTN_YELLOW, 0, "X",         "a" },
    { "green",     VFLASH_BTN_GREEN,  0, "C",         "y" },
    { "blue",      VFLASH_BTN_BLUE,   0, "V",         "x" },
    { "ok",        VFLASH_BTN_ENTER,  0, "Return",    "start" },
    { "exit",      VFLASH_BTN_EXIT,   0, "Backspace", "back" },
    { "l",         VFLASH_BTN_L,      0, "Q",         "leftshoulder" },
    { "r",         VFLASH_BTN_R,      0, "W",         "rightshoulder" },
    { "stick",     VFLASH_BTN_STICK,  0, "Space",     "leftstick" },
    { "question",  VFLASH_BTN_QUESTION, 0, "H",       "lefttrigger" },
    { "book",      VFLASH_BTN_BOOK,   0, "B",         "righttrigger" },
    { "a",         VFLASH_BTN_A,      0, "1",         "" },
    { "b",         VFLASH_BTN_B,      0, "2",         "" },
    { "c",         VFLASH_BTN_C,      0, "3",         "" },
    { "d",         VFLASH_BTN_D,      0, "4",         "" },
    { "console_play",      VFLASH_BTN_CON_PLAY,      0, "5",    "" },
    { "console_stop",      VFLASH_BTN_CON_STOP,      0, "6",    "" },
    { "console_forward",   VFLASH_BTN_CON_FORWARD,   0, "7",    "" },
    { "console_volume_down", VFLASH_BTN_CON_VOL_DOWN, 0, "8",   "" },
    { "console_volume_up", VFLASH_BTN_CON_VOL_UP,    0, "9",    "" },
    { "console_power_on",  VFLASH_BTN_CON_POWER_ON,  0, "Home", "" },
    { "console_power_off", VFLASH_BTN_CON_POWER_OFF, 0, "End",  "" },
    { "turbo",      0, HOTKEY_TURBO,      "Tab",    "rightstick" },
    { "fullscreen", 0, HOTKEY_FULLSCREEN, "F11",    "" },
    { "screenshot", 0, HOTKEY_SCREENSHOT, "F5",     "" },
    { "open_disc",  0, HOTKEY_OPEN_DISC,  "O",      "" },
    { "quit",       0, HOTKEY_QUIT,       "Escape", "" },
};
#define NACTIONS (sizeof(actions) / sizeof(actions[0]))

static Binding bindings[NACTIONS];

static SDL_GameController *pads[MAX_PADS];

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

/* "Z, Return" into the action's keyboard or gamepad bindings */
static void bind(size_t action, const char *list, int gamepad) {
    const Action *a = &actions[action];
    Binding *bd = &bindings[action];
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", list);
    if (gamepad) bd->npad = 0; else bd->nkeys = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        const char *name = trim(tok);
        if (!*name) continue;
        if (gamepad) {
            if (bd->npad >= MAX_BINDINGS) break;
            SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name);
            if (b != SDL_CONTROLLER_BUTTON_INVALID) {
                bd->padbutton[bd->npad++] = b;
                continue;
            }
            SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(name);
            if (ax == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ax == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                bd->padbutton[bd->npad++] = 100 + ax;
                continue;
            }
            fprintf(stderr, "[Input] flashem.ini: unknown gamepad button \"%s\" for %s\n", name, a->name);
        } else {
            if (bd->nkeys >= MAX_BINDINGS) break;
            SDL_Scancode sc = SDL_GetScancodeFromName(name);
            if (sc != SDL_SCANCODE_UNKNOWN)
                bd->key[bd->nkeys++] = sc;
            else
                fprintf(stderr, "[Input] flashem.ini: unknown key \"%s\" for %s\n", name, a->name);
        }
    }
}

static void write_defaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "; FlashEm standalone controls. Each line is an action and its bindings,\n"
        "; up to %d, comma-separated; an empty value unbinds it.\n"
        "; Keys are SDL key names (Z, Return, Left, Tab, F11, Keypad 8 ...).\n"
        "; Gamepad buttons are SDL GameController names: a b x y back start\n"
        "; leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft\n"
        "; dpright lefttrigger righttrigger. The left stick always moves the\n"
        "; V.Flash stick too. turbo runs the emulator unthrottled while held.\n"
        "; Delete this file to get the defaults back.\n\n[keyboard]\n", MAX_BINDINGS);
    for (size_t i = 0; i < NACTIONS; i++)
        fprintf(f, "%s = %s\n", actions[i].name, actions[i].keys);
    fprintf(f, "\n[gamepad]\n");
    for (size_t i = 0; i < NACTIONS; i++)
        fprintf(f, "%s = %s\n", actions[i].name, actions[i].pad);
    fprintf(f, "\n; The console's language: bios (the boot ROM's own), en-us, en-uk, fr, es,\n"
               "; de or nl. It takes effect at the next start.\n[console]\nlanguage = bios\n");
    fclose(f);
}

static char language[16];

const char *input_config_language(void) {
    return language[0] ? language : NULL;
}

void input_config_load(const char *dir) {
    for (size_t i = 0; i < NACTIONS; i++) {
        bind(i, actions[i].keys, 0);
        bind(i, actions[i].pad, 1);
    }

    char path[1100];
    snprintf(path, sizeof(path), "%s%sflashem.ini", dir ? dir : "",
             dir && *dir && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\' ? "/" : "");
    FILE *f = fopen(path, "r");
    if (!f) {
        write_defaults(path);
        return;
    }
    char line[512];
    int gamepad = 0, console = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            gamepad = strncmp(s, "[gamepad]", 9) == 0;
            console = strncmp(s, "[console]", 9) == 0;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *name = trim(s);
        const char *value = trim(eq + 1);
        if (console) {
            if (!strcmp(name, "language"))
                snprintf(language, sizeof(language), "%s", value);
            else
                fprintf(stderr, "[Input] flashem.ini: unknown console setting \"%s\"\n", name);
            continue;
        }
        size_t i = 0;
        for (; i < NACTIONS && strcmp(actions[i].name, name) != 0; i++)
            ;
        if (i == NACTIONS) {
            fprintf(stderr, "[Input] flashem.ini: unknown action \"%s\"\n", name);
            continue;
        }
        bind(i, value, gamepad);
    }
    fclose(f);
    printf("[Input] Controls from %s\n", path);
}

void input_handle_event(const SDL_Event *ev) {
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pads[i]) {
                pads[i] = SDL_GameControllerOpen(ev->cdevice.which);
                if (pads[i])
                    printf("[Input] Gamepad: %s\n", SDL_GameControllerName(pads[i]));
                break;
            }
        }
    } else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int i = 0; i < MAX_PADS; i++) {
            if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == ev->cdevice.which) {
                SDL_GameControllerClose(pads[i]);
                pads[i] = NULL;
            }
        }
    }
}

static int pad_held(SDL_GameController *pad, int binding) {
    if (binding >= 100)
        return SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)(binding - 100)) > STICK_DEADZONE;
    return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)binding);
}

uint32_t input_poll(const uint8_t *keyboard, uint32_t *hotkeys) {
    uint32_t buttons = 0, held = 0;
    for (size_t i = 0; i < NACTIONS; i++) {
        const Action *a = &actions[i];
        const Binding *bd = &bindings[i];
        int on = 0;
        for (int k = 0; keyboard && !on && k < bd->nkeys; k++)
            on = keyboard[bd->key[k]];
        for (int p = 0; !on && p < MAX_PADS; p++)
            for (int b = 0; pads[p] && !on && b < bd->npad; b++)
                on = pad_held(pads[p], bd->padbutton[b]);
        if (on) {
            buttons |= a->button;
            held |= a->hotkey;
        }
    }
    for (int p = 0; p < MAX_PADS; p++) {
        if (!pads[p]) continue;
        int x = SDL_GameControllerGetAxis(pads[p], SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(pads[p], SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -STICK_DEADZONE) buttons |= VFLASH_BTN_LEFT;
        if (x > STICK_DEADZONE)  buttons |= VFLASH_BTN_RIGHT;
        if (y < -STICK_DEADZONE) buttons |= VFLASH_BTN_UP;
        if (y > STICK_DEADZONE)  buttons |= VFLASH_BTN_DOWN;
    }
    if (hotkeys) *hotkeys = held;
    return buttons;
}

void input_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i]) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
    }
}
