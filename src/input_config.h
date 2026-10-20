#pragma once
/* The standalone frontend's controls: keyboard and gamepad bindings for the
 * V.Flash's buttons and the frontend's hotkeys, from flashem.ini beside the
 * program (written with the defaults when it is missing). */
#include <SDL2/SDL.h>
#include <stdint.h>

/* Frontend hotkeys, as bits of input_poll's hotkey state */
enum {
    HOTKEY_TURBO      = 1 << 0,  /* held: run unthrottled */
    HOTKEY_FULLSCREEN = 1 << 1,
    HOTKEY_SCREENSHOT = 1 << 2,
    HOTKEY_OPEN_DISC  = 1 << 3,
    HOTKEY_QUIT       = 1 << 4,
};

/* Reads flashem.ini from dir (SDL_GetBasePath()), or writes it with the
 * default bindings when it does not exist. dir may be NULL. */
void input_config_load(const char *dir);

/* The console's language from flashem.ini's [console] section (see
 * vflash_set_language), or NULL when it names none. */
const char *input_config_language(void);

/* Gamepads coming and going (SDL_CONTROLLERDEVICEADDED / REMOVED). */
void input_handle_event(const SDL_Event *ev);

/* The V.Flash buttons held now (VFLASH_BTN_*), and the hotkeys held now.
 * keyboard is SDL_GetKeyboardState, or NULL to read gamepads only. */
uint32_t input_poll(const uint8_t *keyboard, uint32_t *hotkeys);

void input_shutdown(void);
