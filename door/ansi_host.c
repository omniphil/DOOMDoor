/*
 * ansi_host.c -- the Doom module, run here on the BBS instead of in TERMinator's sandbox.
 *
 * A caller without TRACE can't run the game themselves, so the door runs it for them and sends ANSI pictures of it
 * (ansi_play.c). The game is the very same module that goes to TERMinator as doom.wasm, only compiled for this
 * machine: it only ever talks to the "trace_*" functions of trace_api.h, and this file answers them the way
 * TERMinator would.
 *
 *   present         keeps the newest frame for the door to turn into ANSI
 *   time            a monotonic clock
 *   assets          the WAD the door has already loaded
 *   send            savegame requests and pieces, handled by the door's own savegame code, as if they had come over TRACE
 *   sound           reported as having no room, so the game doesn't mix any: a text terminal can't play it
 *   quit            ends the game thread; the door notices and shows its closing screen
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "trace_api.h"

#include "ansi_host.h"
#include "saves.h"
#include "trace_doom.h"

/* The module's in-memory files (module/src/ramfs.c) */
void tracedoom_ramfs_put(const char *name, const void *data, size_t size, int partial);

/* What Crispy's own main() would have set up (ansi_compat.c) */
void ansi_game_prepare(void);

static const unsigned char *g_wad;
static size_t               g_wad_size;
static char                 g_wad_hash[65];

static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t       *g_frame;
static int             g_frame_w, g_frame_h;
static size_t          g_frame_cap;
static unsigned        g_frame_seq;

static atomic_int      g_finished;
static struct timespec g_start;
static FILE           *g_log;

/*
 * Doom's settings for a text screen. Doom reads these as its config files (in memory, as in the sandbox):
 *   screenblocks 11    the whole picture is the view: the status bar is drawn as text by the door instead
 *   show_messages 0    messages go to the door's text line too; drawn in the picture they'd be unreadable
 *   joyb_speed 29      always run, since a terminal can't report Shift being held
 *   crispy_hires 0     320x200 is already more than a terminal can show, and it costs the BBS a quarter of the work
 *   crispy_uncapped 0  35 frames a second, Doom's own rate, rather than as many as the CPU can draw
 */
static const char DEFAULT_CFG[] =
    "screenblocks 11\n"
    "show_messages 0\n"
    "joyb_speed 29\n";

/* The JPEG XL graphics mode shows Doom's own picture at its own size, so its status bar and messages stay in it.
 * Always run as in ANSI mode: not every terminal reports Shift being held (and where one does, Shift walks). */
static const char PIXEL_CFG[] =
    "screenblocks 10\n"
    "show_messages 1\n"
    "joyb_speed 29\n";

static bool g_pixel_mode;

static const char CRISPY_CFG[] =
    "crispy_hires 0\n"
    "crispy_uncapped 0\n";

/* ---- what the module calls ---- */

void trace_present(const uint32_t *pixels, int32_t width, int32_t height, int32_t flags)
{
    size_t need = (size_t)width * (size_t)height;
    (void)flags;

    if (width <= 0 || height <= 0)
        return;
    pthread_mutex_lock(&g_frame_lock);
    if (need > g_frame_cap)
    {
        uint32_t *grown = realloc(g_frame, need * sizeof(uint32_t));
        if (grown == NULL)
        {
            pthread_mutex_unlock(&g_frame_lock);
            return;
        }
        g_frame = grown;
        g_frame_cap = need;
    }
    memcpy(g_frame, pixels, need * sizeof(uint32_t));
    g_frame_w = width;
    g_frame_h = height;
    g_frame_seq++;
    pthread_mutex_unlock(&g_frame_lock);
}

int32_t trace_input_pending(void) { return 0; }
int32_t trace_cpu_count(void) { return 1; }

void trace_frame_capacity(int32_t *width, int32_t *height)
{
    *width = 1280;
    *height = 800;
}

/* The game's own messages. A door's output goes to the caller, so these only go to a file, and only when the sysop
 * asks for one with DOOMDOOR_LOG=<file>. */
void trace_log(const char *text, int32_t length)
{
    if (g_log != NULL)
    {
        fprintf(g_log, "%.*s\n", (int)length, text);
        fflush(g_log);
    }
}

void trace_quit(int32_t code)
{
    trace_log("doom: quit", 10);
    (void)code;
    atomic_store(&g_finished, 1);
    pthread_exit(NULL);     /* only the game thread ends; the door carries on to its closing screen */
}

int32_t trace_time_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int32_t)((now.tv_sec - g_start.tv_sec) * 1000L + (now.tv_nsec - g_start.tv_nsec) / 1000000L);
}

void trace_set_tick(int32_t hz) { (void)hz; }

/* What the module sends up goes to the door's savegame code; its answers come back through native_reply */
static void native_reply(const char *head, const void *payload, size_t len);

int32_t trace_send(const void *data, int32_t length)
{
    if (length <= 0)
        return 0;
    trace_doom_module_message((const unsigned char *)data, (size_t)length, native_reply);
    return length;
}

int32_t trace_send_room(void) { return 1 << 20; }

int32_t trace_audio_write(const int16_t *frames, int32_t frame_count) { (void)frames; return frame_count; }
int32_t trace_audio_room(void) { return 0; }

int32_t trace_asset_size(const char *sha256)
{
    return strcmp(sha256, g_wad_hash) == 0 ? (int32_t)g_wad_size : 0;
}

int32_t trace_asset_read(const char *sha256, int32_t offset, void *buffer, int32_t length)
{
    if (strcmp(sha256, g_wad_hash) != 0 || offset < 0 || length <= 0 || (size_t)offset >= g_wad_size)
        return 0;
    if ((size_t)offset + (size_t)length > g_wad_size)
        length = (int32_t)(g_wad_size - (size_t)offset);
    memcpy(buffer, g_wad + offset, (size_t)length);
    return length;
}

int32_t trace_store_read(void *buffer, int32_t length) { (void)buffer; (void)length; return 0; }
int32_t trace_store_write(const void *data, int32_t length) { (void)data; (void)length; return 0; }

/* ---- the door's side ---- */

/* A message for the module: its text line, then the payload after a newline, exactly as TRACE would deliver it. */
static void native_reply(const char *head, const void *payload, size_t len)
{
    size_t head_len = strlen(head);
    size_t total = head_len + (payload != NULL ? 1 + len : 0);
    char *message = malloc(total);

    if (message == NULL)
        return;
    memcpy(message, head, head_len);
    if (payload != NULL)
    {
        message[head_len] = '\n';
        memcpy(message + head_len + 1, payload, len);
    }
    trace_on_data(message, (int32_t)total);
    free(message);
}

void ansi_host_set_pixel_mode(bool on)
{
    g_pixel_mode = on;
}

bool ansi_host_start(const unsigned char *wad, size_t wad_size, const char *wad_hash)
{
    char start[80];
    const char *log_path = getenv("DOOMDOOR_LOG");

    if (log_path != NULL && *log_path)
        g_log = fopen(log_path, "a");

    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_wad = wad;
    g_wad_size = wad_size;
    snprintf(g_wad_hash, sizeof(g_wad_hash), "%s", wad_hash);

    ansi_game_prepare();
    if (g_pixel_mode)
        tracedoom_ramfs_put("default.cfg", PIXEL_CFG, sizeof(PIXEL_CFG) - 1, 0);
    else
        tracedoom_ramfs_put("default.cfg", DEFAULT_CFG, sizeof(DEFAULT_CFG) - 1, 0);
    tracedoom_ramfs_put("crispy-doom.cfg", CRISPY_CFG, sizeof(CRISPY_CFG) - 1, 0);

    if (trace_init() != 0)
        return false;

    /* The same two messages the door sends over TRACE: which WAD (this starts the game), then the player's saves */
    snprintf(start, sizeof(start), "wad=%s", g_wad_hash);
    trace_on_data(start, (int32_t)strlen(start));
    saves_send_list(native_reply);
    return true;
}

bool ansi_host_frame(uint32_t **pixels, int *width, int *height, unsigned *seq)
{
    bool fresh = false;

    pthread_mutex_lock(&g_frame_lock);
    if (g_frame != NULL && g_frame_seq != *seq)
    {
        size_t need = (size_t)g_frame_w * (size_t)g_frame_h * sizeof(uint32_t);
        uint32_t *copy = (*width) * (*height) >= g_frame_w * g_frame_h ? *pixels : realloc(*pixels, need);
        if (copy != NULL)
        {
            memcpy(copy, g_frame, need);
            *pixels = copy;
            *width = g_frame_w;
            *height = g_frame_h;
            *seq = g_frame_seq;
            fresh = true;
        }
    }
    pthread_mutex_unlock(&g_frame_lock);
    return fresh;
}

void ansi_host_key(int scancode, bool down)
{
    trace_on_input(1, down ? 1 : 0, scancode, 0, 0);    /* TE_IN_KEY: flags bit 0 = pressed, a = scancode */
}

bool ansi_host_finished(void)
{
    return atomic_load(&g_finished) != 0;
}
