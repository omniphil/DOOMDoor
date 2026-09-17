/*
 * i_sound_trace.c -- Doom's sound, mixed here and handed to TERMinator. Replaces Crispy Doom's i_sdlsound.c.
 *
 * Sound effects are the WAD's own DMX lumps (8-bit, usually 11025 Hz), resampled and mixed into 44.1 kHz stereo with
 * vanilla's volume and left/right separation. Music is the WAD's MUS lumps played on the emulated OPL chip, exactly
 * as a Sound Blaster would have (opl_trace.c); Crispy's i_oplmusic.c drives it unchanged.
 *
 * Nothing is streamed from the BBS: every sound comes out of the WAD the door already sent.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

#include "trace_api.h"
#include "tracedoom.h"

#define OUT_RATE     44100          /* TE_AUDIO_RATE */
#define NUM_CHANNELS 16             /* vanilla mixes 8; Crispy allows more */
#define MIX_FRAMES   1024           /* how much we mix in one go */

extern int snd_samplerate;

/* A sound effect, decoded from its WAD lump into 16-bit mono at the output rate. */
typedef struct
{
    int16_t *samples;
    int      length;
} sfx_data_t;

typedef struct
{
    const sfx_data_t *sfx;
    int   position;                 /* in samples */
    int   left, right;              /* 0-255 */
    boolean playing;
} channel_t;

static channel_t g_channels[NUM_CHANNELS];
static boolean   g_sound_ready;
static int       g_music_volume = 127;

/* OPL music rendering lives in opl_trace.c */
void OPL_Trace_Render(int16_t *stereo, int frames);
int  OPL_Trace_Active(void);

/* ---- decoding the WAD's DMX sound lumps ---- */

/*
 * A DMX lump is: u16 format (3), u16 sample rate, u32 length, then 8-bit unsigned samples with 16 pad bytes at each
 * end. Vanilla's sounds are 11025 Hz, so they are resampled up to the output rate with linear interpolation.
 */
static boolean DecodeSfx(sfxinfo_t *sfxinfo)
{
    byte *data;
    int lumpnum, lumplen, rate, length;
    sfx_data_t *sfx;

    lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0)
        return false;
    lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8)
        return false;
    data = W_CacheLumpNum(lumpnum, PU_STATIC);

    if (data[0] != 0x03 || data[1] != 0x00)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }
    rate = data[2] | (data[3] << 8);
    length = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
    if (rate == 0 || length <= 32 || length > lumplen - 8)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }
    /* skip the pad bytes at both ends, as vanilla does */
    length -= 32;

    {
        const byte *pcm = data + 8 + 16;
        int out_length = (int)(((int64_t)length * OUT_RATE) / rate);
        int i;

        sfx = Z_Malloc(sizeof(*sfx), PU_STATIC, NULL);
        sfx->length = out_length;
        sfx->samples = Z_Malloc(out_length * sizeof(int16_t), PU_STATIC, NULL);

        for (i = 0; i < out_length; i++)
        {
            /* linear interpolation between the two nearest source samples */
            int64_t pos = (int64_t)i * rate;
            int index = (int)(pos / OUT_RATE);
            int frac = (int)(pos % OUT_RATE);
            int a = pcm[index < length ? index : length - 1];
            int b = pcm[index + 1 < length ? index + 1 : length - 1];
            int value = a + ((b - a) * frac) / OUT_RATE;
            sfx->samples[i] = (int16_t)((value - 128) << 8);
        }
    }
    W_ReleaseLumpNum(lumpnum);
    sfxinfo->driver_data = sfx;
    return true;
}

/* ---- the sound module Doom calls ---- */

static boolean I_Trace_InitSound(GameMission_t mission)
{
    (void)mission;
    memset(g_channels, 0, sizeof(g_channels));
    g_sound_ready = true;
    return true;
}

static void I_Trace_ShutdownSound(void)
{
    g_sound_ready = false;
}

static int I_Trace_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    if (sfx->link != NULL)
        sfx = sfx->link;
    M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    /* Checked, not demanded: the shareware WAD is missing a few sounds Crispy knows about (the menu ones), and a
     * missing sound should be silence, not the end of the game. */
    return W_CheckNumForName(namebuf);
}

static void I_Trace_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return;
    /* vanilla's separation: 0 is hard left, 255 hard right */
    g_channels[channel].left = (vol * (255 - sep)) / 255;
    g_channels[channel].right = (vol * sep) / 255;
}

static int I_Trace_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch)
{
    (void)pitch;   /* vanilla pitch-shifting is off by default in Crispy too */
    if (!g_sound_ready || channel < 0 || channel >= NUM_CHANNELS)
        return -1;
    if (sfxinfo->driver_data == NULL && !DecodeSfx(sfxinfo))
        return -1;

    g_channels[channel].sfx = (const sfx_data_t *)sfxinfo->driver_data;
    g_channels[channel].position = 0;
    g_channels[channel].playing = true;
    I_Trace_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_Trace_StopSound(int channel)
{
    if (channel >= 0 && channel < NUM_CHANNELS)
        g_channels[channel].playing = false;
}

static boolean I_Trace_SoundIsPlaying(int channel)
{
    return channel >= 0 && channel < NUM_CHANNELS && g_channels[channel].playing;
}

static void I_Trace_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;   /* decoded on first use instead: the player is waiting at startup */
}

static void I_Trace_UpdateSound(void)
{
    /* The mixing happens in tracedoom_pump_audio, right after each frame is drawn. */
}

static const snddevice_t sound_devices[] = { SNDDEVICE_SB, SNDDEVICE_PCSPEAKER, SNDDEVICE_ADLIB };

/*
 * Registered under the name i_sound.c's own list already looks for (the PC speaker slot, which this module replaces),
 * so that file stays unmodified. It answers for every device Doom might ask for, since there is only one way out.
 */
const sound_module_t sound_pcsound_module =
{
    sound_devices,
    arrlen(sound_devices),
    I_Trace_InitSound,
    I_Trace_ShutdownSound,
    I_Trace_GetSfxLumpNum,
    I_Trace_UpdateSound,
    I_Trace_UpdateSoundParams,
    I_Trace_StartSound,
    I_Trace_StopSound,
    I_Trace_SoundIsPlaying,
    I_Trace_CacheSounds,
};

/* ---- mixing, and handing the result to TERMinator ---- */

static void MixFrames(int16_t *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));

    if (OPL_Trace_Active())
        OPL_Trace_Render(out, frames);   /* music first: the effects mix on top of it */

    for (int c = 0; c < NUM_CHANNELS; c++)
    {
        channel_t *ch = &g_channels[c];
        if (!ch->playing || ch->sfx == NULL)
            continue;

        for (int i = 0; i < frames; i++)
        {
            int sample;
            if (ch->position >= ch->sfx->length)
            {
                ch->playing = false;
                break;
            }
            sample = ch->sfx->samples[ch->position++];
            {
                int left = out[i * 2] + ((sample * ch->left) >> 8);
                int right = out[i * 2 + 1] + ((sample * ch->right) >> 8);
                out[i * 2] = (int16_t)(left > 32767 ? 32767 : left < -32768 ? -32768 : left);
                out[i * 2 + 1] = (int16_t)(right > 32767 ? 32767 : right < -32768 ? -32768 : right);
            }
        }
    }
}

/*
 * Called after each frame is drawn: tops the sound queue up as far as TERMinator will take, so there is always a
 * little ahead of what the player is hearing. Keeping it here means the picture and the sound come from the same
 * thread and can't drift apart.
 */
void tracedoom_pump_audio(void)
{
    static int16_t buffer[MIX_FRAMES * 2];
    int room;

    if (!g_sound_ready)
        return;
    while ((room = trace_audio_room()) >= MIX_FRAMES)
    {
        MixFrames(buffer, MIX_FRAMES);
        if (trace_audio_write(buffer, MIX_FRAMES) <= 0)
            break;
    }
}
