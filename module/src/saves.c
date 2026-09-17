/*
 * saves.c -- savegames that belong to the player, not to the PC they called from.
 *
 * The module has nowhere to keep a savegame: it lives in the sandbox and its memory disappears when the call ends. So
 * saves go to the BBS through the door, and come back the same way, which also means a player can carry a game
 * between calls and between machines.
 *
 * How it works:
 *   - When the game starts, the door sends the first 24 bytes of each save it holds for this player. That's the
 *     description Doom shows in its Load menu, so the menu is filled in straight away with nothing else transferred.
 *   - Opening one of those saves fetches the rest, and the game waits the second or so that takes.
 *   - Saving sends the file up in the background, a few KB per frame, so the game doesn't stall.
 *
 * All of it goes through the door: a module can't reach anything else.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_api.h"
#include "tracedoom.h"

/* Doom has 8 pages of 8 slots; the file is doomsav<n>.dsg where n is 10 * page + slot. */
#define MAX_SLOTS   80
#define HEADER_SIZE 24            /* SAVESTRINGSIZE: the description Doom's menu shows */
/* Bytes of savegame per message. This must match SAVES_CHUNK in the door (door/saves.h): a bigger piece than the
 * door can decode is dropped silently at the other end, which is how the first savegames arrived corrupted. */
#define CHUNK       3000

typedef struct
{
    unsigned char  header[HEADER_SIZE];
    int            has_header;     /* the door says this slot exists */
    int            header_applied; /* and the game thread has put it where Doom will look */

    unsigned char *incoming;       /* the full save, while the door sends it */
    size_t         incoming_size, incoming_total;
    int            complete;
} slot_t;

static slot_t          g_slots[MAX_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* What we're sending the door, a few KB at a time */
static unsigned char *g_outgoing;
static size_t         g_outgoing_size, g_outgoing_sent;
static int            g_outgoing_slot;

/* Doom's own name for a slot's file (p_saveg.c). Only safe to call on the game thread, once the game is running. */
extern char *P_SaveGameFile(int slot);

/* doomsav<n>.dsg -> n, or -1 for anything else (Doom's temp.dsg, the config, ...) */
static int slot_from_name(const char *name)
{
    const char *base = strrchr(name, '/');
    int slot = -1;

    base = base != NULL ? base + 1 : name;
    if (sscanf(base, "doomsav%d.dsg", &slot) != 1)
        return -1;
    return slot >= 0 && slot < MAX_SLOTS ? slot : -1;
}

/* ---- what the door tells us (TERMinator's thread) ---- */

void tracedoom_saves_on_list(int slot, const unsigned char *header, size_t size)
{
    if (slot < 0 || slot >= MAX_SLOTS || size == 0)
        return;
    pthread_mutex_lock(&g_lock);
    memset(g_slots[slot].header, ' ', HEADER_SIZE);
    memcpy(g_slots[slot].header, header, size < HEADER_SIZE ? size : HEADER_SIZE);
    g_slots[slot].has_header = 1;
    g_slots[slot].header_applied = 0;
    pthread_mutex_unlock(&g_lock);
}

void tracedoom_saves_on_data(int slot, size_t offset, size_t total, const unsigned char *data, size_t size)
{
    if (slot < 0 || slot >= MAX_SLOTS || total == 0 || total > 8 * 1024 * 1024)
        return;

    pthread_mutex_lock(&g_lock);
    {
        slot_t *s = &g_slots[slot];
        if (s->incoming == NULL || s->incoming_total != total)
        {
            free(s->incoming);
            s->incoming = (unsigned char *)malloc(total);
            s->incoming_total = total;
            s->incoming_size = 0;
            s->complete = 0;
        }
        /* Pieces are sent in order, so anything out of order means one went missing: start again rather than
         * quietly leaving a hole in the middle of a savegame. */
        if (s->incoming != NULL && offset == s->incoming_size && offset + size <= total)
        {
            memcpy(s->incoming + offset, data, size);
            s->incoming_size += size;
            if (s->incoming_size >= total)
                s->complete = 1;
        }
        else if (s->incoming != NULL && offset != s->incoming_size)
        {
            tracedoom_log("doom: a piece of the savegame went missing; asking again");
            s->incoming_size = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* The door has no such save after all, so stop waiting for it. */
void tracedoom_saves_on_none(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS)
        return;
    pthread_mutex_lock(&g_lock);
    g_slots[slot].complete = 1;
    g_slots[slot].incoming_total = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---- what the game thread does ---- */

/* Fills Doom's Load menu with the slots the door has, as soon as the game is far enough along to name the files. */
static void apply_headers(void)
{
    for (int slot = 0; slot < MAX_SLOTS; slot++)
    {
        unsigned char header[HEADER_SIZE];
        int apply = 0;

        pthread_mutex_lock(&g_lock);
        if (g_slots[slot].has_header && !g_slots[slot].header_applied)
        {
            memcpy(header, g_slots[slot].header, HEADER_SIZE);
            g_slots[slot].header_applied = 1;
            apply = 1;
        }
        pthread_mutex_unlock(&g_lock);

        if (apply)
            tracedoom_ramfs_put(P_SaveGameFile(slot), header, HEADER_SIZE, 1);
    }
}

/*
 * Doom is about to read a savegame we only have the description of. Ask the door for it and wait: this is the one
 * place the game pauses, and on a normal link it's about a second.
 */
void tracedoom_fetch_save(const char *name)
{
    int slot = slot_from_name(name);
    char request[64];
    int waited = 0;

    if (slot < 0)
        return;

    pthread_mutex_lock(&g_lock);
    g_slots[slot].complete = 0;
    g_slots[slot].incoming_size = 0;
    g_slots[slot].incoming_total = 0;
    pthread_mutex_unlock(&g_lock);

    {
        int n = snprintf(request, sizeof(request), "get slot=%d", slot);
        while (trace_send(request, n) == 0 && waited < 2000)
        {
            tracedoom_sleep_ms(10);
            waited += 10;
        }
    }

    /* Up to 30 seconds, which is far longer than any save takes even on a slow link */
    while (waited < 30000)
    {
        int done;
        pthread_mutex_lock(&g_lock);
        done = g_slots[slot].complete;
        pthread_mutex_unlock(&g_lock);
        if (done)
            break;
        tracedoom_sleep_ms(10);
        waited += 10;
    }

    pthread_mutex_lock(&g_lock);
    if (g_slots[slot].incoming != NULL && g_slots[slot].incoming_size >= g_slots[slot].incoming_total
        && g_slots[slot].incoming_total > 0)
    {
        tracedoom_ramfs_put(name, g_slots[slot].incoming, g_slots[slot].incoming_total, 0);
        free(g_slots[slot].incoming);
        g_slots[slot].incoming = NULL;
        g_slots[slot].incoming_total = g_slots[slot].incoming_size = 0;
    }
    pthread_mutex_unlock(&g_lock);

    /* Whatever happened, don't ask again on the next read: a save that never arrived stays as it is */
    tracedoom_ramfs_complete(name);
}

/* A file was written: if it's a savegame, start sending it to the door. */
void tracedoom_file_written(const char *name)
{
    int slot = slot_from_name(name);
    unsigned char *data = NULL;
    size_t size;

    if (slot < 0)
        return;
    size = tracedoom_ramfs_read_all(name, &data);
    if (size == 0)
        return;

    free(g_outgoing);
    g_outgoing = data;
    g_outgoing_size = size;
    g_outgoing_sent = 0;
    g_outgoing_slot = slot;
    tracedoom_log("doom: saving slot %d to the BBS (%zu bytes)", slot, size);
}

/*
 * Called once a frame. Puts the headers Doom's menu needs in place, and sends a little more of any save that's on its
 * way up, as much as the link will take.
 */
void tracedoom_saves_update(void)
{
    apply_headers();

    while (g_outgoing != NULL && g_outgoing_sent < g_outgoing_size)
    {
        char message[CHUNK + 64];
        size_t left = g_outgoing_size - g_outgoing_sent;
        size_t take = left < CHUNK ? left : CHUNK;
        int header;

        if ((size_t)trace_send_room() < take + 64)
            return;             /* the link is busy; the rest goes out next frame */

        header = snprintf(message, sizeof(message), "put slot=%d off=%zu total=%zu\n",
                          g_outgoing_slot, g_outgoing_sent, g_outgoing_size);
        memcpy(message + header, g_outgoing + g_outgoing_sent, take);
        if (trace_send(message, (int32_t)(header + take)) <= 0)
            return;
        g_outgoing_sent += take;
    }

    if (g_outgoing != NULL && g_outgoing_sent >= g_outgoing_size)
    {
        free(g_outgoing);
        g_outgoing = NULL;
        tracedoom_log("doom: save sent");
    }
}
