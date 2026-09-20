/*
 * trace_doom.c -- the TRACE side of the Doom door: sending the game to the player's terminal and starting it.
 *
 * The door carries two files next to its binary:
 *   doom.wasm   the whole game, built from ../module (renderer, sound and all)
 *   doom1.wad   the shareware IWAD id Software gave away, which may be passed on unchanged
 *
 * Both go to TERMinator once, which caches them by SHA-256; every call after that starts straight away. Then the
 * module is opened over the whole screen with the keyboard, and the door waits until the player quits. The talking
 * itself (detection, uploads, messages, binary frames) is the shared door library, trace_door.c.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "trace_doom.h"
#include "trace_door.h"
#include "door.h"
#include "saves.h"

#define MODULE_FILE  "doom.wasm"
#define WAD_FILE     "doom1.wad"

static tdoor_blob_t module_blob, wad_blob;

static void on_module_message(const unsigned char *data, size_t len)
{
    trace_doom_module_message(data, len, trace_doom_send);
}

bool trace_doom_load_files(void)
{
    tdoor_init("doom", on_module_message);
    return tdoor_load_blob(MODULE_FILE, &module_blob, 16 * 1024 * 1024)
        && tdoor_load_blob(WAD_FILE, &wad_blob, 64 * 1024 * 1024);
}

size_t trace_doom_wad_size(void)
{
    return wad_blob.size;
}

const unsigned char *trace_doom_wad_data(void)
{
    return wad_blob.data;
}

const char *trace_doom_wad_hash(void)
{
    return wad_blob.hash;
}

bool trace_doom_detect(void)
{
    /* Doom needs all three: a module of its own, the WAD as an asset, and sound */
    static const char *const need[] = { "assets=1", "audio=1", NULL };
    tdoor_init("doom", on_module_message);
    return tdoor_detect(need);
}

/*
 * A message from the game itself (it can only talk to us):
 *   get slot=<n>                          send this player's save back
 *   put slot=<n> off=<o> total=<t>\n...   a piece of a save to keep for them
 */
void trace_doom_module_message(const unsigned char *data, size_t len,
                               void (*reply)(const char *head, const void *payload, size_t len))
{
    char head[128];
    const unsigned char *payload = NULL;
    size_t payload_len = 0, head_len;
    const unsigned char *newline = memchr(data, '\n', len);
    int slot = -1;
    long off = 0, total = 0;

    head_len = newline != NULL ? (size_t)(newline - data) : len;
    if (head_len >= sizeof(head))
        head_len = sizeof(head) - 1;
    memcpy(head, data, head_len);
    head[head_len] = '\0';
    if (newline != NULL)
    {
        payload = newline + 1;
        payload_len = len - (size_t)(payload - data);
    }

    if (strstr(head, "slot=")) sscanf(strstr(head, "slot="), "slot=%d", &slot);
    if (strstr(head, "off=")) sscanf(strstr(head, "off="), "off=%ld", &off);
    if (strstr(head, "total=")) sscanf(strstr(head, "total="), "total=%ld", &total);

    if (!strncmp(head, "get", 3))
        saves_send_slot(slot, reply);
    else if (!strncmp(head, "put", 3) && payload != NULL)
        saves_receive_chunk(slot, (size_t)off, (size_t)total, payload, payload_len);
}

/*
 * Sends a message to the module: a line of text, and optionally a payload after it. TERMinator hands the module
 * exactly these bytes, so a savegame travels as itself rather than as text.
 */
void trace_doom_send(const char *head, const void *payload, size_t len)
{
    tdoor_send(head, payload, len);
}

/* The WAD: offered by hash, uploaded only if this player hasn't had it before. */
bool trace_doom_send_wad(void (*progress)(int))
{
    return tdoor_send_asset(&wad_blob, progress);
}

bool trace_doom_open(void)
{
    char buf[128];

    if (!tdoor_open(&module_blob, "exclusive=1", NULL))
        return false;

    /* Now the module knows which WAD to play, and the game starts */
    snprintf(buf, sizeof(buf), "wad=%s", wad_blob.hash);
    tdoor_send_text(buf);

    /* And which saved games this player has, so Doom's Load menu is right from the start. Only the descriptions
     * travel now; a save itself is only sent if the player opens it. */
    saves_send_list(trace_doom_send);
    return true;
}

/* Waits until the player quits Doom (TERMinator sends Closed), or the call runs out of time. */
void trace_doom_wait(void)
{
    tdoor_wait_closed();
}

void trace_doom_close(void)
{
    tdoor_close(NULL, 0);
}
