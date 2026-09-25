/*
 * pix_sound.c -- DOOM's sound effects and music on the caller's own terminal, for the JPEG XL graphics mode.
 *
 * The terminal plays sound files it keeps in a cache of its own, one per BBS, so nothing is streamed as raw audio:
 *
 *   Effects  every DS* lump in the WAD, as a small WAV file, uploaded once (checked by md5 on every call) and loaded
 *            into the terminal's sound slots 0-99 at the start. Playing one is then a few dozen bytes: stop that
 *            channel, set its left/right volume, copy the effect into the channel's own slot, queue it.
 *   Music    the WAD's tracks rendered ahead of time on the game's OPL chip (tools/musrender.c) and cut into 5-second
 *            Ogg Vorbis pieces (tools/make_music.py, door/music/). A track plays as its pieces queued back to back on
 *            one channel, which the terminal joins seamlessly; each piece is uploaded only when it is about to be
 *            needed, or earlier while the link has room to spare, and stays in the cache for next time.
 *
 * Volumes follow the game's own mixer (module/src/i_sound_trace.c), so the balance matches TRACE: an effect's side
 * gets vol * (255 - sep) / 255 out of 256, and music its volume out of 127.
 *
 * Terminal resources used: slots 0-99 effects, 200-215 one per effect channel, 250 music; channel 2 music, 3-14 the
 * game's effect channels.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "door.h"
#include "pix_hooks.h"
#include "pix_sound.h"
#include "trace_door.h"
#include "trace_doom.h"

#define SFX_DIR      "doomdoor/sfx/"
#define MUS_DIR      "doomdoor/mus/"
#define MAX_SFX      100
#define MAX_TRACKS   20
#define MAX_PIECES   64
#define PIECE_MS     5000
#define MUSIC_CH     2
#define SFX_CH0      3
#define SFX_CHANNELS 12
#define SCRATCH_SLOT 200
#define MUSIC_SLOT   250

#define QUEUE_AHEAD_MS  8000    /* music queued this far ahead of what's playing */
#define URGENT_MS       3000    /* a piece needed sooner than this is sent even if the picture has to wait */

typedef struct
{
    char     name[9];
    uint8_t *wav;
    size_t   size;
} sfx_t;

typedef struct
{
    char     name[9];
    char     hash[65];
    int      pieces, last_ms;
    uint8_t *data[MAX_PIECES];
    size_t   size[MAX_PIECES];
    bool     cached[MAX_PIECES];
} track_t;

static sfx_t   g_sfx[MAX_SFX];
static int     g_sfx_count;
static track_t g_tracks[MAX_TRACKS];
static int     g_track_count;
static bool    g_ready;

/* The music as it stands */
static struct
{
    int  track;             /* -1 none */
    bool looping, paused;
    int  next;              /* the next piece to queue */
    long queued_until;      /* when what's queued runs out */
    int  playing_piece;     /* for resuming after a pause: the piece that was playing */
    long piece_started[MAX_PIECES];
    int  volume;            /* 0-127 */
} M = { .track = -1, .volume = 127 };

/* ---- building the files ---- */

/* A DMX lump as a WAV file: 8-bit unsigned mono at its own rate, without the 16 bytes of padding at each end */
static bool make_wav(const uint8_t *d, size_t len, uint8_t **out, size_t *out_len)
{
    uint32_t rate, samples, data_len, riff, bps;
    uint8_t *w;

    if (len < 8 || d[0] != 3 || d[1] != 0)
        return false;
    rate = d[2] | (d[3] << 8);
    samples = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (samples > len - 8)
        samples = (uint32_t)(len - 8);
    if (samples <= 32 || rate == 0)
        return false;
    data_len = samples - 32;
    riff = 36 + data_len;
    bps = rate;

    w = malloc(44 + data_len);
    if (w == NULL)
        return false;
    memcpy(w, "RIFF", 4);
    memcpy(w + 4, &riff, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    memcpy(w + 16, "\x10\0\0\0\x01\0\x01\0", 8);      /* 16-byte fmt, PCM, 1 channel */
    memcpy(w + 24, &rate, 4);
    memcpy(w + 28, &bps, 4);
    memcpy(w + 32, "\x01\0\x08\0", 4);                 /* 1 byte a frame, 8 bits */
    memcpy(w + 36, "data", 4);
    memcpy(w + 40, &data_len, 4);
    memcpy(w + 44, d + 8 + 16, data_len);
    *out = w;
    *out_len = 44 + data_len;
    return true;
}

static void load_sfx(void)
{
    const uint8_t *wad = trace_doom_wad_data();
    size_t wad_size = trace_doom_wad_size();
    uint32_t n, dir;

    if (wad == NULL || wad_size < 12)
        return;
    memcpy(&n, wad + 4, 4);
    memcpy(&dir, wad + 8, 4);
    for (uint32_t i = 0; i < n && g_sfx_count < MAX_SFX && dir + 16 * (i + 1) <= wad_size; i++)
    {
        const uint8_t *e = wad + dir + 16 * i;
        uint32_t pos, size;
        sfx_t *s = &g_sfx[g_sfx_count];

        if (e[8] != 'D' || e[9] != 'S')
            continue;
        memcpy(&pos, e, 4);
        memcpy(&size, e + 4, 4);
        if ((size_t)pos + size > wad_size)
            continue;
        memset(s->name, 0, sizeof(s->name));
        memcpy(s->name, e + 8, 8);
        if (make_wav(wad + pos, size, &s->wav, &s->size))
            g_sfx_count++;
    }
}

static void load_music(void)
{
    tdoor_blob_t index;
    char *text, *line, *save = NULL;

    if (!tdoor_load_blob("music/index.txt", &index, 64 * 1024))
        return;
    text = malloc(index.size + 1);
    if (text == NULL)
        return;
    memcpy(text, index.data, index.size);
    text[index.size] = '\0';

    for (line = strtok_r(text, "\n", &save); line != NULL && g_track_count < MAX_TRACKS; line = strtok_r(NULL, "\n", &save))
    {
        track_t *t = &g_tracks[g_track_count];
        bool all = true;

        if (sscanf(line, "%8s %64s %d %d", t->name, t->hash, &t->pieces, &t->last_ms) != 4 || t->pieces <= 0 ||
            t->pieces > MAX_PIECES)
            continue;
        for (int k = 0; k < t->pieces && all; k++)
        {
            char path[64];
            tdoor_blob_t piece;
            snprintf(path, sizeof(path), "music/%s_%02d.ogg", t->name, k);
            if (tdoor_load_blob(path, &piece, 1024 * 1024))
            {
                t->data[k] = piece.data;
                t->size[k] = piece.size;
            }
            else
            {
                all = false;
            }
        }
        if (all)
            g_track_count++;
    }
    free(text);
}

/* ---- what the terminal already has ---- */

/* Asks for the cache listing of one folder and reads the reply: lines of name TAB md5, between APC markers.
 * Returns the reply text (static), or NULL if none came. */
static const char *list_cache(const char *dir)
{
    static char reply[65536];
    size_t n = 0;
    int c;

    door_write(APC_PREFIX "C;L;");
    door_write(dir);
    door_write("*" APC_END);

    while (n < sizeof(reply) - 1)
    {
        c = door_read_char_timeout(n == 0 ? 3000 : 1000);
        if (c < 0)
            return NULL;
        reply[n++] = (char)c;
        if (n >= 2 && reply[n - 2] == '\033' && reply[n - 1] == '\\')
            break;
    }
    reply[n] = '\0';
    return strstr(reply, "C;L") != NULL ? reply : NULL;
}

/* Does the listing have this file with this content? */
static bool listed(const char *listing, const char *name, const void *data, size_t size)
{
    char want[128], md5[33];
    md5_hex(data, size, md5);
    snprintf(want, sizeof(want), "\n%s\t%s", name, md5);
    return listing != NULL && strstr(listing, want) != NULL;
}

/*
 * Waits for one answer to a cursor-position request (ESC [ row ; col R), which says everything sent before it has
 * reached the terminal. The upload below keeps a few of these outstanding, so its progress is what has arrived, not
 * what has been handed to the network, and the game doesn't start with the tail of it still queued in front.
 */
static bool wait_arrived(void)
{
    int c;
    while ((c = door_read_char_timeout(30000)) >= 0)
        if (c == 'R')
            return true;
    return false;
}

#define UPLOAD_AHEAD 65536      /* bytes sent before waiting for them to arrive */

bool pix_sound_prepare(void (*progress)(int percent))
{
    const char *listing;
    size_t total = 0, sent = 0;
    bool need[MAX_SFX];

    load_sfx();
    load_music();
    if (g_sfx_count == 0)
        return false;

    listing = list_cache(SFX_DIR);
    for (int i = 0; i < g_sfx_count; i++)
    {
        char file[32];
        snprintf(file, sizeof(file), "%.8s.wav", g_sfx[i].name);
        need[i] = !listed(listing, file, g_sfx[i].wav, g_sfx[i].size);
        if (need[i])
            total += g_sfx[i].size;
    }
    {
        size_t queued[MAX_SFX], arrived = 0;
        int ahead = 0, first = 0;

        for (int i = 0; i < g_sfx_count; i++)
        {
            outbuf_t o = { 0 };
            char head[64];
            if (!need[i])
                continue;
            snprintf(head, sizeof(head), "C;S;" SFX_DIR "%.8s.wav;", g_sfx[i].name);
            apc_blob(&o, head, g_sfx[i].wav, g_sfx[i].size);
            out_str(&o, "\033[6n");
            door_write_raw(o.data, o.len);
            free(o.data);
            queued[ahead++] = g_sfx[i].size;
            sent += g_sfx[i].size;
            /* keep no more than UPLOAD_AHEAD on its way */
            while (sent - arrived > UPLOAD_AHEAD && first < ahead && wait_arrived())
            {
                arrived += queued[first++];
                if (progress != NULL)
                    progress((int)(arrived * 100 / total));
            }
        }
        while (first < ahead && wait_arrived())
        {
            arrived += queued[first++];
            if (progress != NULL)
                progress((int)(arrived * 100 / total));
        }
    }

    listing = list_cache(MUS_DIR);
    for (int t = 0; t < g_track_count; t++)
        for (int k = 0; k < g_tracks[t].pieces; k++)
        {
            char file[32];
            snprintf(file, sizeof(file), "%.8s_%02d.ogg", g_tracks[t].name, k);
            g_tracks[t].cached[k] = listed(listing, file, g_tracks[t].data[k], g_tracks[t].size[k]);
        }

    g_ready = true;
    return true;
}

/* ---- playing ---- */

static int db(double gain)
{
    return gain <= 0.001 ? -60 : (int)lround(20.0 * log10(gain));
}

static int find_sfx(const char *name)
{
    for (int i = 0; i < g_sfx_count; i++)
        if (strncmp(g_sfx[i].name, name, 8) == 0)
            return i;
    return -1;
}

void pix_sound_start(outbuf_t *o)
{
    if (!g_ready)
        return;
    for (int i = 0; i < g_sfx_count; i++)
        apc_cmd(o, "A;Load;S=%d;" SFX_DIR "%s.wav", i, g_sfx[i].name);
    apc_cmd(o, "A;Volume;C=%d;V=%ddB", MUSIC_CH, db(M.volume / 127.0));
}

static void sfx_volume(outbuf_t *o, int ch, int vol, int sep)
{
    double left = vol * (255 - sep) / 255.0 / 256.0, right = vol * sep / 255.0 / 256.0;
    apc_cmd(o, "A;Volume;C=%d;VL=%ddB;VR=%ddB", SFX_CH0 + ch % SFX_CHANNELS, db(left), db(right));
}

/* At once, with no fade: a fade (O=) only fades the last piece queued, and the several seconds queued in front of it
 * would play on (after the game ended, or over the next level's track) */
static void music_stop(outbuf_t *o)
{
    apc_cmd(o, "A;Flush;C=%d", MUSIC_CH);
}

static void handle(outbuf_t *o, const pix_event_t *ev, long now)
{
    int ch = SFX_CH0 + ev->channel % SFX_CHANNELS;

    switch (ev->kind)
    {
    case PIX_SFX_START:
    {
        int s = find_sfx(ev->name);
        if (s < 0)
            return;
        apc_cmd(o, "A;Flush;C=%d", ch);
        sfx_volume(o, ev->channel, ev->vol, ev->sep);
        apc_cmd(o, "A;Copy;S=%d;D=%d", s, SCRATCH_SLOT + ev->channel % SFX_CHANNELS);
        apc_cmd(o, "A;Queue;C=%d;S=%d", ch, SCRATCH_SLOT + ev->channel % SFX_CHANNELS);
        return;
    }
    case PIX_SFX_PARAMS:
        sfx_volume(o, ev->channel, ev->vol, ev->sep);
        return;
    case PIX_SFX_STOP:
        apc_cmd(o, "A;Flush;C=%d;O=10", ch);
        return;

    case PIX_MUS_PLAY:
        music_stop(o);
        M.track = -1;
        for (int t = 0; t < g_track_count; t++)
            if (strcmp(g_tracks[t].hash, ev->hash) == 0)
                M.track = t;
        M.looping = ev->looping != 0;
        M.paused = false;
        M.next = 0;
        M.playing_piece = 0;
        M.queued_until = now;
        return;
    case PIX_MUS_STOP:
        music_stop(o);
        M.track = -1;
        return;
    case PIX_MUS_PAUSE:
        if (M.track < 0 || M.paused)
            return;
        /* Remember which piece was playing, and start again from its beginning on resume */
        M.playing_piece = M.next > 0 ? M.next - 1 : 0;
        for (int k = 0; k < M.next; k++)
            if (M.piece_started[k] <= now)
                M.playing_piece = k;
        music_stop(o);
        M.paused = true;
        return;
    case PIX_MUS_RESUME:
        if (M.track < 0 || !M.paused)
            return;
        M.paused = false;
        M.next = M.playing_piece;
        M.queued_until = now;
        return;
    case PIX_MUS_VOLUME:
        M.volume = ev->vol;
        apc_cmd(o, "A;Volume;C=%d;V=%ddB", MUSIC_CH, db(M.volume / 127.0));
        return;
    }
}

static int piece_ms(const track_t *t, int k)
{
    return k == t->pieces - 1 ? t->last_ms : PIECE_MS;
}

void pix_sound_update(outbuf_t *o, long now)
{
    pix_event_t ev;

    if (!g_ready)
    {
        while (pix_hooks_next(&ev))
            ;
        return;
    }
    while (pix_hooks_next(&ev))
        handle(o, &ev, now);

    /* Keep the music queued ahead, as far as its pieces have reached the terminal */
    while (M.track >= 0 && !M.paused && M.next < g_tracks[M.track].pieces && M.queued_until - now < QUEUE_AHEAD_MS)
    {
        track_t *t = &g_tracks[M.track];
        if (!t->cached[M.next])
            break;
        if (M.queued_until < now)
            M.queued_until = now;       /* it ran dry (a piece arrived late): start again from now */
        apc_cmd(o, "A;Load;S=%d;" MUS_DIR "%s_%02d.ogg", MUSIC_SLOT, t->name, M.next);
        apc_cmd(o, "A;Queue;C=%d;S=%d", MUSIC_CH, MUSIC_SLOT);
        M.piece_started[M.next] = M.queued_until;
        M.queued_until += piece_ms(t, M.next);
        M.next++;
        if (M.next == t->pieces)
        {
            if (!M.looping)
                break;
            M.next = 0;
        }
    }
}

/* The next piece to upload: the one the music will reach first, then the rest of this track, then other tracks */
static bool next_upload(int *track, int *piece, long now, bool *urgent)
{
    if (M.track >= 0)
    {
        track_t *t = &g_tracks[M.track];
        int k = M.paused ? M.playing_piece : M.next;
        long due = M.paused ? now + URGENT_MS * 2 : M.queued_until;
        for (int i = 0; i < t->pieces; i++, k = (k + 1) % t->pieces, due += PIECE_MS)
            if (!t->cached[k])
            {
                *track = M.track;
                *piece = k;
                *urgent = due - now < URGENT_MS;
                return true;
            }
    }
    for (int tr = 0; tr < g_track_count; tr++)
        for (int k = 0; k < g_tracks[tr].pieces; k++)
            if (!g_tracks[tr].cached[k])
            {
                *track = tr;
                *piece = k;
                *urgent = false;
                return true;
            }
    return false;
}

int pix_sound_upload_wanted(long now)
{
    int t, k;
    bool urgent;
    if (!g_ready || !next_upload(&t, &k, now, &urgent))
        return 0;
    return urgent ? 2 : 1;
}

size_t pix_sound_upload(outbuf_t *o, long now)
{
    int t, k;
    bool urgent;
    char head[64];
    size_t before = o->len;

    if (!g_ready || !next_upload(&t, &k, now, &urgent))
        return 0;
    snprintf(head, sizeof(head), "C;S;" MUS_DIR "%s_%02d.ogg;", g_tracks[t].name, k);
    apc_blob(o, head, g_tracks[t].data[k], g_tracks[t].size[k]);
    g_tracks[t].cached[k] = true;
    return o->len - before;
}

void pix_sound_stop(outbuf_t *o)
{
    if (!g_ready)
        return;
    music_stop(o);
    for (int c = 0; c < SFX_CHANNELS; c++)
        apc_cmd(o, "A;Flush;C=%d", SFX_CH0 + c);
}
