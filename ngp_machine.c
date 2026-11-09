/* The machine as the dual link drives it (libretro/libretro.c). Built
 * into both instances: instance A's functions are ngpm_*, the hidden
 * instance B's ones the same with every global renamed ngpb_* (see the
 * Makefile), so each call reaches its own machine.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <string.h>

#include "ngp_machine.h"
#include "ngp_link.h"
#include "race-memory.h"
#include "tlcs900h.h"
#include "state.h"
#include "main.h"

extern int tipo_consola;
extern int setting_ngp_language;
extern int neopop_audio_accurate;
extern int finscan;
extern int rtc_deterministic;
void system_sound_chipreset(int sample_rate);
void rtc_tick_frame(void);
int handleInputFile(const char *romName, const unsigned char *romData, int romSize);

void ngpm_config(int console_type, int language, int accurate_audio)
{
   tipo_consola          = console_type;
   setting_ngp_language  = language;
   neopop_audio_accurate = accurate_audio;
   /* Both machines on both consoles count the same clock */
   rtc_deterministic     = 1;
}

int ngpm_load_rom(const uint8_t *rom, size_t size, const char *name)
{
   mainemuinit();
   if (!handleInputFile(name, rom, (int)size))
      return 0;
   system_sound_chipreset(44100);
   return 1;
}

/* Starts over; the save (flash, in mainrom) stays */
void ngpm_reset(void)
{
   system_sound_chipreset(44100);
   mainemuinit();
   ngp_sio_reset();
}

size_t ngpm_state_size(void)
{
   return (size_t)state_get_size();
}

int ngpm_save(void *buf)
{
   return state_store_mem(buf);
}

int ngpm_load(const void *buf)
{
   return state_restore_mem((void *)buf) == 1;
}

void ngpm_frame_begin(uint8_t keys)
{
   ngpInputState = keys;
   rtc_tick_frame();
}

void ngpm_run(int cycles, int render)
{
   tlcs_execute(cycles, !render);
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
   while (n--)
      h = (h ^ *p++) * 16777619u;
   return h;
}

/* What the game can see: work RAM, video and I/O */
uint32_t ngpm_hash(void)
{
   uint32_t h = 2166136261u;
   h = fnv(h, mainram, 0xC000);
   h = fnv(h, cpuram, 0x8A0);
   return h;
}

void ngpm_link(int on)
{
   ngp_sio_reset();
   ngp_link_on = (uint8_t)(on ? 1 : 0);
}

int ngpm_take(uint8_t *data)
{
   return ngp_sio_take(data);
}

void ngpm_give(uint8_t data)
{
   ngp_sio_receive(data);
}

uint8_t ngpm_rts(void)
{
   return ngp_sio_rts();
}

void ngpm_cts(uint8_t on)
{
   ngp_link_cts = on;
}

uint8_t *ngpm_rom(void)
{
   return mainrom;
}

size_t ngpm_rom_size(void)
{
   return (size_t)m_emuInfo.romSize;
}
