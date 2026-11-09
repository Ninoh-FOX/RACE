/* What libretro/libretro.c gives the core, for the hidden instance B of the
 * dual link (built with -DRACE_INSTANCE_B instead of libretro.c): its own
 * screen (never shown), settings (copied from instance A by ngpm_config),
 * no frontend and no save directory (its save comes from the other
 * console, and it never writes one: see flash.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdint.h>
#include <stdlib.h>

#include "graphics.h"
#include "libretro.h"

#define FB_WIDTH 160
#define FB_HEIGHT 152

static unsigned short b_pixels[FB_WIDTH * FB_HEIGHT];
static struct ngp_screen b_screen = { FB_WIDTH, FB_HEIGHT, b_pixels };

struct ngp_screen *screen = &b_screen;
uint8_t ngpInputState;
char retro_save_directory[2048];
int setting_ngp_language;
int tipo_consola;
int gfx_hacks;

/* Never seen */
void graphics_paint(unsigned char render)
{
   (void)render;
}
