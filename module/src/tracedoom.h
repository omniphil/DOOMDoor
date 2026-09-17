/*
 * tracedoom.h -- what the module's own files share: events from TERMinator, the WAD the door sent, logging.
 * Nothing in Crispy Doom includes this.
 */

#ifndef TRACEDOOM_H
#define TRACEDOOM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The name Doom sees for the door's IWAD; the real file is fetched by hash (w_file_trace.c). */
#define TRACEDOOM_IWAD_NAME "doom1.wad"

/* One input record from TERMinator (engine contract TE_IN_*). */
typedef struct
{
    int32_t type, flags, a, b, c;
} trace_event_t;

/* Takes the next event, or returns 0 when there are none. Called from the game thread. */
int tracedoom_next_event(trace_event_t *out);

/* The SHA-256 of the WAD the door sent, or NULL before it has sent one. */
const char *tracedoom_wad_hash(void);

/* A line for TERMinator's debug output. */
void tracedoom_log(const char *fmt, ...);

/* Mixes the next frames of Doom's sound (i_sound_trace.c) and hands them to TERMinator. */
void tracedoom_pump_audio(void);

/* Sleeps, without spinning a core. */
void tracedoom_sleep_ms(int ms);

/* ---- the in-memory files (ramfs.c) ---- */
void   tracedoom_ramfs_put(const char *name, const void *data, size_t size, int partial);
size_t tracedoom_ramfs_append(const char *name, size_t offset, const void *data, size_t size);
void   tracedoom_ramfs_complete(const char *name);
int    tracedoom_ramfs_is_partial(const char *name);
size_t tracedoom_ramfs_read_all(const char *name, unsigned char **out);

/* ---- savegames, which live on the BBS (saves.c) ---- */

/* The door has a save in this slot; the payload is the description Doom's menu shows. */
void tracedoom_saves_on_list(int slot, const unsigned char *header, size_t size);
/* Part of a save the door is sending. */
void tracedoom_saves_on_data(int slot, size_t offset, size_t total, const unsigned char *data, size_t size);
/* The door has no save in that slot after all. */
void tracedoom_saves_on_none(int slot);
/* Doom wants to read a save we only have the description of: fetch it and wait. */
void tracedoom_fetch_save(const char *name);
/* A file finished being written; a savegame goes to the door from here. */
void tracedoom_file_written(const char *name);
/* Once a frame: fill in the Load menu, and send a little more of any save on its way up. */
void tracedoom_saves_update(void);

#endif
