/*
 * pix_hooks.h -- Doom's sound and music calls, caught for the JPEG XL graphics mode. See pix_hooks.c.
 */

#ifndef PIX_HOOKS_H
#define PIX_HOOKS_H

#include <stdbool.h>

typedef enum
{
    PIX_SFX_START,      /* name, channel, vol, sep */
    PIX_SFX_PARAMS,     /* channel, vol, sep: a sound moved or got quieter while playing */
    PIX_SFX_STOP,       /* channel */
    PIX_MUS_PLAY,       /* hash (of the MUS lump), looping */
    PIX_MUS_STOP,
    PIX_MUS_PAUSE,
    PIX_MUS_RESUME,
    PIX_MUS_VOLUME,     /* vol, 0-127 */
} pix_event_kind_t;

typedef struct
{
    pix_event_kind_t kind;
    int  channel, vol, sep, looping;
    char name[9];       /* the sound's lump, e.g. DSPISTOL */
    char hash[65];
} pix_event_t;

/* While on, Doom's sound calls are turned into events here instead of going to its own mixer (which the door can't
 * play anyway). Off (ANSI mode) they pass straight through. */
void pix_hooks_enable(bool on);

/* The frame rate IDRATE shows while the hooks are on: frames a second that reached the player (-1: the game's own) */
void pix_hooks_set_fps(int fps);

/* The next event, oldest first. False when there are none. Called from the door's thread. */
bool pix_hooks_next(pix_event_t *ev);

#endif
