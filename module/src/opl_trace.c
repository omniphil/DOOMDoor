/*
 * opl_trace.c -- the emulated OPL chip, rendered into our own mix instead of SDL_mixer's. Replaces opl_sdl.c.
 *
 * Doom's music is MUS lumps in the WAD, played on an OPL2 as a Sound Blaster would have done in 1993; Crispy's
 * i_oplmusic.c does all of that and just needs somewhere to send register writes and get samples back. This is the
 * same code as Crispy's SDL backend, with two differences: the samples are rendered when we ask for them
 * (i_sound_trace.c calls OPL_Trace_Render after each frame), and the locks are gone, because the music, the sound
 * effects and the game all run on the one thread inside the sandbox.
 *
 * It is registered under the name Crispy's driver list already looks for, so opl.c stays unmodified.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"
#include "opl3.h"

/* The chip's two timers, as opl_sdl.c declares them (it keeps the type to itself) */
typedef struct
{
    unsigned int rate;        /* how many times a second the timer advances */
    unsigned int enabled;
    unsigned int value;       /* the last value written */
    uint64_t expire_time;
} opl_timer_t;

#define RENDER_RATE 44100        /* TE_AUDIO_RATE: the rate TERMinator mixes at */

static opl_callback_queue_t *callback_queue;
static uint64_t  current_time;       /* microseconds since the chip started */
static uint64_t  pause_offset;
static int       opl_paused;
static int       opl_active;
static opl3_chip opl_chip;
static int       opl_opl3mode;
static int       register_num;
static int16_t  *mix_buffer;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

/* Is there a chip to render? i_sound_trace.c asks before mixing. */
int OPL_Trace_Active(void)
{
    return opl_active;
}

/* Runs any music callbacks that have come due, and moves the clock on by that many samples. */
static void AdvanceTime(unsigned int nsamples)
{
    uint64_t us = ((uint64_t)nsamples * OPL_SECOND) / RENDER_RATE;
    current_time += us;

    if (opl_paused)
        pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        opl_callback_t callback;
        void *callback_data;

        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
            break;
        callback(callback_data);
    }
}

/*
 * Renders the next frames of music and adds them to what's already in the buffer, stopping at each point in time
 * where the music has something to do (a note on, a note off), exactly as the SDL backend does.
 */
void OPL_Trace_Render(int16_t *stereo, int frames)
{
    int filled = 0;

    if (!opl_active || mix_buffer == NULL)
        return;

    while (filled < frames)
    {
        uint64_t nsamples;

        if (opl_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = (uint64_t)(frames - filled);
        }
        else
        {
            uint64_t next = OPL_Queue_Peek(callback_queue) + pause_offset;
            nsamples = (next > current_time ? next - current_time : 0) * RENDER_RATE;
            nsamples = (nsamples + OPL_SECOND - 1) / OPL_SECOND;
            if (nsamples > (uint64_t)(frames - filled))
                nsamples = (uint64_t)(frames - filled);
            if (nsamples == 0)
                nsamples = 1;      /* always move forward, so a burst of events can't stall the mix */
        }

        OPL3_GenerateStream(&opl_chip, (Bit16s *)mix_buffer, (Bit32u)nsamples);
        for (uint64_t i = 0; i < nsamples * 2; i++)
        {
            int mixed = stereo[filled * 2 + i] + mix_buffer[i];
            stereo[filled * 2 + i] = (int16_t)(mixed > 32767 ? 32767 : mixed < -32768 ? -32768 : mixed);
        }
        filled += (int)nsamples;

        AdvanceTime((unsigned int)nsamples);
    }
}

/*
 * Renders a little music and throws it away. opl.c calls this (through the SDL shim) when it wants to wait for a
 * callback to come due: with one thread, waiting has to mean moving the chip's clock on.
 */
void tracedoom_opl_pump(void)
{
    static int16_t scratch[64 * 2];
    memset(scratch, 0, sizeof(scratch));
    OPL_Trace_Render(scratch, 64);
}

/* ---- the driver Crispy's opl.c calls ---- */

static int OPL_Trace_Init(unsigned int port_base)
{
    (void)port_base;
    mix_buffer = malloc(sizeof(int16_t) * 2 * RENDER_RATE);   /* room for a second, far more than we mix at once */
    if (mix_buffer == NULL)
        return 0;
    callback_queue = OPL_Queue_Create();
    current_time = 0;
    pause_offset = 0;
    opl_paused = 0;
    OPL3_Reset(&opl_chip, RENDER_RATE);
    opl_active = 1;
    return 1;
}

static void OPL_Trace_Shutdown(void)
{
    opl_active = 0;
    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
    free(mix_buffer);
    mix_buffer = NULL;
}

static unsigned int OPL_Trace_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
        return 0xff;

    if (timer1.enabled && current_time > timer1.expire_time)
        result |= 0x80 | 0x40;
    if (timer2.enabled && current_time > timer2.expire_time)
        result |= 0x80 | 0x20;

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    if (timer->enabled)
    {
        int tics = 0x100 - timer->value;
        timer->expire_time = current_time + ((uint64_t)tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
    case OPL_REG_TIMER1:
        timer1.value = value;
        OPLTimer_CalculateEndTime(&timer1);
        break;

    case OPL_REG_TIMER2:
        timer2.value = value;
        OPLTimer_CalculateEndTime(&timer2);
        break;

    case OPL_REG_TIMER_CTRL:
        if (value & 0x80)
        {
            timer1.enabled = 0;
            timer2.enabled = 0;
        }
        else
        {
            if ((value & 0x40) == 0)
            {
                timer1.enabled = (value & 0x01) != 0;
                OPLTimer_CalculateEndTime(&timer1);
            }
            if ((value & 0x20) == 0)
            {
                timer2.enabled = (value & 0x02) != 0;
                OPLTimer_CalculateEndTime(&timer2);
            }
        }
        break;

    case OPL_REG_NEW:
        opl_opl3mode = value & 0x01;
        /* fall through */

    default:
        OPL3_WriteRegBuffered(&opl_chip, (Bit16u)reg_num, (Bit8u)value);
        break;
    }
}

static void OPL_Trace_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
        register_num = value;
    else if (port == OPL_REGISTER_PORT_OPL3)
        register_num = value | 0x100;
    else if (port == OPL_DATA_PORT)
        WriteRegister(register_num, value);
}

static void OPL_Trace_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    OPL_Queue_Push(callback_queue, callback, data, current_time - pause_offset + us);
}

static void OPL_Trace_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

/* One thread does all of it, so there is nothing to lock against. */
static void OPL_Trace_Lock(void) { }
static void OPL_Trace_Unlock(void) { }

static void OPL_Trace_SetPaused(int paused)
{
    opl_paused = paused;
}

static void OPL_Trace_AdjustCallbacks(float factor)
{
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
}

opl_driver_t opl_sdl_driver =
{
    "TRACE",
    OPL_Trace_Init,
    OPL_Trace_Shutdown,
    OPL_Trace_PortRead,
    OPL_Trace_PortWrite,
    OPL_Trace_SetCallback,
    OPL_Trace_ClearCallbacks,
    OPL_Trace_Lock,
    OPL_Trace_Unlock,
    OPL_Trace_SetPaused,
    OPL_Trace_AdjustCallbacks,
};
