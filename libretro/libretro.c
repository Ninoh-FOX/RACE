#include "libretro.h"
#include "libretro_core_options.h"
#include "log.h"
#include <string.h>
#include <streams/file_stream.h>

#include "../types.h"
#include "../state.h"
#include "../neopopsound.h"
#include "../neopop_blip.h"
#include "../sound.h"
#include "../flash.h"
#include "../tlcs900h.h"
#include "../race-memory.h"
#include "../graphics.h"
#include "../state.h"
#include "../ngp_link.h"
#include "../ngp_machine.h"
#include "ngp_dual.h"

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

#define RACE_NAME_MODULE "race"
#define RACE_NAME "RACE"
#define RACE_VERSION "v2.16"
#define RACE_EXTENSIONS "ngp|ngc|ngpc|npc"
#define RACE_TIMING_FPS 60.25
#define RACE_GEOMETRY_BASE_W 160
#define RACE_GEOMETRY_BASE_H 152
#define RACE_GEOMETRY_MAX_W 160
#define RACE_GEOMETRY_MAX_H 152
#define RACE_GEOMETRY_ASPECT_RATIO 1.05

#define FB_WIDTH 160
#define FB_HEIGHT 152

uint8_t ngpInputState = 0;

/* Frameskipping Support */

static unsigned frameskip_type             = 0;
static unsigned frameskip_threshold        = 0;
static uint16_t frameskip_counter          = 0;

static bool retro_audio_buff_active        = false;
static unsigned retro_audio_buff_occupancy = 0;
static bool retro_audio_buff_underrun      = false;
/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 60

static unsigned audio_latency              = 0;
static bool update_audio_latency           = false;

/* Dual link (ngp_dual.cpp): both Neo Geo Pockets of a netplay session on
 * each console, only the buttons over the network */
static bool dual_enabled                   = false; /* Core option, at load */
static bool dual_linked                    = false; /* Both machines on the cable */
static unsigned dual_my_slot               = 0;
static uint32_t dual_slot_keys[2];
static uint8_t *dual_rom                   = NULL;  /* The ROM as loaded (no save) */
static size_t dual_rom_size                = 0;
static char dual_rom_name[_MAX_PATH];
static int dual_saved_rtc                  = 1;
static void dual_register(void);
static void race_audio_frame(void);
static void dual_idle_frame(void);
static void dual_run_frame(void);

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frameskip disabled - frontend does not support audio buffer status monitoring.\n");

         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         audio_latency              = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / (float)RACE_TIMING_FPS;

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }

   update_audio_latency = true;
}

/* core options */
static int RETRO_SAMPLE_RATE = 44100;

/* The NGPC has no sample-based sound hardware: audio is synthesised in real
 * time, so the output sample rate is a free choice. The configurable rate is
 * capped here, and the per-frame audio buffers are sized from that cap so they
 * cannot overflow regardless of the selected rate.
 *
 * The buffers carry a headroom guard on top of the nominal samples-per-frame:
 * the emulated CPU runs slightly more than clock/60 cycles per frame (the
 * per-line loop overshoots its budget a little), so the band-limited path can
 * legitimately produce a few more samples than sample_rate/60 in a frame
 * (measured worst case ~805 at 48 kHz vs a nominal 800). The guard lets the
 * frame drain completely instead of leaving a residue that would otherwise
 * accumulate as growing latency. */
#define AUDIO_MAX_SAMPLE_RATE     48000
#define AUDIO_FRAME_GUARD         128
#define AUDIO_SAMPLES_PER_FRAME   ((AUDIO_MAX_SAMPLE_RATE / 60) + AUDIO_FRAME_GUARD)

struct ngp_screen* screen;
int setting_ngp_language; /* 0x6F87 - language */

int gfx_hacks = 0;
int tipo_consola = 0; /* 0x6F91 - OS version */
static bool libretro_supports_input_bitmasks;

char retro_save_directory[2048];

struct map
{
   unsigned retro;
   unsigned ngp;
};

static struct map btn_map[] = {
   { RETRO_DEVICE_ID_JOYPAD_A, 0x20 },
   { RETRO_DEVICE_ID_JOYPAD_B, 0x10 },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x08 },
   { RETRO_DEVICE_ID_JOYPAD_LEFT, 0x04 },
   { RETRO_DEVICE_ID_JOYPAD_UP, 0x01 },
   { RETRO_DEVICE_ID_JOYPAD_DOWN, 0x02 },
   { RETRO_DEVICE_ID_JOYPAD_START, 0x40 },
};

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void graphics_paint(unsigned char render)
{
   video_cb((bool)render ? screen->pixels : NULL,
         screen->w, screen->h, FB_WIDTH << 1);
}

static void check_variables(bool first_run)
{
   struct retro_variable var  = {0};
   unsigned dark_filter_level = 0;
   unsigned old_frameskip_type;

   if (first_run)
   {
      var.key = "race_language";
      var.value = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         /* user must manually restart core for change to happen
          * > 0: English
          * > 1: Japanese
          */
         if (!strcmp(var.value, "japanese"))
            setting_ngp_language = 1;
         else if (!strcmp(var.value, "english"))
            setting_ngp_language = 0;
      }
   }

   var.key   = "race_dark_filter_level";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      dark_filter_level = (unsigned)(atoi(var.value));
   graphicsSetDarkFilterLevel(dark_filter_level);

   old_frameskip_type = frameskip_type;
   frameskip_type     = 0;
   var.key            = "race_frameskip";
   var.value          = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
         frameskip_type = 1;
      else if (!strcmp(var.value, "manual"))
         frameskip_type = 2;
   }

   frameskip_threshold = 33;
   var.key             = "race_frameskip_threshold";
   var.value           = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   /* Reinitialise frameskipping, if required */
   if ((frameskip_type != old_frameskip_type) && !first_run)
      init_frameskip();

   if (first_run)
   {
      var.key   = "race_dual_link";
      var.value = NULL;
      dual_enabled = !(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value
            && !strcmp(var.value, "disabled"));
   }

   var.key   = "race_rtc_mode";
   var.value = NULL;
   {
      int rtc = 1;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
         rtc = strcmp(var.value, "realtime") ? 1 : 0;
      /* While linked the clock stays deterministic: the option applies
       * once the link ends */
      if (dual_linked)
         dual_saved_rtc = rtc;
      else
         rtc_deterministic = rtc;
   }

   /* Audio quality is read on (re)start only, so the synthesis path does not
    * change mid-frame. The fast path is the default. */
   if (first_run)
   {
      var.key   = "race_audio_quality";
      var.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
         neopop_audio_accurate = strcmp(var.value, "accurate") ? 0 : 1;
      else
         neopop_audio_accurate = 0;
   }

   /* Output sample rate. Read on (re)start only: it is reported to the frontend
    * in retro_get_system_av_info() and feeds the synthesis init, both fixed at
    * load time. "auto" asks the frontend for its target rate and picks the
    * nearest supported value; otherwise the literal rate is used. Supported
    * rates are capped at AUDIO_MAX_SAMPLE_RATE so the audio buffers (sized from
    * that cap) cannot overflow. */
   if (first_run)
   {
      RETRO_SAMPLE_RATE = 44100;
      var.key   = "race_audio_samplerate";
      var.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "auto"))
         {
            /* GET_TARGET_SAMPLE_RATE writes an unsigned int (Hz), not a float. */
            unsigned target = 0;
            if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &target)
                  && target > 0)
            {
               /* Pick the nearest supported rate to the host's target. */
               if (target >= (44100 + 48000) / 2)
                  RETRO_SAMPLE_RATE = 48000;
               else if (target >= (32000 + 44100) / 2)
                  RETRO_SAMPLE_RATE = 44100;
               else
                  RETRO_SAMPLE_RATE = 32000;
            }
            /* If the frontend can't report a target, keep the 44100 default. */
         }
         else
         {
            int rate = atoi(var.value);
            if (rate > AUDIO_MAX_SAMPLE_RATE)
               rate = AUDIO_MAX_SAMPLE_RATE;
            if (rate == 48000 || rate == 44100 || rate == 32000)
               RETRO_SAMPLE_RATE = rate;
         }
      }
   }
}

void retro_init(void)
{
   char *dir = NULL;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   audio_latency              = 0;
   update_audio_latency       = false;

   /* set up some logging */
   init_log(environ_cb);

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      sprintf(retro_save_directory, "%s%c", dir, path_default_slash_c());

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[RACE]: Save directory: %s.\n", retro_save_directory);

   if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt) && log_cb)
      log_cb(RETRO_LOG_ERROR, "[could not set RGB565]\n");

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_input_bitmasks = true;
}

void retro_reset(void)
{
   /* Linked: both consoles restart both machines in the same frame */
   if (dual_active())
   {
      dual_request_reset();
      return;
   }
   flashShutdown();
   system_sound_chipreset(RETRO_SAMPLE_RATE);
   mainemuinit();
}

void retro_deinit(void)
{
    flashShutdown();
    libretro_supports_input_bitmasks = false;
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   static const struct retro_system_content_info_override content_overrides[] = {
      {
         RACE_EXTENSIONS, /* extensions */
#if defined(LOW_MEMORY)
         true,            /* need_fullpath */
#else
         false,           /* need_fullpath */
#endif
         false            /* persistent_data */
      },
      { NULL, false, false }
   };

   environ_cb = cb;

   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   libretro_set_core_options(environ_cb);
   environ_cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE,
         (void*)content_overrides);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static bool race_initialize_system(const char *gamepath,
      const unsigned char *gamedata, size_t gamesize)
{
   mainemuinit();

   if (!handleInputFile(gamepath, gamedata, (int)gamesize))
   {
      handle_error("ERROR handleInputFile");
      return false;
   }

   return true;
}

void retro_set_controller_port_device(unsigned a, unsigned b) { }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = RACE_NAME;
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif

   info->need_fullpath    = true;
   info->library_version  = RACE_VERSION GIT_VERSION;
   info->valid_extensions = RACE_EXTENSIONS;
   info->block_extract    = false;
}
void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = RACE_TIMING_FPS;
   info->timing.sample_rate    = RETRO_SAMPLE_RATE;
   info->geometry.base_width   = RACE_GEOMETRY_BASE_W;
   info->geometry.base_height  = RACE_GEOMETRY_BASE_H;
   info->geometry.max_width    = RACE_GEOMETRY_MAX_W;
   info->geometry.max_height   = RACE_GEOMETRY_MAX_H;
   info->geometry.aspect_ratio = RACE_GEOMETRY_ASPECT_RATIO;
}

#define CPU_FREQ 6144000

void retro_run(void)
{
   bool updated = false;
   int skipFrame = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);

   /* The dual link: both machines run here, in step with the other console */
   if (dual_setting_up())
   {
      dual_idle_frame();
      return;
   }
   if (dual_running())
   {
      dual_run_frame();
      return;
   }

   ngpInputState = 0;
   input_poll_cb();
   if (libretro_supports_input_bitmasks)
   {
	   unsigned i = 0;
	   unsigned ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	   for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
		   ngpInputState |= (ret & (1 << btn_map[i].retro)) ? btn_map[i].ngp : 0;
   }
   else
   {
	   unsigned i   = 0;
	   for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
		   ngpInputState |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, btn_map[i].retro) ? btn_map[i].ngp : 0;
   }

   /* Advance the deterministic RTC by one frame. */
   rtc_tick_frame();

   /* Check whether current frame should be skipped */
   if ((frameskip_type > 0) && retro_audio_buff_active)
   {
      switch (frameskip_type)
      {
         case 1: /* auto */
            skipFrame = retro_audio_buff_underrun ? 1 : 0;
            break;
         case 2: /* manual */
            skipFrame = (retro_audio_buff_occupancy < frameskip_threshold) ? 1 : 0;
            break;
         default:
            skipFrame = 0;
            break;
      }

      if (!skipFrame || (frameskip_counter >= FRAMESKIP_MAX))
      {
         skipFrame         = 0;
         frameskip_counter = 0;
      }
      else
         frameskip_counter++;
   }

   /* If the frontend has disabled video (e.g. run-ahead or fast-forward frames
    * whose output it will discard), skip rendering this frame. The NGPC
    * renderer is a pure function of emulation state - it reads VRAM and writes
    * pixels without feeding anything back - so skipping it leaves the next
    * frame's output and savestates unchanged, as the environment call requires. */
   {
      int av_enable = 0x7; /* default: video+audio enabled if unsupported */
      if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_enable))
      {
         if (!(av_enable & 0x1)) /* bit 0 clear: video disabled */
            skipFrame = 1;
      }
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   tlcs_execute(CPU_FREQ / 60, skipFrame);
   race_audio_frame();
}

/* The sound of the frame that just ran */
static void race_audio_frame(void)
{
   unsigned i;
   static int16_t sampleBuffer[AUDIO_SAMPLES_PER_FRAME];
   static int16_t stereoBuffer[AUDIO_SAMPLES_PER_FRAME * 2];
   int16_t *p = NULL;
   uint16_t samplesPerFrame;

   /* Get the number of samples in a frame */
   samplesPerFrame = RETRO_SAMPLE_RATE / 60;

   if (neopop_audio_accurate)
   {
      /* Band-limited path: transitions were emitted into the Blip buffer as
       * the CPU ran (via soundStep); flush them to interleaved stereo here.
       * The cap is the buffer's capacity in stereo frames (it holds
       * AUDIO_SAMPLES_PER_FRAME pairs), so the drain can never overrun it. */
      int n = neopop_blip_flush(stereoBuffer, AUDIO_SAMPLES_PER_FRAME);
      if (n > 0)
         audio_batch_cb(stereoBuffer, n);
   }
   else
   {
      /* sound_update overwrites every sample in the frame, so no clear is
       * needed first; dac_update then mixes the DAC channel on top. */
      sound_update((uint16_t*)sampleBuffer, samplesPerFrame * sizeof(int16_t));
      dac_update((uint16_t*)sampleBuffer, samplesPerFrame * sizeof(int16_t));

      p = stereoBuffer;

      for (i = 0; i < samplesPerFrame; i++)
      {
         p[0] = sampleBuffer[i];
         p[1] = sampleBuffer[i];
         p += 2;
      }

      audio_batch_cb(stereoBuffer, samplesPerFrame);
   }
}


/* ---- Dual link -------------------------------------------------------- */

/* The pad, as it travels (the NGP's own byte) */
static uint32_t dual_local_keys(void)
{
   unsigned i;
   uint32_t keys = 0;
   input_poll_cb();
   if (libretro_supports_input_bitmasks)
   {
      unsigned ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
         keys |= (ret & (1 << btn_map[i].retro)) ? btn_map[i].ngp : 0;
   }
   else
      for (i = 0; i < sizeof(btn_map) / sizeof(struct map); i++)
         keys |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, btn_map[i].retro) ? btn_map[i].ngp : 0;
   return keys;
}

/* A frame that does not run (the link waits): the same picture, silence */
static void dual_idle_frame(void)
{
   static int16_t silence[AUDIO_SAMPLES_PER_FRAME * 2];
   unsigned frames = RETRO_SAMPLE_RATE / 60;
   if (frames > AUDIO_SAMPLES_PER_FRAME)
      frames = AUDIO_SAMPLES_PER_FRAME;
   video_cb(NULL, FB_WIDTH, FB_HEIGHT, FB_WIDTH << 1);
   audio_batch_cb(silence, frames);
}

/* The machine of a slot: ours is instance A, the other one instance B */
static int dual_is_own(unsigned slot)
{
   return slot == dual_my_slot;
}

static void m_frame_begin(unsigned slot, uint8_t keys)
{
   if (dual_is_own(slot)) ngpm_frame_begin(keys); else ngpb_ngpm_frame_begin(keys);
}
static void m_run(unsigned slot, int cycles, int render)
{
   if (dual_is_own(slot)) ngpm_run(cycles, render); else ngpb_ngpm_run(cycles, 0);
}
static int m_take(unsigned slot, uint8_t *b)
{
   return dual_is_own(slot) ? ngpm_take(b) : ngpb_ngpm_take(b);
}
static void m_give(unsigned slot, uint8_t b)
{
   if (dual_is_own(slot)) ngpm_give(b); else ngpb_ngpm_give(b);
}
static uint8_t m_rts(unsigned slot)
{
   return dual_is_own(slot) ? ngpm_rts() : ngpb_ngpm_rts();
}
static void m_cts(unsigned slot, uint8_t on)
{
   if (dual_is_own(slot)) ngpm_cts(on); else ngpb_ngpm_cts(on);
}

/* One frame of both machines, a line (515 cycles) of each in turn, slot 0
 * first on both consoles; what one sends gets to the other before its
 * next line runs. */
static void dual_link_frame(int render)
{
   int budget = CPU_FREQ / 60;
   unsigned s;
   for (s = 0; s < 2; s++)
      m_frame_begin(s, (uint8_t)dual_slot_keys[s]);
   while (budget > 0)
   {
      const int c = budget < 515 ? budget : 515;
      for (s = 0; s < 2; s++)
      {
         uint8_t b;
         m_cts(s, m_rts(s ^ 1));
         m_run(s, c, render && dual_is_own(s));
         while (m_take(s, &b))
            m_give(s ^ 1, b);
      }
      budget -= c;
   }
}

static void dual_run_frame(void)
{
   if (dual_frame_begin(dual_local_keys()) != DUAL_FRAME_RUN || !dual_linked)
   {
      dual_idle_frame();
      return;
   }
   dual_link_frame(1);
   race_audio_frame();
   dual_frame_end();
}

static uint32_t dual_fnv(uint32_t h, const void *data, size_t n)
{
   const uint8_t *p = (const uint8_t*)data;
   while (n--)
      h = (h ^ *p++) * 16777619u;
   return h;
}

/* What makes this machine another one: both consoles must agree */
static uint32_t dual_machine_cfg(void)
{
   uint32_t cfg = 2166136261u;
   int v[2];
   v[0] = tipo_consola;
   v[1] = setting_ngp_language;
   return dual_fnv(cfg, v, sizeof(v));
}

static void put32le(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32le(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The machine sent to the other console: its state, then its save (the
 * bytes of the cartridge flash that differ from the ROM as loaded), as
 * runs of (offset, length, bytes). */
static bool dual_hook_capture(uint8_t **blob, size_t *size)
{
   const size_t ss = ngpm_state_size();
   const uint8_t *rom = ngpm_rom();
   size_t cap = 16 + ss + 64, len, i;
   uint8_t *out;

   /* The game restarts (games look for the cable at boot), save kept */
   system_sound_chipreset(RETRO_SAMPLE_RATE);
   mainemuinit();
   ngp_sio_reset();

   for (i = 0; i < MAINROM_SIZE_MAX; )
   {
      size_t j = i;
      while (j < MAINROM_SIZE_MAX && rom[j] != dual_rom[j]) j++;
      if (j > i) { cap += 8 + (j - i); i = j; } else i++;
   }
   if (!(out = (uint8_t*)malloc(cap)))
      return false;
   /* (8 byte header: the state is a struct with 64 bit fields, ARM wants
    * it aligned) */
   put32le(out, (uint32_t)ss);
   put32le(out + 4, 0);
   if (!ngpm_save(out + 8))
   {
      free(out);
      return false;
   }
   len = 8 + ss;
   for (i = 0; i < MAINROM_SIZE_MAX; )
   {
      size_t j = i;
      while (j < MAINROM_SIZE_MAX && rom[j] != dual_rom[j]) j++;
      if (j > i)
      {
         put32le(out + len, (uint32_t)i);
         put32le(out + len + 4, (uint32_t)(j - i));
         memcpy(out + len + 8, rom + i, j - i);
         len += 8 + (j - i);
         i = j;
      }
      else
         i++;
   }
   *blob = out;
   *size = len;
   return true;
}

/* A machine from a captured blob: its save onto the ROM, then its state */
static bool dual_apply_blob(int hidden, const uint8_t *blob, size_t size)
{
   uint32_t ss;
   size_t pos;
   uint8_t *rom = hidden ? ngpb_ngpm_rom() : ngpm_rom();
   if (size < 8 || (ss = get32le(blob)) > size - 8)
      return false;
   pos = 8 + ss;
   if (hidden)
   {
      while (pos + 8 <= size)
      {
         const uint32_t off = get32le(blob + pos), n = get32le(blob + pos + 4);
         if (off > MAINROM_SIZE_MAX || n > MAINROM_SIZE_MAX - off || n > size - pos - 8)
            return false;
         memcpy(rom + off, blob + pos + 8, n);
         pos += 8 + n;
      }
      return ngpb_ngpm_load(blob + 8) != 0;
   }
   return ngpm_load(blob + 8) != 0;
}

static bool dual_hook_link(unsigned my_slot, const uint8_t *own, size_t own_size,
      const uint8_t *peer, size_t peer_size)
{
   ngpb_ngpm_config(tipo_consola, setting_ngp_language, 0);
   if (!ngpb_ngpm_load_rom(dual_rom, dual_rom_size, dual_rom_name)
         || !dual_apply_blob(0, own, own_size)
         || !dual_apply_blob(1, peer, peer_size))
      return false;
   dual_my_slot   = my_slot;
   dual_saved_rtc = rtc_deterministic;
   rtc_deterministic = 1;
   ngpm_link(1);
   ngpb_ngpm_link(1);
   dual_slot_keys[0] = dual_slot_keys[1] = 0;
   dual_linked = true;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "NGP dual link: this Neo Geo Pocket is slot %u\n", my_slot);
   return true;
}

static void dual_hook_unlink(void)
{
   if (!dual_linked)
      return;
   ngpm_link(0);
   ngpb_ngpm_link(0);
   rtc_deterministic = dual_saved_rtc;
   dual_linked = false;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "NGP dual link ended\n");
}

static void dual_hook_set_keys(unsigned slot, uint32_t keys)
{
   if (slot < 2)
      dual_slot_keys[slot] = keys & 0xFF;
}

static void dual_hook_reset_all(void)
{
   unsigned s;
   for (s = 0; s < 2; s++)
   {
      if (dual_is_own(s))
      {
         system_sound_chipreset(RETRO_SAMPLE_RATE);
         mainemuinit();
         ngpm_link(1);
      }
      else
      {
         ngpb_ngpm_reset();
         ngpb_ngpm_link(1);
      }
   }
}

static uint32_t dual_hook_hash(unsigned slot)
{
   return dual_is_own(slot) ? ngpm_hash() : ngpb_ngpm_hash();
}

static void dual_hook_message(const char *text, unsigned ms)
{
   struct retro_message msg;
   msg.msg    = text;
   msg.frames = ms * 60 / 1000;
   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

static void RETRO_CALLCONV dual_np_start(uint16_t client_id, retro_netpacket_send_t send_fn,
      retro_netpacket_poll_receive_t poll_receive_fn)
{
   dual_set_cfg(dual_machine_cfg());
   dual_net_start(client_id, send_fn, poll_receive_fn);
}
static void RETRO_CALLCONV dual_np_receive(const void *buf, size_t len, uint16_t client_id)
{
   dual_net_receive(buf, len, client_id);
}
static void RETRO_CALLCONV dual_np_stop(void)
{
   dual_net_stop();
}
static bool RETRO_CALLCONV dual_np_connected(uint16_t client_id)
{
   return dual_net_connected(client_id);
}
static void RETRO_CALLCONV dual_np_disconnected(uint16_t client_id)
{
   dual_net_disconnected(client_id);
}

/* Netplay with this game is the dual link: only the buttons travel */
static void dual_register(void)
{
   static const struct retro_netpacket_callback iface = {
      dual_np_start,
      dual_np_receive,
      dual_np_stop,
      NULL,
      dual_np_connected,
      dual_np_disconnected,
      "race-dual-1",
   };
   static const struct dual_hooks hooks = {
      dual_hook_capture,
      dual_hook_link,
      dual_hook_unlink,
      dual_hook_set_keys,
      dual_hook_reset_all,
      dual_hook_hash,
      dual_hook_message,
   };
   dual_set_hooks(&hooks);
   dual_game_loaded(dual_fnv(2166136261u, dual_rom, dual_rom_size),
         dual_machine_cfg(), RACE_TIMING_FPS);
   if (environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&iface) && log_cb)
      log_cb(RETRO_LOG_INFO, "NGP dual link ready for netplay\n");
}

size_t retro_serialize_size(void)
{
   return state_get_size();
}

bool retro_serialize(void *data, size_t size)
{
   /* The copy of this machine on the other console could not follow */
   if (dual_active())
      return false;
   if (size < state_get_size())
      return false;
   return state_store_mem(data);
}

bool retro_unserialize(const void *data, size_t size)
{
   int ret;
   if (dual_active())
      return false;
   if (size < state_get_size())
      return false;
   ret = state_restore_mem((void*)data);
   return (ret == 1);
}

bool retro_load_game(const struct retro_game_info *info)
{
   const struct retro_game_info_ext *info_ext = NULL;
   const unsigned char *content_data          = NULL;
   size_t content_size                        = 0;
   char content_path[_MAX_PATH];

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Option" },

      { 0 },
   };

   content_path[0] = '\0';

   /* Attempt to fetch extended game info */
   if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info_ext))
   {
#if !defined(LOW_MEMORY)
      content_data = (const unsigned char *)info_ext->data;
      content_size = info_ext->size;
#endif
      if (info_ext->file_in_archive)
      {
         /* We don't have a 'physical' file in this
          * case, but the core still needs a filename
          * in order to build the save file path.
          * We therefore fake it, using the content
          * directory, canonical content name, and
          * content file extension */
         snprintf(content_path, sizeof(content_path), "%s%c%s.%s",
               info_ext->dir, path_default_slash_c(),
               info_ext->name, info_ext->ext);
      }
      else
      {
         strncpy(content_path, info_ext->full_path, sizeof(content_path));
         content_path[sizeof(content_path) - 1] = '\0';
      }
   }
   else
   {
      if (!info || !info->path)
         return false;

      strncpy(content_path, info->path, sizeof(content_path));
      content_path[sizeof(content_path) - 1] = '\0';
   }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   /* If a previous load left a screen allocated (e.g. frontend re-loads
    * without unloading), release it before allocating a new one. */
   if (screen)
   {
      free(screen->pixels);
      free(screen);
      screen = NULL;
   }

   screen         = (struct ngp_screen*)calloc(1, sizeof(*screen));

   if (!screen)
      return false;

   screen->w      = FB_WIDTH;
   screen->h      = FB_HEIGHT;

   screen->pixels = calloc(1, FB_WIDTH * FB_HEIGHT * 2);

   if (!screen->pixels)
   {
      free(screen);
      screen = NULL;
      return false;
   }

   check_variables(true);
   init_frameskip();

   if (!race_initialize_system(content_path,
         content_data, content_size))
   {
      free(screen->pixels);
      free(screen);
      screen = NULL;
      return false;
   }

   system_sound_chipreset(RETRO_SAMPLE_RATE);

   /* The dual link needs the ROM as it was loaded (the save is overlaid
    * on mainrom), to load the other console's machine and its save */
   /* (Any clock: the link makes it deterministic while it lasts) */
   if (dual_enabled)
   {
      free(dual_rom);
      dual_rom_size = 0;
      if ((dual_rom = (uint8_t*)malloc(MAINROM_SIZE_MAX)))
      {
         memset(dual_rom, 0xFF, MAINROM_SIZE_MAX);
         if (content_data && content_size)
         {
            dual_rom_size = content_size > MAINROM_SIZE_MAX ? MAINROM_SIZE_MAX : content_size;
            memcpy(dual_rom, content_data, dual_rom_size);
         }
         else
         {
            RFILE *f = filestream_open(content_path, RETRO_VFS_FILE_ACCESS_READ,
                  RETRO_VFS_FILE_ACCESS_HINT_NONE);
            if (f)
            {
               int64_t n = filestream_read(f, dual_rom, MAINROM_SIZE_MAX);
               dual_rom_size = n > 0 ? (size_t)n : 0;
               filestream_close(f);
            }
         }
         strncpy(dual_rom_name, content_path, sizeof(dual_rom_name) - 1);
         dual_rom_name[sizeof(dual_rom_name) - 1] = '\0';
         if (dual_rom_size)
            dual_register();
         else
         {
            free(dual_rom);
            dual_rom = NULL;
         }
      }
   }

   {
      /* TODO: Mappings might need updating
       * Size is based on what is exposed in Mednafen NGP */
      struct retro_memory_descriptor descs = {
         RETRO_MEMDESC_SYSTEM_RAM, mainram, 0, 0, 0, 0, 16384, "RAM"
      };
      struct retro_memory_map retro_map = {
         &descs, 1
      };
      environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retro_map);
   }

   return true;
}

bool retro_load_game_special(unsigned a, const struct retro_game_info *b, size_t c)
{
   return false;
}

void retro_unload_game(void)
{
   dual_shutdown();
   free(dual_rom);
   dual_rom      = NULL;
   dual_rom_size = 0;
   if (screen)
   {
      if (screen->pixels)
         free(screen->pixels);
      free(screen);
      screen = NULL;
   }
   ngpInputState = 0;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned type)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
   return 0;
}
