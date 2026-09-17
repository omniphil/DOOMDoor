/*
 * i_timer_trace.c -- Doom's clock, from TRACE instead of SDL. Replaces Crispy Doom's i_timer.c.
 * Everything comes from trace_time_ms(), which is monotonic, so the game's 35 tics a second stay honest.
 */

#include <stdint.h>

#include "doomtype.h"
#include "i_timer.h"
#include "m_fixed.h"
#include <time.h>

#include "trace_api.h"

static int g_basetime;

void I_InitTimer(void)
{
    g_basetime = trace_time_ms();
}

int I_GetTimeMS(void)
{
    return trace_time_ms() - g_basetime;
}

int I_GetTime(void)
{
    return (I_GetTimeMS() * TICRATE) / 1000;
}

uint64_t I_GetTimeUS(void)
{
    return (uint64_t)I_GetTimeMS() * 1000;
}

/* [crispy] how far we are through the current tic, for interpolated rendering */
fixed_t I_GetFracRealTime(void)
{
    return (fixed_t)(((int64_t)I_GetTimeMS() * TICRATE % 1000) * FRACUNIT / 1000);
}

void I_Sleep(int ms)
{
    /* wasi-libc's nanosleep; the sandbox lets a thread wait without spinning a core */
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}
