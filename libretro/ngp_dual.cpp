/*
 * Neo Geo Pocket dual link for RACE, from Gearsystem's Gear-to-Gear dual
 * link (Gearsystem - Sega Master System / Game Gear Emulator,
 * Copyright (C) 2013  Ignacio Sanchez)

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

// Dual NGP link link: the session. See ngp_dual.h.
//
//   host                                  client
//     HELLO (ROM, settings)  <-------->   HELLO
//     START (who is in each slot) ---->
//     PING/PONG (round trip)
//     GO                     -------->
//     STATE of its Neo Geo Pocket <-------->   STATE of its Neo Geo Pocket
//       (run length coded: after the restart it is mostly empty)
//     READY (input delay)    -------->
//     INPUT (frame, buttons) <-------->   INPUT, every frame
//     HASH (both Neo Geo Pockets) <-------->   HASH: the copies must agree
//
// Both games restart once when the link starts (games look for the cable at
// boot), each one keeping its own save. The buttons for frame N are sent at
// frame N - delay, and both consoles run frame N with the same buttons on
// both Neo Geo Pockets. A console that does not have the other's buttons yet
// shows the last frame again and tries later.
//
// Both consoles also show frame N at the same time, so that the screens
// (and the speakers) go together: each one tells the other how far ahead it
// thinks it is, and the one that is ahead waits a frame now and then.

#include "ngp_dual.h"
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define DUAL_HEADER        0x4E474431   // NGD1 (Neo Geo Pocket, Dual)
#define DUAL_VERSION       1

// Packets: header, type | sender's slot << 8 (FF: none yet), words, data.
#define DPK_HELLO          1   // version, rom, settings, host
#define DPK_PING           2   // time
#define DPK_PONG           3   // time (echoed)
#define DPK_STATE          4   // offset, total (coded) size, data
#define DPK_READY          5   // input delay
#define DPK_INPUT          6   // frame, buttons, current frame, lead, stalls
#define DPK_HASH           7   // frame, hash of each slot's Neo Geo Pocket
#define DPK_BYE            8
#define DPK_START          9   // client id of each slot
#define DPK_GO             10  // the states can go
#define DPK_GIVEUP         11  // no dual link in this session

#define NP                 2           // Neo Geo Pockets on the cable
#define DUAL_NO_SLOT       0xFF
#define DUAL_CHUNK         (16 * 1024)
#define DUAL_STATE_MAX     (5 * 1024 * 1024)  // State, and the save (flash) of a 4 MB cartridge
#define DUAL_RING          256         // Frames of buttons kept
#define DUAL_HASH_EVERY    120         // Frames between agreement checks
#define DUAL_HASH_SLOTS    4
#define DUAL_HELLO_US      6000000     // No answer: the other core is not dual
#define DUAL_HELLO_EVERY   250000      // HELLO sent again until the host's comes
#define DUAL_SETUP_US      30000000    // Whole setup
#define DUAL_PEER_US       15000000    // Peer silent while playing: it is gone
#define DUAL_STALL_US      12000       // Longest wait for buttons in a frame
#define DUAL_PINGS         8           // Round trips measured before the states
#define DUAL_PING_US       1000000     // ...for at most this long
#define DUAL_PONG_US       200000      // A ping with no answer: send another
#define DUAL_SLICE_US      10000       // Setup work per frame (it waits anyway)
#define DUAL_SYNC_WIN      16          // Packets a peer's position comes from
#define DUAL_SYNC_GAP      60          // Frames between two waits
#define DUAL_DELAY_MORE    4           // Most frames added to the input delay
#define DUAL_DELAY_CALM_US 60000000    // Peer happy this long: one frame less
#define DUAL_DELAY_CALM_MAX 600000000  // (twice as long each time that was wrong)

#define DUAL_MSG_DIFFER  "NGP link: games or settings differ, no link"

enum { DS_OFF = 0, DS_HELLO, DS_WAIT, DS_PING, DS_TRANSFER, DS_READY, DS_RUN };

static struct dual_hooks hk;

static struct
{
    retro_netpacket_send_t send;
    retro_netpacket_poll_receive_t poll_receive;

    u32 rom_crc;
    u32 cfg;                      // Settings that change the machine
    u32 frame_us;                 // How long a frame is

    int state;
    bool is_host;
    unsigned my_id;               // Our libretro client id (host: 0)
    unsigned n;                   // Neo Geo Pockets on the cable (once START is out)
    unsigned my_slot;
    int slot_id[NP];              // libretro client id of each slot's console
    u64 t_state;                  // When the current step started
    u64 t_heard[NP];              // Last packet from each slot

    // Client, before START
    bool hello_sent;
    u64 t_hello;
    bool go;                      // GO came

    // Host: round trips to the client
    u32 rtt[DUAL_PINGS], rtt_n;
    bool ping_out;
    u64 t_ping;

    u32 ow_us;                    // One way time between the consoles
    bool peer_ready;              // Client: READY came
    u32 delay;                    // Input delay, frames (agreed at the start)
    u32 send_delay;               // Ours now: more after the peer waited
    u32 stalls;                   // Frames we waited for the peer's buttons
    u32 stall_frame;              // (the last one counted, + 1)
    u32 peer_stalls;              // ...and the peer for ours
    u64 t_peer_stall;             // When the peer last waited for us
    u64 t_lowered;                // When we last took a frame off
    u64 calm_us;                  // How long it must be calm for that
    bool reset_wanted;            // The player asked for a reset

    std::vector<u8> own;          // Our Neo Geo Pocket
    std::vector<u8> own_pack;     // ...run length coded
    std::vector<u8> peer_pack;    // The peer's, coded, as it comes
    u32 peer_pack_len, peer_rcvd;
    bool linked;

    u32 frame;                    // Next frame to run
    u32 sent_upto;                // Buttons sent for frames < this
    u32 own_keys[DUAL_RING];
    u32 keys[DUAL_RING];          // The peer's buttons...
    u32 frame_ok[DUAL_RING];      // ...valid when this is frame + 1
    // Agreement checks: [frame + 1, each slot's Neo Geo Pocket], ours and the peer's
    u32 my_hash[DUAL_HASH_SLOTS][1 + NP];
    u32 peer_hash[DUAL_HASH_SLOTS][1 + NP];
    u32 hash_done;                // Last check made (frame + 1)
    u32 hash_bad;                 // Last one that differed (frame + 1)

    // Time sync. Where the peer is: from each INPUT, its frame (in us) at our
    // time 0; the best of the last ones is the least late.
    s64 peer_pos[DUAL_SYNC_WIN];
    u32 peer_pos_n;
    s32 peer_lead;                // How far ahead of us the peer says it is
    u32 sync_next;                // No waiting before this frame
} ds;

/* ---- helpers --------------------------------------------------------------- */

static u64 now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ULL + (u64)(ts.tv_nsec / 1000);
}

static u32 be32(u32 v)
{
    const u8* p = (const u8*)&v;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 word(const void* buf, unsigned i)
{
    u32 v;
    memcpy(&v, (const u8*)buf + 4 * i, 4);
    return be32(v);
}

static void dual_send(unsigned to, u32 type, const u32* words, u32 nwords,
                      const void* data, u32 len)
{
    static u8 buf[8 + 16 * 4 + DUAL_CHUNK];
    const u32 slot = ds.n ? ds.my_slot : DUAL_NO_SLOT;
    u32 i;
    if (!ds.send)
        return;
    put32(buf, DUAL_HEADER);
    put32(buf + 4, type | (slot << 8));
    for (i = 0; i < nwords; i++)
        put32(buf + 8 + 4 * i, words[i]);
    if (len)
        memcpy(buf + 8 + 4 * nwords, data, len);
    // Every packet as soon as possible: latency is all that matters here.
    ds.send(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf,
            8 + 4 * nwords + len, (uint16_t)to);
}

// To the other console: the host only ever talks to the client of slot 1,
// a client to the host.
static void dual_send_peer(u32 type, const u32* words, u32 nwords,
                           const void* data, u32 len)
{
    if (ds.is_host)
    {
        if (ds.slot_id[1] >= 0)
            dual_send((unsigned)ds.slot_id[1], type, words, nwords, data, len);
    }
    else
        dual_send(0, type, words, nwords, data, len);
}

static void dual_msg(const char* text)
{
    if (hk.message)
        hk.message(text, 3000);
}

static void poll_receive(void)
{
    if (ds.poll_receive)
        ds.poll_receive();
}

// Run length coding for the states sent at the start: after the restart
// they are mostly zeros. A control byte c < 80 is followed by c + 1 bytes as
// they are; otherwise the next byte is there (c & 7F) + 3 times.
static void rle_pack(std::vector<u8>& dst, const std::vector<u8>& src)
{
    const u32 len = (u32)src.size();
    u32 i = 0, lit = 0;
    dst.clear();
    dst.reserve(len + len / 128 + 16);
    while (i <= len)
    {
        u32 run = 0;
        if (i < len)
            for (run = 1; i + run < len && run < 130 && src[i + run] == src[i]; run++);
        if (i == len || run >= 3)
        {
            while (lit < i)
            {
                const u32 n = MIN(128, i - lit);
                dst.push_back((u8)(n - 1));
                dst.insert(dst.end(), src.begin() + lit, src.begin() + lit + n);
                lit += n;
            }
            if (i == len)
                break;
            dst.push_back((u8)(0x80 | (run - 3)));
            dst.push_back(src[i]);
            lit = i + run;
        }
        i += run;
    }
}

static bool rle_unpack(std::vector<u8>& dst, const u8* src, u32 slen)
{
    u32 i = 0;
    dst.clear();
    while (i < slen)
    {
        const u32 c = src[i++];
        if (c < 0x80)
        {
            if (i + c + 1 > slen || dst.size() + c + 1 > DUAL_STATE_MAX)
                return false;
            dst.insert(dst.end(), src + i, src + i + c + 1);
            i += c + 1;
        }
        else
        {
            if (i >= slen || dst.size() + (c & 0x7F) + 3 > DUAL_STATE_MAX)
                return false;
            dst.insert(dst.end(), (c & 0x7F) + 3, src[i++]);
        }
    }
    return true;
}

/* ---- session ------------------------------------------------------------- */

static void dual_free_states(void)
{
    std::vector<u8>().swap(ds.own);
    std::vector<u8>().swap(ds.own_pack);
    std::vector<u8>().swap(ds.peer_pack);
}

// Back to a single Neo Geo Pocket: the player's one goes on, unplugged.
static void dual_stop(const char* why)
{
    if (ds.state == DS_OFF)
        return;
    if (why)
        dual_msg(why);
    ds.state = DS_OFF;
    dual_free_states();
    if (ds.linked)
    {
        hk.unlink();
        ds.linked = false;
    }
}

// No dual link for the rest of this session (the games go on alone).
static void dual_give_up(const char* why)
{
    if (ds.is_host)
        dual_send_peer(DPK_GIVEUP, NULL, 0, NULL, 0);
    else
        dual_send(0, DPK_BYE, NULL, 0, NULL, 0);
    dual_stop(why);
}

static void dual_count_stall(u32 f)
{
    if (ds.stall_frame != f + 1)
    {
        ds.stall_frame = f + 1;
        ds.stalls++;
    }
}

static void dual_send_hello(unsigned to)
{
    u32 w[4];
    w[0] = DUAL_VERSION;
    w[1] = ds.rom_crc;
    w[2] = ds.cfg;
    w[3] = ds.is_host ? 1 : 0;
    dual_send(to, DPK_HELLO, w, 4, NULL, 0);
    ds.hello_sent = true;
    ds.t_hello = now_us();
}

static bool dual_hello_matches(const u32* w)
{
    return w[0] == DUAL_VERSION && w[1] == ds.rom_crc && w[2] == ds.cfg &&
           w[3] == (ds.is_host ? 0u : 1u);
}

static void dual_send_ping(void)
{
    const u64 now = now_us();
    u32 w[2] = { (u32)now, (u32)(now >> 32) };
    dual_send_peer(DPK_PING, w, 2, NULL, 0);
    ds.ping_out = true;
    ds.t_ping = now;
}

static u32 dual_rtt_min(void)
{
    u32 i, m = ~0u;
    for (i = 0; i < MIN(ds.rtt_n, (u32)DUAL_PINGS); i++)
        m = MIN(m, ds.rtt[i]);
    return ds.rtt_n ? m : 0;
}

// The buttons must get to the other console before it needs them. The time
// sync keeps the consoles within 5/8 of a frame of each other; on top of that
// the network takes one way, and a frame more covers it having a bad moment.
static u32 dual_pick_delay(void)
{
    const u32 sync_us = ds.frame_us * 5 / 8;
    const u32 d = (ds.ow_us + sync_us + ds.frame_us + ds.frame_us - 1) / ds.frame_us;
    return MAX(2u, MIN(d, 30u));
}

static void dual_session_reset(void)
{
    const u64 now = now_us();
    unsigned s;
    dual_free_states();
    ds.n = 0;
    ds.hello_sent = ds.go = ds.peer_ready = false;
    ds.peer_pack_len = ds.peer_rcvd = 0;
    ds.rtt_n = 0;
    ds.ping_out = false;
    ds.ow_us = 0;
    memset(ds.keys, 0, sizeof(ds.keys));
    memset(ds.frame_ok, 0, sizeof(ds.frame_ok));
    memset(ds.my_hash, 0, sizeof(ds.my_hash));
    memset(ds.peer_hash, 0, sizeof(ds.peer_hash));
    ds.hash_done = ds.hash_bad = 0;
    ds.peer_pos_n = 0;
    ds.peer_lead = 0;
    for (s = 0; s < NP; s++)
    {
        ds.slot_id[s] = -1;
        ds.t_heard[s] = now;
    }
    ds.t_state = now;
}

static void dual_start_running(void)
{
    u32 i;
    char msg[96];
    ds.state = DS_RUN;
    ds.frame = 0;
    // Nobody presses anything during the first frames (the delay).
    for (i = 0; i < ds.delay; i++)
    {
        ds.own_keys[i] = 0;
        ds.keys[i] = 0;
        ds.frame_ok[i] = i + 1;
    }
    ds.sent_upto = ds.delay;
    ds.send_delay = ds.delay;
    ds.stalls = ds.stall_frame = ds.peer_stalls = 0;
    ds.t_peer_stall = now_us();
    ds.t_lowered = 0;
    ds.calm_us = DUAL_DELAY_CALM_US;
    ds.sync_next = DUAL_SYNC_GAP;
    ds.reset_wanted = false;
    ds.t_heard[0] = ds.t_heard[1] = now_us();
    snprintf(msg, sizeof(msg), "NGP link: both Neo Geo Pockets connected (%u frames of delay)",
             (unsigned)ds.delay);
    dual_msg(msg);
}

// Freeze our Neo Geo Pocket between two frames (the game restarts first) and make
// the copy sent to the other console.
static void dual_begin_transfer(void)
{
    u32 off;
    uint8_t* blob = NULL;
    size_t blob_size = 0;
    if (!hk.capture(&blob, &blob_size))
    {
        free(blob);
        dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
        dual_stop("NGP link: could not take this Neo Geo Pocket's state");
        return;
    }
    ds.own.assign(blob, blob + blob_size);
    free(blob);
    rle_pack(ds.own_pack, ds.own);
    ds.state = DS_TRANSFER;
    for (off = 0; off < ds.own_pack.size(); off += DUAL_CHUNK)
    {
        u32 w[2] = { off, (u32)ds.own_pack.size() };
        const u32 len = MIN((u32)DUAL_CHUNK, (u32)ds.own_pack.size() - off);
        dual_send_peer(DPK_STATE, w, 2, &ds.own_pack[off], len);
    }
    std::vector<u8>().swap(ds.own_pack);
}

// Both Neo Geo Pockets start from the states that were exchanged: ours from the
// copy we sent (so that both copies of it start the same way), the other
// one from what came.
static bool dual_load_machines(void)
{
    std::vector<u8> peer;
    if (!rle_unpack(peer, &ds.peer_pack[0], ds.peer_pack_len))
        return false;
    if (!hk.link(ds.my_slot, &ds.own[0], ds.own.size(), &peer[0], peer.size()))
        return false;
    ds.linked = true;
    dual_free_states();
    return true;
}

// Setup steps, between frames.
static void dual_setup_step(void)
{
    const u64 now = now_us();

    if (ds.state == DS_HELLO)
    {
        // The host may not be listening yet: say it again until it answers.
        if (!ds.hello_sent || now - ds.t_hello > DUAL_HELLO_EVERY)
            dual_send_hello(0);
        if (now - ds.t_state > DUAL_HELLO_US)
            dual_give_up("NGP link: the host does not have the dual link");
        return;
    }

    if (ds.state == DS_WAIT)
        return;     // Host: waiting for a client (the game goes on)

    if (ds.state == DS_PING)
    {
        if (ds.is_host)
        {
            if (ds.rtt_n < DUAL_PINGS && (!ds.ping_out || now - ds.t_ping > DUAL_PONG_US))
                dual_send_ping();
            if (ds.rtt_n >= DUAL_PINGS || now - ds.t_state > DUAL_PING_US)
            {
                ds.ow_us = dual_rtt_min() / 2;
                ds.delay = dual_pick_delay();
                dual_send_peer(DPK_GO, NULL, 0, NULL, 0);
                dual_begin_transfer();
            }
        }
        else if (ds.go)
            dual_begin_transfer();
    }

    if (ds.state == DS_TRANSFER && ds.peer_pack_len && ds.peer_rcvd >= ds.peer_pack_len)
    {
        if (!dual_load_machines())
        {
            dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
            dual_stop("NGP link: could not start the other Neo Geo Pocket");
            return;
        }
        if (ds.is_host)
        {
            u32 w[1] = { ds.delay };
            dual_send_peer(DPK_READY, w, 1, NULL, 0);
        }
        ds.state = DS_READY;
    }

    if (ds.state == DS_READY && (ds.is_host || ds.peer_ready))
        dual_start_running();

    if (ds.state >= DS_PING && ds.state < DS_RUN && now - ds.t_state > DUAL_SETUP_US)
    {
        dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
        dual_stop("NGP link: the other console did not answer");
    }
}

// Host: the client said HELLO and it matches: it takes slot 1.
static void dual_host_start(unsigned client_id)
{
    u32 w[NP];
    ds.n = NP;
    ds.my_slot = 0;
    ds.slot_id[0] = 0;
    ds.slot_id[1] = (int)client_id;
    w[0] = 0;
    w[1] = client_id;
    dual_send(client_id, DPK_START, w, NP, NULL, 0);
    ds.state = DS_PING;
    ds.t_state = now_us();
    ds.t_heard[1] = ds.t_state;
    dual_send_ping();
}

/* ---- frontend interface (libretro.cpp) --------------------------------- */

void dual_set_hooks(const struct dual_hooks* hooks)
{
    hk = *hooks;
}

void dual_game_loaded(uint32_t rom_crc, uint32_t cfg, double fps)
{
    ds.rom_crc = rom_crc;
    ds.cfg = cfg;
    ds.frame_us = fps > 1.0 ? (u32)(1000000.0 / fps + 0.5) : 16688;
}

void dual_set_cfg(uint32_t cfg)
{
    ds.cfg = cfg;
}

void dual_net_start(uint16_t client_id, retro_netpacket_send_t send,
                    retro_netpacket_poll_receive_t poll_receive)
{
    dual_stop(NULL);
    ds.send = send;
    ds.poll_receive = poll_receive;
    dual_session_reset();
    ds.is_host = (client_id == 0);
    ds.my_id = client_id;
    if (ds.is_host)
        ds.state = DS_WAIT;
    else
    {
        ds.state = DS_HELLO;
        dual_send_hello(0);
    }
    ds.t_state = now_us();
}

void dual_net_stop(void)
{
    dual_stop(NULL);
    ds.send = NULL;
    ds.poll_receive = NULL;
}

// Host: a client joined. The cable has two ends: only one client plays.
bool dual_net_connected(uint16_t client_id)
{
    // (Its HELLO can come before the frontend tells us about it.)
    if (!ds.is_host)
        return true;
    return ds.slot_id[1] < 0 || ds.slot_id[1] == (int)client_id;
}

void dual_net_disconnected(uint16_t client_id)
{
    if (!ds.is_host || ds.state == DS_OFF || ds.slot_id[1] != (int)client_id)
        return;
    dual_stop(ds.state == DS_RUN ? "NGP link: the other console left" : NULL);
}

bool dual_setting_up(void)
{
    const u64 t0 = now_us();
    if (ds.state == DS_OFF || ds.state == DS_RUN || ds.state == DS_WAIT)
        return false;
    // A client still saying HELLO plays on meanwhile: the host may not have
    // this core at all.
    if (ds.state == DS_HELLO)
    {
        dual_setup_step();
        return false;
    }
    // The game is stopped anyway: rather than one step per frame, go on for
    // most of the frame, as soon as the other console answers.
    while (true)
    {
        dual_setup_step();
        if (ds.state == DS_OFF || ds.state == DS_RUN || now_us() - t0 > DUAL_SLICE_US)
            break;
        poll_receive();
        usleep(250);
    }
    return ds.state != DS_OFF && ds.state != DS_RUN;
}

bool dual_running(void)
{
    return ds.state == DS_RUN;
}

bool dual_active(void)
{
    return ds.state != DS_OFF && ds.state != DS_HELLO && ds.state != DS_WAIT;
}

void dual_request_reset(void)
{
    if (ds.state == DS_RUN)
        ds.reset_wanted = true;
}

// How far ahead of the peer we are (us, at `now`): where we are (the frame
// about to start) against where it is (the frame it was starting when it
// sent its last buttons, plus the time since).
static s32 dual_lead(u64 now)
{
    s64 best;
    u32 i;
    if (!ds.peer_pos_n)
        return 0;
    best = ds.peer_pos[0];
    for (i = 1; i < MIN(ds.peer_pos_n, (u32)DUAL_SYNC_WIN); i++)
        best = MAX(best, ds.peer_pos[i]);
    return (s32)((s64)ds.frame * ds.frame_us - (s64)now - best);
}

int dual_frame_begin(uint32_t local_keys)
{
    const u32 f = ds.frame;
    const u32 fi = f % DUAL_RING;
    const u64 t0 = now_us();
    const unsigned peer = ds.my_slot ^ 1;
    s32 lead;

    // Ahead of the other console (both work it out the same way, so only one
    // of the two can be): stay a frame where we are. Never so often that the
    // other one cannot see it before the next time.
    lead = dual_lead(t0);
    if (f >= ds.sync_next && ds.peer_pos_n >= DUAL_SYNC_WIN &&
        (lead - ds.peer_lead) / 2 > (s32)(ds.frame_us * 5 / 8))
    {
        ds.sync_next = f + DUAL_SYNC_GAP;
        return DUAL_FRAME_HOLD;
    }

    // The network had a bad moment and the peer waited for our buttons: they
    // go out earlier (each console sends its own buttons for whatever frame
    // it likes, so this needs no agreement). Back down once all is calm.
    if (ds.send_delay > ds.delay && t0 - ds.t_peer_stall > ds.calm_us)
    {
        ds.send_delay--;
        ds.t_peer_stall = ds.t_lowered = t0;
    }

    // Ours for frame f + delay.
    while (ds.sent_upto <= f + ds.send_delay)
    {
        u32 w[5];
        w[0] = ds.sent_upto;
        w[1] = local_keys | (ds.reset_wanted ? DUAL_KEY_RESET : 0);
        ds.reset_wanted = false;
        w[2] = f;
        w[3] = (u32)lead;
        w[4] = ds.stalls;
        ds.own_keys[ds.sent_upto % DUAL_RING] = w[1];
        dual_send_peer(DPK_INPUT, w, 5, NULL, 0);
        ds.sent_upto++;
    }

    // The peer's for frame f.
    while (ds.frame_ok[fi] != f + 1)
    {
        const u64 now = now_us();
        if (ds.state != DS_RUN)
            return DUAL_FRAME_WAIT;
        if (now - ds.t_heard[peer] > DUAL_PEER_US)
        {
            dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
            dual_stop("NGP link: the other console stopped answering");
            return DUAL_FRAME_WAIT;
        }
        if (now - t0 > DUAL_STALL_US)
        {
            dual_count_stall(f);
            return DUAL_FRAME_WAIT;
        }
        poll_receive();
        if (ds.frame_ok[fi] != f + 1)
            usleep(250);
    }

    // A player pressed reset: both consoles restart both games in this very
    // frame, with the cable plugged again.
    if ((ds.own_keys[fi] | ds.keys[fi]) & DUAL_KEY_RESET)
        hk.reset_all();

    hk.set_keys(ds.my_slot, ds.own_keys[fi] & ~DUAL_KEY_RESET);
    hk.set_keys(peer, ds.keys[fi] & ~DUAL_KEY_RESET);
    // Test of the agreement check: the copy of the other Neo Geo Pocket gets other
    // buttons for a while.
    if (getenv("GEARSYSTEM_DUAL_FAULT") && ds.my_slot == 0 && f >= 300 && f < 330)
        hk.set_keys(peer, (ds.keys[fi] & ~DUAL_KEY_RESET) ^ DUAL_KEY_UP);
    return DUAL_FRAME_RUN;
}

// Both copies of each Neo Geo Pocket must be the same: compare once both told.
// The link is cut when two checks in a row differ.
static void dual_check_hash(u32 f)
{
    const u32 i = (f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS;
    const u32* mine = ds.my_hash[i];
    const u32* theirs = ds.peer_hash[i];
    if (mine[0] != f + 1 || theirs[0] != f + 1 || ds.hash_done >= f + 1)
        return;
    ds.hash_done = f + 1;
    if (getenv("GEARSYSTEM_DUAL_DEBUG"))
        fprintf(stderr, "[dual] slot %u frame %u: ours %08x %08x, theirs %08x %08x\n",
                ds.my_slot, (unsigned)f, mine[1], mine[2], theirs[1], theirs[2]);
    if (mine[1] == theirs[1] && mine[2] == theirs[2])
        return;
    if (f >= DUAL_HASH_EVERY && ds.hash_bad == f + 1 - DUAL_HASH_EVERY)
    {
        dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
        dual_stop("NGP link: the consoles went out of sync");
        return;
    }
    ds.hash_bad = f + 1;
}

void dual_frame_end(void)
{
    const u32 f = ds.frame++;
    if (f % DUAL_HASH_EVERY == 0)
    {
        u32 w[1 + NP];
        u32* mine = ds.my_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
        unsigned s;
        w[0] = f;
        mine[0] = f + 1;
        for (s = 0; s < NP; s++)
            mine[1 + s] = w[1 + s] = hk.hash(s);
        dual_send_peer(DPK_HASH, w, 1 + NP, NULL, 0);
        dual_check_hash(f);
    }
}

void dual_net_receive(const void* buf, size_t len, uint16_t client_id)
{
    const u64 now = now_us();
    u32 type, from;

    if (len < 8 || word(buf, 0) != DUAL_HEADER || ds.state == DS_OFF)
        return;

    type = word(buf, 1) & 0xFF;
    from = (word(buf, 1) >> 8) & 0xFF;
    // (START is what tells the client which slot it has.)
    if (type == DPK_START)
        from = DUAL_NO_SLOT;
    // Packets with a slot: only from the console of this session.
    if (from != DUAL_NO_SLOT)
    {
        if (!ds.n || from >= ds.n || from == ds.my_slot || ds.slot_id[from] != (int)client_id)
            return;
        ds.t_heard[from] = now;
    }

    switch (type)
    {
    case DPK_HELLO:
        if (len < 8 + 4 * 4)
            break;
        {
            u32 w[4];
            unsigned i;
            for (i = 0; i < 4; i++)
                w[i] = word(buf, 2 + i);
            if (ds.is_host && ds.state == DS_WAIT)
            {
                if (!dual_hello_matches(w))
                {
                    // That console plays alone, and so do we.
                    dual_send(client_id, DPK_GIVEUP, NULL, 0, NULL, 0);
                    dual_stop(DUAL_MSG_DIFFER);
                    break;
                }
                dual_send_hello(client_id);
                dual_host_start(client_id);
            }
            else if (!ds.is_host && ds.state == DS_HELLO && client_id == 0)
            {
                if (!dual_hello_matches(w))
                    dual_give_up(DUAL_MSG_DIFFER);
                else
                    ds.state = DS_WAIT;
            }
        }
        break;

    case DPK_START:
        if (len >= 8 + NP * 4 && !ds.is_host && client_id == 0 &&
            (ds.state == DS_WAIT || ds.state == DS_HELLO))
        {
            ds.slot_id[0] = (int)word(buf, 2);
            ds.slot_id[1] = (int)word(buf, 3);
            if (ds.slot_id[1] != (int)ds.my_id)
            {
                dual_give_up(NULL);
                break;
            }
            ds.n = NP;
            ds.my_slot = 1;
            ds.t_heard[0] = ds.t_heard[1] = now;
            ds.state = DS_PING;
            ds.t_state = now;
        }
        break;

    case DPK_PING:
        if (len >= 16)
        {
            u32 w[2] = { word(buf, 2), word(buf, 3) };
            dual_send(client_id, DPK_PONG, w, 2, NULL, 0);
        }
        break;

    case DPK_PONG:
        if (len >= 16 && ds.is_host && from == 1)
        {
            const u64 t = word(buf, 2) | ((u64)word(buf, 3) << 32);
            ds.rtt[ds.rtt_n % DUAL_PINGS] = (u32)(now - t);
            ds.rtt_n++;
            ds.ping_out = false;
        }
        break;

    case DPK_GO:
        if (!ds.is_host && from == 0)
            ds.go = true;
        break;

    case DPK_STATE:
        if (len > 16 && from != DUAL_NO_SLOT && ds.state >= DS_PING && ds.state <= DS_TRANSFER)
        {
            const u32 off = word(buf, 2);
            const u32 total = word(buf, 3);
            const u32 dlen = (u32)len - 16;
            // (Checked so that no sum can wrap around.)
            if (total <= DUAL_STATE_MAX + DUAL_STATE_MAX / 64 && off <= total && dlen <= total - off)
            {
                if (ds.peer_pack.size() != total)
                    ds.peer_pack.resize(total);
                memcpy(&ds.peer_pack[off], (const u8*)buf + 16, dlen);
                ds.peer_pack_len = total;
                ds.peer_rcvd += dlen;
            }
        }
        break;

    case DPK_READY:
        if (len >= 12 && !ds.is_host && from == 0)
        {
            ds.delay = MAX(1u, MIN(word(buf, 2), 30u));
            ds.peer_ready = true;
        }
        break;

    case DPK_INPUT:
        if (len >= 8 + 5 * 4 && from != DUAL_NO_SLOT && ds.state >= DS_TRANSFER)
        {
            const u32 f = word(buf, 2);
            const u32 their_stalls = word(buf, 6);
            ds.keys[f % DUAL_RING] = word(buf, 3);
            ds.frame_ok[f % DUAL_RING] = f + 1;
            // It was starting this frame one way ago.
            ds.peer_pos[ds.peer_pos_n++ % DUAL_SYNC_WIN] =
                (s64)word(buf, 4) * ds.frame_us + ds.ow_us - (s64)now;
            ds.peer_lead = (s32)word(buf, 5);
            // It waited for our buttons: they go out a frame earlier.
            if (their_stalls != ds.peer_stalls && ds.state == DS_RUN)
            {
                ds.peer_stalls = their_stalls;
                ds.send_delay = MIN(ds.send_delay + 1, MIN(ds.delay + DUAL_DELAY_MORE, (u32)DUAL_RING / 2));
                // Taking that frame off was too soon: next time, wait longer.
                if (ds.t_lowered && now - ds.t_lowered < ds.calm_us)
                    ds.calm_us = MIN(ds.calm_us * 2, (u64)DUAL_DELAY_CALM_MAX);
                ds.t_peer_stall = now;
            }
        }
        break;

    case DPK_HASH:
        if (len >= 8 + (1 + NP) * 4 && from != DUAL_NO_SLOT && ds.state == DS_RUN)
        {
            const u32 f = word(buf, 2);
            u32* theirs = ds.peer_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
            theirs[0] = f + 1;
            theirs[1] = word(buf, 3);
            theirs[2] = word(buf, 4);
            dual_check_hash(f);
        }
        break;

    case DPK_GIVEUP:
        if (!ds.is_host && client_id == 0)
            dual_stop(DUAL_MSG_DIFFER);
        break;

    case DPK_BYE:
        if (ds.is_host && ds.state == DS_WAIT)
            break;      // A client that cannot take part: still waiting
        if (from != DUAL_NO_SLOT || client_id == 0)
            dual_stop(ds.state == DS_RUN ? "NGP link: the other console left" : NULL);
        break;
    }
}

void dual_shutdown(void)
{
    dual_stop(NULL);
    ds.send = NULL;
    ds.poll_receive = NULL;
}
