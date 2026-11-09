/* Neo Geo Pocket dual link: the session (ngp_dual.cpp), taken from
 * Gearsystem's Gear-to-Gear dual link (GPL-3.0-or-later; RACE is GPL-2.0
 * or later, so the combined core is GPL-3.0-or-later).
 *
 * Both Neo Geo Pockets of a netplay session run on each console, joined
 * by the cable in memory (ngp_link.c), and only the buttons travel over
 * the network. Instance A is the player's own machine: its ROM, its save,
 * its screen and sound. Instance B is a copy of the other console's one,
 * made from the state that console sends when the link starts; it is never
 * seen nor heard. Slot 0 is the netplay host's machine, slot 1 the
 * client's: each console runs both slots in the same order.
 */
#ifndef NGP_DUAL_H
#define NGP_DUAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buttons as they travel: the NGP pad byte (ngpInputState) */
#define DUAL_KEY_UP     0x01
#define DUAL_KEY_RESET  0x80000000u   /* With the buttons: both consoles restart */

// What libretro.cpp does for the session.
struct dual_hooks
{
    /* Restart the player's machine (keeping its save) and take its state:
     * a malloc'd buffer the session frees. */
    bool (*capture)(uint8_t** blob, size_t* size);
    // Both machines on the cable: ours from `own`, the other one from `peer`.
    // `my_slot` is the slot of ours.
    bool (*link)(unsigned my_slot, const uint8_t* own, size_t own_size,
                 const uint8_t* peer, size_t peer_size);
    // Back to the player's machine alone.
    void (*unlink)(void);
    // Buttons for the next frame, by slot.
    void (*set_keys)(unsigned slot, uint32_t keys);
    // Both machines restart, with the cable plugged.
    void (*reset_all)(void);
    // What the game of a slot can see, hashed.
    uint32_t (*hash)(unsigned slot);
    void (*message)(const char* text, unsigned ms);
};

void dual_set_hooks(const struct dual_hooks* hooks);
// The game: its identity (ROM, BIOS) and the settings that change the machine.
void dual_game_loaded(uint32_t rom_crc, uint32_t cfg, double fps);
void dual_set_cfg(uint32_t cfg);

// Netpacket interface (libretro.cpp passes the frontend's calls on).
void dual_net_start(uint16_t client_id, retro_netpacket_send_t send,
                    retro_netpacket_poll_receive_t poll_receive);
void dual_net_receive(const void* buf, size_t len, uint16_t client_id);
void dual_net_stop(void);
bool dual_net_connected(uint16_t client_id);
void dual_net_disconnected(uint16_t client_id);

// Whether the link is being set up (true: the game waits this frame).
bool dual_setting_up(void);
// Whether both machines are running.
bool dual_running(void);
// Whether a session uses (or may still use) the dual link: savestates,
// cheats and setting changes wait until it ends.
bool dual_active(void);
void dual_request_reset(void);

enum { DUAL_FRAME_WAIT = 0, DUAL_FRAME_RUN, DUAL_FRAME_HOLD };
int dual_frame_begin(uint32_t local_keys);
void dual_frame_end(void);
void dual_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NGP_DUAL_H */
