/*
 * musrender.c -- renders every music track in the WAD to a WAV file, for the JPEG XL graphics mode.
 *
 * That mode plays music on the caller's own terminal from files it keeps in its cache, so the tracks are rendered
 * here once, ahead of time, on the same emulated OPL chip the game uses everywhere else (module/src/opl_trace.c).
 * make_music.sh then cuts them into the short compressed pieces the door sends (see pix_sound.c).
 *
 * Built with `make musrender`: the game plus ansi_host.c, with three of the host's functions wrapped (--wrap) so that
 * the first frame the game draws starts the rendering instead, and the sound comes here instead of being dropped.
 *
 *   ./musrender <out dir>      (run in the door folder, beside doom1.wad)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ansi_host.h"
#include "../saves.h"
#include "../sha256.h"

#define RATE     44100
#define TAIL_SEC 1.5          /* notes still ringing after the last event */

/* From the game (Crispy's w_wad.h and i_sound.h). Doom's boolean is passed as an int. */
int   W_CheckNumForName(const char *name);
int   W_LumpLength(int lump);
void *W_CacheLumpNum(int lump, int tag);
void *I_RegisterSong(void *data, int len);
void  I_UnRegisterSong(void *handle);
void  I_PlaySong(void *handle, int looping);
void  I_StopSong(void);
void  I_SetMusicVolume(int volume);
void  tracedoom_pump_audio(void);
#define PU_STATIC 1

static const char *g_out_dir;
static int16_t    *g_pcm;
static long        g_pcm_frames, g_pcm_want;

/* A MUS lump's length in seconds: the sum of its delays, at 140 ticks a second */
static double mus_seconds(const uint8_t *d, int len)
{
    int p = d[6] | (d[7] << 8);
    long ticks = 0;
    static const int ARGS[8] = { 1, 1, 1, 1, 2, 0, 0, 1 };

    while (p < len)
    {
        int b = d[p++], type = (b >> 4) & 7;
        if (type == 1 && p < len && (d[p] & 0x80))
            p++;                           /* a note with its volume */
        p += ARGS[type];
        if (type == 6)
            break;                         /* the end of the score */
        if (b & 0x80)
        {
            long v = 0;
            int c;
            do
            {
                c = d[p++];
                v = v * 128 + (c & 127);
            } while ((c & 128) && p < len);
            ticks += v;
        }
    }
    return ticks / 140.0;
}

static void write_wav(const char *path, const int16_t *pcm, long frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)frames * 4, riff = 36 + data, fmt = 16, rate = RATE, bps = RATE * 4;
    uint16_t pcm_tag = 1, channels = 2, align = 4, bits = 16;

    if (f == NULL)
    {
        fprintf(stderr, "can't write %s\n", path);
        exit(1);
    }
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm_tag, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 4, (size_t)frames, f);
    fclose(f);
}

/* ---- the host's functions, wrapped ---- */

void __real_trace_present(const uint32_t *pixels, int32_t width, int32_t height, int32_t flags);

int32_t __wrap_trace_audio_room(void)
{
    return g_pcm_frames < g_pcm_want ? 1024 : 0;
}

int32_t __wrap_trace_audio_write(const int16_t *frames, int32_t count)
{
    long take = count;
    if (g_pcm_frames + take > g_pcm_want)
        take = g_pcm_want - g_pcm_frames;
    memcpy(g_pcm + g_pcm_frames * 2, frames, (size_t)take * 4);
    g_pcm_frames += take;
    return count;
}

/* The first frame: the game is fully up, so render every track, write the index, and stop */
void __wrap_trace_present(const uint32_t *pixels, int32_t width, int32_t height, int32_t flags)
{
    static const char *const TRACKS[] = { "D_E1M1", "D_E1M2", "D_E1M3", "D_E1M4", "D_E1M5", "D_E1M6", "D_E1M7",
                                          "D_E1M8", "D_E1M9", "D_INTER", "D_INTRO", "D_INTROA", "D_VICTOR", "D_BUNNY" };
    char path[512];
    FILE *index;

    (void)pixels; (void)width; (void)height; (void)flags;
    I_StopSong();
    I_SetMusicVolume(127);

    snprintf(path, sizeof(path), "%s/tracks.txt", g_out_dir);
    index = fopen(path, "w");
    for (size_t t = 0; t < sizeof(TRACKS) / sizeof(TRACKS[0]); t++)
    {
        int lump = W_CheckNumForName(TRACKS[t]);
        uint8_t *data;
        int len;
        void *song;
        char hash[65];

        if (lump < 0)
            continue;
        data = W_CacheLumpNum(lump, PU_STATIC);
        len = W_LumpLength(lump);
        sha256_hex(data, (size_t)len, hash);

        g_pcm_want = (long)((mus_seconds(data, len) + TAIL_SEC) * RATE);
        g_pcm_frames = 0;
        g_pcm = realloc(g_pcm, (size_t)g_pcm_want * 4);

        song = I_RegisterSong(data, len);
        I_PlaySong(song, 0);
        while (g_pcm_frames < g_pcm_want)
            tracedoom_pump_audio();
        I_StopSong();
        I_UnRegisterSong(song);

        snprintf(path, sizeof(path), "%s/%s.wav", g_out_dir, TRACKS[t]);
        write_wav(path, g_pcm, g_pcm_frames);
        fprintf(index, "%s %s\n", TRACKS[t], hash);
        fprintf(stderr, "%s: %.1f s\n", TRACKS[t], g_pcm_frames / (double)RATE);
    }
    fclose(index);
    exit(0);
}

int main(int argc, char **argv)
{
    FILE *f = fopen("doom1.wad", "rb");
    long size;
    unsigned char *wad;
    char hash[65];

    if (argc < 2 || f == NULL)
    {
        fprintf(stderr, "usage: musrender <out dir>   (run beside doom1.wad)\n");
        return 1;
    }
    g_out_dir = argv[1];
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    wad = malloc((size_t)size);
    if (wad == NULL || fread(wad, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    sha256_hex(wad, (size_t)size, hash);

    saves_init("musrender", 0);
    if (!ansi_host_start(wad, (size_t)size, hash))
        return 1;
    for (;;)
    {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }
}
