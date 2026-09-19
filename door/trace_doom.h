/* trace_doom.h -- sending Doom to the player's terminal and starting it; see trace_doom.c. */

#ifndef TRACE_DOOM_H
#define TRACE_DOOM_H

#include <stdbool.h>
#include <stddef.h>

/* Reads doom.wasm and doom1.wad from beside the door binary. False means the door isn't installed properly. */
bool trace_doom_load_files(void);

/* How big the WAD is, for the "first time" message. */
size_t trace_doom_wad_size(void);

/* Does this terminal have TRACE, with everything Doom needs (its own module, assets and sound)? */
bool trace_doom_detect(void);

/* Makes sure the terminal has the WAD, uploading it if this is the player's first game. */
bool trace_doom_send_wad(void (*progress)(int));

/* Starts the game on the player's machine (uploading it the first time). */
bool trace_doom_open(void);

/* Waits until the player quits, or their time runs out. */
void trace_doom_wait(void);

/* Sends a message to the game: a line of text, and optionally a payload after it. */
void trace_doom_send(const char *head, const void *payload, size_t len);

/* Handles a message the game sent the door (a savegame request or piece). The ANSI mode, which runs the game
 * here on the BBS, calls this directly instead of it arriving over TRACE; reply is how a save goes back. */
void trace_doom_module_message(const unsigned char *data, size_t len,
                               void (*reply)(const char *head, const void *payload, size_t len));

/* The WAD's bytes and SHA-256, for the ANSI mode to play from. */
const unsigned char *trace_doom_wad_data(void);
const char *trace_doom_wad_hash(void);

/* Stops the game if it's still running. */
void trace_doom_close(void);

#endif
