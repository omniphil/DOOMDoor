/*
 * ansi_host.h -- runs the Doom module here on the BBS, for players without TRACE. See ansi_host.c.
 */

#ifndef ANSI_HOST_H
#define ANSI_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts Doom on its own thread, playing the WAD the door loaded, with this player's savegames. */
bool ansi_host_start(const unsigned char *wad, size_t wad_size, const char *wad_hash);

/* Copies the newest frame into *pixels (reallocated to fit) when there is one newer than *seq. BGRA, width x height,
 * shown at 4:3. Returns false when nothing new has been drawn. */
bool ansi_host_frame(uint32_t **pixels, int *width, int *height, unsigned *seq);

/* A key going down or up, as a set-1 scancode (what Doom's input layer takes). */
void ansi_host_key(int scancode, bool down);

/* True once Doom has quit (the player chose Quit Game, or a fatal error). */
bool ansi_host_finished(void);

#endif
