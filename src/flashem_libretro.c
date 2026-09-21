/* -------------------------------------------------------------------------
 *  FlashEm - libretro front end
 *
 *  The emulator is already shaped for this: vflash_run_frame() advances one
 *  frame, vflash_get_framebuffer() hands back 320x240 ARGB8888, and
 *  vflash_set_input() takes a button mask. What the core adds is the frontend
 *  side of those three, plus draining the audio ring buffer that the standalone
 *  build lets SDL pull from a callback.
 *
 *  needs_fullpath: vflash_create() opens the disc itself, by path.
 * ---------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "vflash.h"
#include "audio.h"

#define FLASHEM_FPS          60.0
#define FLASHEM_SAMPLE_RATE  ((double)AUDIO_SAMPLE_RATE)

/* One frame of stereo at 44100/60, with room for the jitter a frame of
 * emulation produces - the ring buffer is drained dry every frame either way. */
#define FLASHEM_AUDIO_MAX    ((AUDIO_SAMPLE_RATE / 30) * 2)

static retro_environment_t   environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t  audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t    input_poll_cb;
static retro_input_state_t   input_state_cb;
static retro_log_printf_t    log_cb;

static VFlash *s_vf;
static int16_t s_audio[FLASHEM_AUDIO_MAX];

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   (void)fmt;
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_controller_description pad[] = {
      { "V.Flash Controller", RETRO_DEVICE_JOYPAD },
      { NULL, 0 },
   };
   static const struct retro_controller_info ports[] = {
      { pad, 1 },
      { NULL, 0 },
   };
   bool no_content = false;

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void)
{
   struct retro_log_callback log;

   log_cb = fallback_log;
   if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
}

void retro_deinit(void)
{
   if (s_vf)
   {
      vflash_destroy(s_vf);
      s_vf = NULL;
   }
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "FlashEm";
   info->library_version  = "0.1";
   info->need_fullpath    = true;
   info->valid_extensions = "cue|bin|iso";
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = VFLASH_SCREEN_W;
   info->geometry.base_height  = VFLASH_SCREEN_H;
   info->geometry.max_width    = VFLASH_SCREEN_W;
   info->geometry.max_height   = VFLASH_SCREEN_H;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = FLASHEM_FPS;
   info->timing.sample_rate    = FLASHEM_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_reset(void)
{
   /* The emulator has no reset entry point of its own yet; the disc path is
    * not kept here either, so say so rather than pretend a reset happened. */
   log_cb(RETRO_LOG_WARN, "FlashEm: reset is not implemented yet\n");
}

static uint32_t flashem_poll_buttons(void)
{
   static const struct { unsigned id; uint32_t mask; } map[] = {
      { RETRO_DEVICE_ID_JOYPAD_UP,     VFLASH_BTN_UP     },
      { RETRO_DEVICE_ID_JOYPAD_DOWN,   VFLASH_BTN_DOWN   },
      { RETRO_DEVICE_ID_JOYPAD_LEFT,   VFLASH_BTN_LEFT   },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT,  VFLASH_BTN_RIGHT  },
      { RETRO_DEVICE_ID_JOYPAD_A,      VFLASH_BTN_RED    },
      { RETRO_DEVICE_ID_JOYPAD_B,      VFLASH_BTN_YELLOW },
      { RETRO_DEVICE_ID_JOYPAD_X,      VFLASH_BTN_GREEN  },
      { RETRO_DEVICE_ID_JOYPAD_Y,      VFLASH_BTN_BLUE   },
      { RETRO_DEVICE_ID_JOYPAD_START,  VFLASH_BTN_ENTER  },
   };
   uint32_t buttons = 0;
   size_t i;

   if (!input_state_cb)
      return 0;

   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
   {
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
         buttons |= map[i].mask;
   }

   return buttons;
}

void retro_run(void)
{
   uint32_t *fb;
   Audio    *audio;

   if (!s_vf)
      return;

   if (input_poll_cb)
      input_poll_cb();

   vflash_set_input(s_vf, flashem_poll_buttons());
   vflash_run_frame(s_vf);

   fb = vflash_get_framebuffer(s_vf);
   if (fb && video_cb)
      video_cb(fb, VFLASH_SCREEN_W, VFLASH_SCREEN_H,
               VFLASH_SCREEN_W * sizeof(uint32_t));

   /* Whatever the frame produced, in one batch. Left in the ring buffer it
    * would only be overwritten by the next one. */
   audio = (Audio*)vflash_get_audio(s_vf);
   if (audio && audio_batch_cb)
   {
      uint32_t got = audio_pull_samples(audio, s_audio, FLASHEM_AUDIO_MAX);

      /* The buffer is interleaved stereo, so a frame is two samples. An odd
       * count would put the channels out of step from here on. */
      if (got >= 2)
         audio_batch_cb(s_audio, got / 2);
   }
}

bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   static const struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Red" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Yellow" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Green" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Blue" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Enter" },
      { 0, 0, 0, 0, NULL },
   };

   if (!game || !game->path)
   {
      log_cb(RETRO_LOG_ERROR, "FlashEm: no content path\n");
      return false;
   }

   /* XRGB8888 because that is what the framebuffer already is; a frontend that
    * refuses it has nothing to draw. */
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "FlashEm: the frontend refused XRGB8888\n");
      return false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);

   s_vf = vflash_create(game->path);
   if (!s_vf)
   {
      log_cb(RETRO_LOG_ERROR, "FlashEm: could not open '%s'\n", game->path);
      return false;
   }

   /* No device to open here - the samples go out through retro_run - but the
    * emulator gates WAV playback on the audio being live, so say that it is. */
   audio_init_external((Audio*)vflash_get_audio(s_vf));

   log_cb(RETRO_LOG_INFO, "FlashEm: loaded '%s'\n", game->path);
   return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info,
                             size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

void retro_unload_game(void)
{
   if (s_vf)
   {
      vflash_destroy(s_vf);
      s_vf = NULL;
   }
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* Save states need a serialiser the emulator does not have yet: the ARM core,
 * the MMU, the CD-ROM position and the JIT's state would all have to go out and
 * come back. Claiming a size here and writing nothing would break rewind,
 * run-ahead and netplay in ways that look like emulation bugs. */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
