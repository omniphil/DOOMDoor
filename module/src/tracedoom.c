/*
 * tracedoom.c -- the TRACE side of the Doom module: what TERMinator talks to.
 *
 * The whole game runs here, in the sandbox on the player's PC. The door sends this module and the WAD; after that
 * almost nothing crosses the wire. Doom itself is unmodified Crispy Doom 7.1 from ../third_party.
 *
 * How it fits together:
 *   - The door sends "wad=<sha256>" (and anything else it wants) with TRACE Data. The first one starts the game.
 *   - Doom runs on its own thread, exactly as it always does: D_DoomMain never returns. That way none of Crispy's
 *     code has to be restructured around a per-frame callback.
 *   - The game thread presents frames (i_video_trace.c) and writes sound (i_sound_trace.c) itself.
 *   - Keyboard events arrive on TERMinator's thread and are queued here for the game thread to read.
 */

#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "trace_api.h"
#include "tracedoom.h"

/* ---- what the door told us ---- */
static char  g_wad_hash[65];
static int   g_started;
static pthread_t g_game_thread;

/* ---- events from TERMinator, drained by the game thread ---- */
#define EVENT_MAX 256
static trace_event_t   g_events[EVENT_MAX];
static int             g_event_head, g_event_tail;
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;

void tracedoom_log(const char *fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    trace_log(text, (int32_t)strlen(text));
}

/*
 * Doom's own messages. The sandbox throws away anything written to stdout (a module has no console), so printf and
 * friends are redirected here by include/tracedoom_compat.h and come out in TERMinator's debug output instead.
 * Output is gathered into whole lines, since that is how Doom writes it.
 */
static char g_line[512];
static int  g_line_len;

int tracedoom_vprintf(const char *fmt, va_list args)
{
    char text[512];
    int n = vsnprintf(text, sizeof(text), fmt, args);

    for (const char *p = text; *p; p++)
    {
        if (*p == '\n' || g_line_len == (int)sizeof(g_line) - 1)
        {
            trace_log(g_line, g_line_len);
            g_line_len = 0;
            if (*p != '\n')
                g_line[g_line_len++] = *p;
        }
        else if (*p != '\r')
        {
            g_line[g_line_len++] = *p;
        }
    }
    return n;
}

int tracedoom_printf(const char *fmt, ...)
{
    va_list args;
    int n;
    va_start(args, fmt);
    n = tracedoom_vprintf(fmt, args);
    va_end(args);
    return n;
}

/* Printing to one of the in-memory files (the config) writes to it; anything else is a message for the log. */
int tracedoom_fprintf(void *stream, const char *fmt, ...)
{
    char text[512];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n > 0)
        tracedoom_fputs(text, stream);
    return n;
}

int tracedoom_fputs(const char *text, void *stream)
{
    if (tracedoom_is_ram_file(stream))
    {
        tracedoom_fwrite(text, 1, strlen(text), stream);
        return 0;
    }
    return tracedoom_printf("%s", text);   /* stdout or stderr: a message, not a file */
}

/*
 * Doom quits by calling exit(). Ending this thread isn't enough: the sandbox would keep running and TERMinator would
 * be left showing the last frame for ever, so it's told to stop, which closes the picture and tells the door.
 */
void tracedoom_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void tracedoom_exit(int code)
{
    tracedoom_log("doom: quitting (%d)", code);
    trace_quit(code >= 0 && code < 126 ? code : 0);
    _Exit(0);   /* not reached */
}

const char *tracedoom_wad_hash(void)
{
    return g_wad_hash[0] ? g_wad_hash : NULL;
}

/* The game thread takes events one at a time; TERMinator's thread adds them. */
int tracedoom_next_event(trace_event_t *out)
{
    int got = 0;
    pthread_mutex_lock(&g_event_lock);
    if (g_event_head != g_event_tail) {
        *out = g_events[g_event_head];
        g_event_head = (g_event_head + 1) % EVENT_MAX;
        got = 1;
    }
    pthread_mutex_unlock(&g_event_lock);
    return got;
}

static void queue_event(const trace_event_t *ev)
{
    int next;
    pthread_mutex_lock(&g_event_lock);
    next = (g_event_tail + 1) % EVENT_MAX;
    if (next != g_event_head) {      /* full: drop the newest rather than block the terminal */
        g_events[g_event_tail] = *ev;
        g_event_tail = next;
    }
    pthread_mutex_unlock(&g_event_lock);
}

/* ---- the game thread ---- */

extern void D_DoomMain(void);      /* Crispy Doom's entry point; it never returns */
extern int  myargc;
extern char **myargv;

static void *game_thread(void *arg)
{
    static char *args[] = { "doom", "-iwad", TRACEDOOM_IWAD_NAME, NULL };
    (void)arg;
    myargc = 3;
    myargv = args;
    D_DoomMain();
    return NULL;
}

static void start_game(void)
{
    pthread_attr_t attr;
    if (g_started || !g_wad_hash[0])
        return;
    if (trace_asset_size(g_wad_hash) <= 0) {
        tracedoom_log("doom: the door's WAD isn't here (%s)", g_wad_hash);
        return;
    }
    g_started = 1;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);   /* Doom's renderer is stack-hungry */
    if (pthread_create(&g_game_thread, &attr, game_thread, NULL) != 0)
        tracedoom_log("doom: couldn't start the game thread");
    pthread_attr_destroy(&attr);
}

/* ---- TRACE entry points ---- */

int32_t trace_init(void)
{
    /* Nothing happens until the door says which WAD to use */
    trace_set_tick(0);
    return 0;
}

void *trace_alloc(int32_t size)
{
    return size > 0 ? malloc((size_t)size) : NULL;
}

void trace_free(void *ptr)
{
    free(ptr);
}

void trace_on_resize(int32_t width, int32_t height)
{
    (void)width; (void)height;   /* Doom renders at its own resolution and TERMinator scales it */
}

/*
 * What the door sends. Everything up to the first newline is the message; anything after it is its payload.
 *   wad=<sha256>                          the IWAD to play (starts the game the first time)
 *   list slot=<n>\n<24 bytes>             this player has a save in that slot, and its description
 *   data slot=<n> off=<o> total=<t>\n...  part of a savegame the door is sending back
 *   none slot=<n>                         no such save after all
 * A door can add fields of its own without breaking this one.
 */
void trace_on_data(const char *data, int32_t length)
{
    char head[256];
    const char *payload = NULL;
    int32_t payload_len = 0;
    int head_len;
    int slot = -1;
    long off = 0, total = 0;

    if (length <= 0)
        return;

    /* Split the message from its payload */
    {
        const char *newline = (const char *)memchr(data, '\n', (size_t)length);
        head_len = newline != NULL ? (int)(newline - data) : length;
        if (head_len >= (int)sizeof(head))
            head_len = (int)sizeof(head) - 1;
        memcpy(head, data, (size_t)head_len);
        head[head_len] = 0;
        if (newline != NULL)
        {
            payload = newline + 1;
            payload_len = length - (int32_t)(payload - data);
        }
    }

    if (!strncmp(head, "wad=", 4))
    {
        if (strlen(head + 4) == 64 && !g_wad_hash[0])
        {
            memcpy(g_wad_hash, head + 4, 65);
            start_game();
        }
        return;
    }

    sscanf(strstr(head, "slot=") ? strstr(head, "slot=") : "", "slot=%d", &slot);
    if (strstr(head, "off="))
        sscanf(strstr(head, "off="), "off=%ld", &off);
    if (strstr(head, "total="))
        sscanf(strstr(head, "total="), "total=%ld", &total);

    if (!strncmp(head, "list", 4) && payload != NULL)
        tracedoom_saves_on_list(slot, (const unsigned char *)payload, (size_t)payload_len);
    else if (!strncmp(head, "data", 4) && payload != NULL)
        tracedoom_saves_on_data(slot, (size_t)off, (size_t)total, (const unsigned char *)payload, (size_t)payload_len);
    else if (!strncmp(head, "none", 4))
        tracedoom_saves_on_none(slot);
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c)
{
    trace_event_t ev = { type, flags, a, b, c };
    queue_event(&ev);
}

void trace_update(void)
{
    /* The game thread does the work; there is nothing to do here between events. */
}
