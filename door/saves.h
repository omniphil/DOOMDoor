/* saves.h -- savegames kept on the BBS, one set per player; see saves.c. */

#ifndef SAVES_H
#define SAVES_H

#include <stddef.h>

/* Bytes of savegame per message. Base64 plus the TRACE wrapper has to stay under the terminal's 8 KB command limit. */
#define SAVES_CHUNK 3000

/* Picks the folder for this player (their handle and BBS user number, from the drop file) and makes sure it exists. */
void saves_init(const char *player, int user_number);

/* Tells the game which slots exist, sending only each save's description. */
void saves_send_list(void (*send)(const char *head, const void *payload, size_t len));

/* Sends one whole savegame, in pieces. */
void saves_send_slot(int slot, void (*send)(const char *head, const void *payload, size_t len));

/* Takes a piece of a save from the game. Returns 1 when the last piece completes the file. */
int saves_receive_chunk(int slot, size_t offset, size_t total, const unsigned char *data, size_t size);

#endif
