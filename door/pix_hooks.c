/*
 * pix_hooks.c -- Doom's sound and music calls, caught for the JPEG XL graphics mode.
 *
 * In that mode the sound plays on the caller's terminal, from files it keeps in its cache (pix_sound.c), so what the
 * door needs from the game is not the mixed sound but the events: this sound started on that channel at this volume,
 * this track began. Crispy's i_sound.c is left alone: the door is linked with --wrap for each of these functions
 * (Makefile), so the game's calls land here first and are passed on unchanged when the mode is off.
 *
 * Runs on the game's thread; the door reads the events from its own, hence the lock.
 *
 * Also the frame rate Crispy shows when the player types IDRATE: the game's own count is always 35 here, since the
 * game runs on the BBS, so in this mode it shows the frames that actually reached the player's terminal instead.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "crispy.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "pix_hooks.h"
#include "sha256.h"

#define QUEUE     256
#define CHANNELS  16
#define MAX_SONGS 8

static atomic_int      g_on;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pix_event_t     g_queue[QUEUE];
static int             g_head, g_tail;

static long g_ends_at[CHANNELS];                                    /* when each channel's sound runs out, in ms */
static struct { void *handle; char hash[65]; } g_songs[MAX_SONGS];  /* registered songs, by the game's handle */

void pix_hooks_enable(bool on)
{
    atomic_store(&g_on, on ? 1 : 0);
}

bool pix_hooks_next(pix_event_t *ev)
{
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_head != g_tail)
    {
        *ev = g_queue[g_head];
        g_head = (g_head + 1) % QUEUE;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

static void push(const pix_event_t *ev)
{
    pthread_mutex_lock(&g_lock);
    if ((g_tail + 1) % QUEUE != g_head)
    {
        g_queue[g_tail] = *ev;
        g_tail = (g_tail + 1) % QUEUE;
    }
    pthread_mutex_unlock(&g_lock);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* How long a DMX sound lump plays: u16 format, u16 rate, u32 samples (16 padding at each end), then the samples */
static long sfx_ms(int lumpnum)
{
    const byte *d;
    long rate, samples;

    if (lumpnum < 0 || W_LumpLength(lumpnum) < 8)
        return 0;
    d = W_CacheLumpNum(lumpnum, PU_CACHE);
    rate = d[2] | (d[3] << 8);
    samples = (long)d[4] | ((long)d[5] << 8) | ((long)d[6] << 16) | ((long)d[7] << 24);
    if (rate <= 0 || samples <= 32)
        return 0;
    return (samples - 32) * 1000 / rate;
}

/* ---- sound effects ---- */

int  __real_I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch);
void __real_I_UpdateSoundParams(int channel, int vol, int sep);
void __real_I_StopSound(int channel);
boolean __real_I_SoundIsPlaying(int channel);

int __wrap_I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch)
{
    pix_event_t ev = { 0 };
    int lumpnum;

    if (!atomic_load(&g_on))
        return __real_I_StartSound(sfxinfo, channel, vol, sep, pitch);
    if (channel < 0 || channel >= CHANNELS)
        return -1;

    lumpnum = sfxinfo->lumpnum >= 0 ? sfxinfo->lumpnum : I_GetSfxLumpNum(sfxinfo);
    if (lumpnum < 0)
        return -1;
    ev.kind = PIX_SFX_START;
    ev.channel = channel;
    ev.vol = vol;
    ev.sep = sep;
    memcpy(ev.name, lumpinfo[lumpnum]->name, 8);
    push(&ev);
    g_ends_at[channel] = now_ms() + sfx_ms(lumpnum);
    return channel;
}

void __wrap_I_UpdateSoundParams(int channel, int vol, int sep)
{
    pix_event_t ev = { 0 };

    if (!atomic_load(&g_on))
    {
        __real_I_UpdateSoundParams(channel, vol, sep);
        return;
    }
    ev.kind = PIX_SFX_PARAMS;
    ev.channel = channel;
    ev.vol = vol;
    ev.sep = sep;
    push(&ev);
}

void __wrap_I_StopSound(int channel)
{
    pix_event_t ev = { 0 };

    if (!atomic_load(&g_on))
    {
        __real_I_StopSound(channel);
        return;
    }
    if (channel >= 0 && channel < CHANNELS)
        g_ends_at[channel] = 0;
    ev.kind = PIX_SFX_STOP;
    ev.channel = channel;
    push(&ev);
}

/* The game asks so it can reuse channels whose sounds have finished: answered from each sound's length */
boolean __wrap_I_SoundIsPlaying(int channel)
{
    if (!atomic_load(&g_on))
        return __real_I_SoundIsPlaying(channel);
    return channel >= 0 && channel < CHANNELS && now_ms() < g_ends_at[channel];
}

/* ---- music ----
 *
 * The real calls still happen, so the game's own music state stays right; they just never reach a speaker. Songs
 * are known by the hash of their MUS lump, which is how music/index.txt names the pre-rendered tracks. */

void *__real_I_RegisterSong(void *data, int len);
void  __real_I_UnRegisterSong(void *handle);
void  __real_I_PlaySong(void *handle, boolean looping);
void  __real_I_StopSong(void);
void  __real_I_PauseSong(void);
void  __real_I_ResumeSong(void);
void  __real_I_SetMusicVolume(int volume);

void *__wrap_I_RegisterSong(void *data, int len)
{
    void *handle = __real_I_RegisterSong(data, len);

    for (int i = 0; i < MAX_SONGS; i++)
        if (g_songs[i].handle == NULL || g_songs[i].handle == handle)
        {
            g_songs[i].handle = handle;
            sha256_hex(data, (size_t)len, g_songs[i].hash);
            break;
        }
    return handle;
}

void __wrap_I_UnRegisterSong(void *handle)
{
    for (int i = 0; i < MAX_SONGS; i++)
        if (g_songs[i].handle == handle)
            g_songs[i].handle = NULL;
    __real_I_UnRegisterSong(handle);
}

void __wrap_I_PlaySong(void *handle, boolean looping)
{
    __real_I_PlaySong(handle, looping);
    if (!atomic_load(&g_on))
        return;
    for (int i = 0; i < MAX_SONGS; i++)
        if (g_songs[i].handle == handle && handle != NULL)
        {
            pix_event_t ev = { 0 };
            ev.kind = PIX_MUS_PLAY;
            ev.looping = looping ? 1 : 0;
            memcpy(ev.hash, g_songs[i].hash, sizeof(ev.hash));
            push(&ev);
            return;
        }
}

static void simple(void (*real)(void), pix_event_kind_t kind)
{
    real();
    if (atomic_load(&g_on))
    {
        pix_event_t ev = { 0 };
        ev.kind = kind;
        push(&ev);
    }
}

void __wrap_I_StopSong(void)   { simple(__real_I_StopSong, PIX_MUS_STOP); }
void __wrap_I_PauseSong(void)  { simple(__real_I_PauseSong, PIX_MUS_PAUSE); }
void __wrap_I_ResumeSong(void) { simple(__real_I_ResumeSong, PIX_MUS_RESUME); }

void __wrap_I_SetMusicVolume(int volume)
{
    __real_I_SetMusicVolume(volume);
    if (atomic_load(&g_on))
    {
        pix_event_t ev = { 0 };
        ev.kind = PIX_MUS_VOLUME;
        ev.vol = volume;
        push(&ev);
    }
}

/* ---- IDRATE ---- */

static atomic_int g_fps = -1;

void pix_hooks_set_fps(int fps)
{
    atomic_store(&g_fps, fps);
}

void __real_I_FinishUpdate(void);

/* The game sets crispy->fps (its own rate) in I_FinishUpdate; it's put back to the delivered rate straight after,
 * well before the next frame's HUD shows it */
void __wrap_I_FinishUpdate(void)
{
    int fps;
    __real_I_FinishUpdate();
    fps = atomic_load(&g_fps);
    if (fps >= 0)
        crispy->fps = fps;
}
