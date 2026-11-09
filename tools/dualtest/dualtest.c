/*
 * NGP dual link test: two consoles in one process, as RetroArch would run
 * them, joined by a network in memory (from Gearsystem's Gear-to-Gear test).
 *
 *   dualtest <core.so> <rom.ngc> [frames] [one way ms] [join frame]
 *
 * Each console loads its own copy of the core (dlopen of a copy of the .so),
 * on its own thread, at 60.25 frames a second. Console 0 is the netplay
 * host, console 1 the client; the session starts at the join frame.
 *
 * Environment: KEYS0 / KEYS1 = "from-to:key+key;..." (up down left right a
 * b option) script the buttons (by default both press up and down now and
 * then); SHOTS=dir SHOT_EVERY=n save screenshots (PPM); DUAL_OFF=1 turns
 * the link off; ROM2=file gives the client another game.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "libretro.h"

#define CONSOLES 2

typedef struct
{
    void* h;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    bool (*load_game)(const struct retro_game_info*);
    void (*unload_game)(void);
    void (*run)(void);
    void (*reset)(void);
    void* (*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);

    struct retro_netpacket_callback np;
    bool has_np;
    bool started;
    unsigned frame;
    unsigned shown, dupes;
    size_t samples;
    char last_msg[256];
} console_t;

static console_t con[CONSOLES];
static __thread int cur = -1;
static unsigned frames = 1800, one_way_ms = 5, join_frame = 120;
static const char* rom_path;
static volatile bool client_started;

/* ---- network in memory ----------------------------------------------------- */

typedef struct pkt
{
    struct pkt* next;
    int to;                 // console
    uint16_t from_id;       // libretro client id of the sender
    uint64_t due;
    size_t len;
    uint8_t data[];
} pkt;

static pthread_mutex_t net_mtx = PTHREAD_MUTEX_INITIALIZER;
static pkt* net_head;
static pkt* net_tail;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static void RETRO_CALLCONV net_send(int flags, const void* buf, size_t len, uint16_t client_id)
{
    (void)flags;
    // Host (console 0, id 0) talks to client id 1 (console 1); the client to
    // the host.
    int to = cur == 0 ? (client_id == 0xFFFF ? 1 : (int)client_id) : 0;
    pkt* p = (pkt*)malloc(sizeof(pkt) + len);
    p->next = NULL;
    p->to = to;
    p->from_id = (uint16_t)cur;
    p->due = now_us() + one_way_ms * 1000ULL;
    p->len = len;
    memcpy(p->data, buf, len);
    pthread_mutex_lock(&net_mtx);
    if (net_tail)
        net_tail->next = p;
    else
        net_head = p;
    net_tail = p;
    pthread_mutex_unlock(&net_mtx);
}

static void RETRO_CALLCONV net_poll(void)
{
    const uint64_t now = now_us();
    while (true)
    {
        pkt *p, *prev = NULL;
        pthread_mutex_lock(&net_mtx);
        for (p = net_head; p; prev = p, p = p->next)
            if (p->to == cur)
                break;
        if (!p || p->due > now)
        {
            pthread_mutex_unlock(&net_mtx);
            return;
        }
        if (prev)
            prev->next = p->next;
        else
            net_head = p->next;
        if (net_tail == p)
            net_tail = prev;
        pthread_mutex_unlock(&net_mtx);
        if (con[cur].started)
            con[cur].np.receive(p->data, p->len, p->from_id);
        free(p);
    }
}

/* ---- frontend ------------------------------------------------------------------ */

static void log_printf(enum retro_log_level level, const char* fmt, ...)
{
    va_list va;
    if (level < RETRO_LOG_INFO)
        return;
    fprintf(stderr, "[c%d] ", cur);
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static bool environ_cb(unsigned cmd, void* data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char**)data = "/nonexistent";
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable* var = (struct retro_variable*)data;
        var->value = NULL;
        if (!strcmp(var->key, "race_dual_link"))
            var->value = getenv("DUAL_OFF") ? "disabled" : "enabled";
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
        con[cur].np = *(const struct retro_netpacket_callback*)data;
        con[cur].has_np = true;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        const struct retro_message* msg = (const struct retro_message*)data;
        fprintf(stderr, "[c%d] frame %u MESSAGE: %s\n", cur, con[cur].frame, msg->msg);
        snprintf(con[cur].last_msg, sizeof(con[cur].last_msg), "%s", msg->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE)
            *(bool*)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch)
{
    const char* dir = getenv("SHOTS");
    unsigned every = getenv("SHOT_EVERY") ? (unsigned)atoi(getenv("SHOT_EVERY")) : 60;
    if (data)
        con[cur].shown++;
    else
        con[cur].dupes++;
    if (data && dir && every && con[cur].frame % every == 0)
    {
        char path[512];
        FILE* f;
        unsigned x, y;
        snprintf(path, sizeof(path), "%s/c%d_%05u.ppm", dir, cur, con[cur].frame);
        if ((f = fopen(path, "wb")))
        {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                {
                    uint16_t c = ((const uint16_t*)((const uint8_t*)data + y * pitch))[x];
                    unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t audio_batch_cb(const int16_t* data, size_t n)
{
    (void)data;
    con[cur].samples += n;
    return n;
}

static void input_poll_cb(void) {}

/* KEYS0..KEYS1: "from-to:key+key;..." */
static int scripted_keys(int who, unsigned frame, int16_t* bits)
{
    static const struct { const char* n; int id; } names[] = {
        { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
        { "option", RETRO_DEVICE_ID_JOYPAD_START },
        { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    };
    char var[8];
    const char* s;
    snprintf(var, sizeof(var), "KEYS%d", who);
    if (!(s = getenv(var)))
        return 0;
    *bits = 0;
    while (*s)
    {
        char* e;
        const unsigned from = (unsigned)strtoul(s, &e, 10);
        unsigned to = from, k;
        if (*e == '-') to = (unsigned)strtoul(e + 1, &e, 10);
        if (*e == ':') e++;
        while (*e && *e != ';')
        {
            size_t n = strcspn(e, "+;");
            for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
                if (strlen(names[k].n) == n && !strncmp(e, names[k].n, n) && frame >= from && frame <= to)
                    *bits |= (int16_t)(1 << names[k].id);
            e += n;
            if (*e == '+') e++;
        }
        s = *e ? e + 1 : e;
    }
    return 1;
}

// By default console 0 holds up every other half second, console 1 down
// every other 0.75 seconds.
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    unsigned f = con[cur].frame;
    int16_t bits = 0;
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (scripted_keys(cur, f, &bits))
        ;
    else if (cur == 0 && (f / 30) % 2 == 0)
        bits |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
    else if (cur == 1 && (f / 45) % 2 == 0)
        bits |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return bits;
    return (bits >> id) & 1;
}

#define SYM(c, name) do { *(void**)&(c)->name = dlsym((c)->h, "retro_" #name); \
    if (!(c)->name) { fprintf(stderr, "missing retro_%s\n", #name); exit(1); } } while (0)

static void load_core(console_t* c, const char* so, int i)
{
    char path[512], cmd[1200];
    snprintf(path, sizeof(path), "/tmp/dualtest_core%d_%d.so", i, (int)getpid());
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", so, path);
    if (system(cmd))
        exit(1);
    c->h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    unlink(path);
    if (!c->h)
    {
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    SYM(c, set_environment); SYM(c, set_video_refresh); SYM(c, set_audio_sample);
    SYM(c, set_audio_sample_batch); SYM(c, set_input_poll); SYM(c, set_input_state);
    SYM(c, init); SYM(c, deinit); SYM(c, load_game); SYM(c, unload_game); SYM(c, run); SYM(c, reset);
    SYM(c, get_memory_data); SYM(c, get_memory_size);
}

static void* console_thread(void* arg)
{
    const int i = (int)(intptr_t)arg;
    console_t* c = &con[i];
    struct retro_game_info info = { rom_path, NULL, 0, NULL };

    const unsigned reset_at = getenv("RESET_AT") ? (unsigned)atoi(getenv("RESET_AT")) : ~0u;
    const unsigned stop_at = getenv("STOP_AT") ? (unsigned)atoi(getenv("STOP_AT")) : ~0u;
    uint64_t t0;


    cur = i;
    // ROM2: the client loads another game.
    if (i == 1 && getenv("ROM2"))
        info.path = getenv("ROM2");
    c->set_environment(environ_cb);
    c->set_video_refresh(video_cb);
    c->set_audio_sample(audio_cb);
    c->set_audio_sample_batch(audio_batch_cb);
    c->set_input_poll(input_poll_cb);
    c->set_input_state(input_state_cb);
    c->init();
    if (!c->load_game(&info))
    {
        fprintf(stderr, "[c%d] load failed\n", i);
        exit(1);
    }

    t0 = now_us();
    for (c->frame = 0; c->frame < frames; c->frame++)
    {
        const uint64_t due = t0 + (uint64_t)c->frame * 16598;
        const uint64_t now = now_us();
        if (due > now)
            usleep((useconds_t)(due - now));

        if (c->frame == join_frame && c->has_np)
        {
            if (i == 0)
            {
                c->started = true;
                c->np.start(0, net_send, net_poll);
            }
            else
            {
                c->started = true;
                c->np.start(1, net_send, net_poll);
                client_started = true;
            }
        }
        // The host hears about the client (a little late, as with RetroArch).
        if (i == 0 && client_started && c->started && c->np.connected)
        {
            static bool told;
            if (!told)
            {
                told = true;
                if (!c->np.connected(1))
                    fprintf(stderr, "[c0] the host refused the client\n");
            }
        }
        if (c->frame == stop_at && c->started)
        {
            if (c->np.stop)
                c->np.stop();
            c->started = false;
        }
        if (c->started)
            net_poll();
        if (i == 0 && c->frame == reset_at)
            c->reset();
        c->run();
    }
    return NULL;
}

int main(int argc, char** argv)
{
    pthread_t th[CONSOLES];
    int i, fail = 0;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s core.so rom.ngc [frames] [one way ms] [join frame]\n", argv[0]);
        return 2;
    }
    rom_path = argv[2];
    if (argc > 3) frames = (unsigned)atoi(argv[3]);
    if (argc > 4) one_way_ms = (unsigned)atoi(argv[4]);
    if (argc > 5) join_frame = (unsigned)atoi(argv[5]);

    for (i = 0; i < CONSOLES; i++)
        load_core(&con[i], argv[1], i);
    for (i = 0; i < CONSOLES; i++)
        pthread_create(&th[i], NULL, console_thread, (void*)(intptr_t)i);
    for (i = 0; i < CONSOLES; i++)
        pthread_join(th[i], NULL);

    for (i = 0; i < CONSOLES; i++)
    {
        console_t* c = &con[i];
        printf("console %d: shown %u, repeated %u, audio frames %zu\n", i, c->shown, c->dupes, c->samples);
        printf("  last message: %s\n", c->last_msg);
        if (strstr(c->last_msg, "out of sync") || strstr(c->last_msg, "differ") ||
            strstr(c->last_msg, "did not") || strstr(c->last_msg, "stopped") ||
            strstr(c->last_msg, "could not"))
        {
            printf("  FAIL: %s\n", c->last_msg);
            fail = 1;
        }
        if (!getenv("DUAL_OFF") && !strstr(c->last_msg, "connected"))
        {
            printf("  FAIL: never linked\n");
            fail = 1;
        }
    }
    printf(fail ? "FAIL\n" : "OK\n");
    return fail;
}
