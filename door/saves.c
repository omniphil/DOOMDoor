/*
 * saves.c -- savegames, kept on the BBS, one set per player.
 *
 * The module has nowhere of its own to keep a save: it runs in a sandbox on the player's PC and its memory goes when
 * the call ends. So the game sends its saves here and asks for them back, which is also what a player expects from a
 * BBS door: the game follows them, not the machine they happened to call from.
 *
 * Files live in saves/<player>/doomsav<n>.dsg beside the door binary. The player's name comes from the BBS drop file,
 * cut down to plain characters so it can only ever name one file in that folder.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "saves.h"
#include "door.h"

#define MAX_SLOTS       80
#define MAX_SAVE_BYTES  (8 * 1024 * 1024)

/* Room for the door's own folder plus "/saves/<player>"; PATH_MAX all round makes the compiler fret about it. */
#define DIR_MAX 512
static char g_dir[DIR_MAX];
static char g_player[80];

/* An incoming save, while the game sends it up */
static unsigned char *g_incoming;
static size_t         g_incoming_size, g_incoming_total;
static int            g_incoming_slot = -1;

/* Where the door keeps things, which is beside its own binary rather than the current directory */
static void door_dir(char *out, size_t size)
{
    ssize_t len = readlink("/proc/self/exe", out, size - 1);
    char *slash;

    if (len <= 0)
    {
        snprintf(out, size, ".");
        return;
    }
    out[len] = '\0';
    slash = strrchr(out, '/');
    if (slash != NULL)
        *slash = '\0';
}

/*
 * A folder name for this player: their handle, plus the BBS's own user number.
 *
 * The handle alone isn't enough to tell two people apart, because cutting it down to plain characters can make two
 * different handles the same ("Phil" and "P.h.i.l" both become "phil"), and they would then share savegames. The user
 * number is unique on the board, so the two together can't collide. Cutting the handle down also means it can only
 * ever name a folder inside saves/, never anywhere else.
 */
void saves_init(const char *player, int user_number)
{
    char base[DIR_MAX - 128];
    char name[80];
    char handle[48];
    size_t n = 0;

    for (const char *p = player; *p != '\0' && n < sizeof(handle) - 1; p++)
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_')
            handle[n++] = (char)tolower((unsigned char)*p);
    handle[n] = '\0';
    if (n == 0)
        snprintf(handle, sizeof(handle), "player");

    if (user_number > 0)
        snprintf(name, sizeof(name), "%s-%d", handle, user_number);
    else
        snprintf(name, sizeof(name), "%s", handle);

    snprintf(g_player, sizeof(g_player), "%s", name);

    door_dir(base, sizeof(base));
    snprintf(g_dir, sizeof(g_dir), "%s/saves", base);
    mkdir(g_dir, 0755);
    snprintf(g_dir, sizeof(g_dir), "%s/saves/%s", base, name);
    mkdir(g_dir, 0755);
}

/* Which player's saves these are, for the door to show. */
const char *saves_player(void)
{
    return g_player;
}

/* The player's own folder, for anything else the door keeps per player (their display choice). */
const char *saves_folder(void)
{
    return g_dir;
}

static void slot_path(int slot, char *out, size_t size)
{
    snprintf(out, size, "%s/doomsav%d.dsg", g_dir, slot);
}

/*
 * Tells the game which slots this player has, sending only each save's 24-byte description. That's what Doom's Load
 * menu shows, so the menu is right immediately and nothing else has to travel until a save is actually opened.
 */
void saves_send_list(void (*send)(const char *head, const void *payload, size_t len))
{
    for (int slot = 0; slot < MAX_SLOTS; slot++)
    {
        char path[DIR_MAX + 32], head[64];
        unsigned char description[24];
        FILE *fp;
        size_t got;

        slot_path(slot, path, sizeof(path));
        fp = fopen(path, "rb");
        if (fp == NULL)
            continue;
        got = fread(description, 1, sizeof(description), fp);
        fclose(fp);
        if (got != sizeof(description))
            continue;

        snprintf(head, sizeof(head), "list slot=%d", slot);
        send(head, description, sizeof(description));
    }
}

/* The game asked for a save: send it in pieces the terminal can carry. */
void saves_send_slot(int slot, void (*send)(const char *head, const void *payload, size_t len))
{
    char path[DIR_MAX + 32], head[64];
    unsigned char *data;
    long size;
    FILE *fp;

    if (slot < 0 || slot >= MAX_SLOTS)
        return;
    slot_path(slot, path, sizeof(path));
    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        snprintf(head, sizeof(head), "none slot=%d", slot);
        send(head, NULL, 0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = size > 0 && size <= MAX_SAVE_BYTES ? malloc((size_t)size) : NULL;
    if (data == NULL || fread(data, 1, (size_t)size, fp) != (size_t)size)
    {
        free(data);
        fclose(fp);
        snprintf(head, sizeof(head), "none slot=%d", slot);
        send(head, NULL, 0);
        return;
    }
    fclose(fp);

    for (long off = 0; off < size; off += SAVES_CHUNK)
    {
        long take = size - off < SAVES_CHUNK ? size - off : SAVES_CHUNK;
        snprintf(head, sizeof(head), "data slot=%d off=%ld total=%ld", slot, off, size);
        send(head, data + off, (size_t)take);
    }
    free(data);
}

/* A piece of a save on its way up from the game. Returns 1 once the whole thing has been written to disk. */
int saves_receive_chunk(int slot, size_t offset, size_t total, const unsigned char *data, size_t size)
{
    if (slot < 0 || slot >= MAX_SLOTS || total == 0 || total > MAX_SAVE_BYTES)
        return 0;

    if (g_incoming == NULL || g_incoming_slot != slot || g_incoming_total != total)
    {
        free(g_incoming);
        g_incoming = malloc(total);
        g_incoming_total = total;
        g_incoming_size = 0;
        g_incoming_slot = slot;
    }
    if (g_incoming == NULL || offset + size > total)
        return 0;

    /* The game sends a savegame in order, so a piece arriving out of turn means one was lost. Dropping the lot is
     * better than writing a savegame with a hole in it, which is what happened before this check existed. */
    if (offset != g_incoming_size)
    {
        g_incoming_size = 0;
        return 0;
    }
    memcpy(g_incoming + offset, data, size);
    g_incoming_size += size;
    if (g_incoming_size < total)
        return 0;

    /* All of it is here: write it out, through a temporary file so a dropped call can't leave half a save behind */
    {
        char path[DIR_MAX + 32], temp[DIR_MAX + 48];
        FILE *fp;

        slot_path(slot, path, sizeof(path));
        snprintf(temp, sizeof(temp), "%s.part", path);
        fp = fopen(temp, "wb");
        if (fp != NULL)
        {
            int ok = fwrite(g_incoming, 1, total, fp) == total;
            fclose(fp);
            if (ok)
                rename(temp, path);
            else
                remove(temp);
        }
    }
    free(g_incoming);
    g_incoming = NULL;
    g_incoming_slot = -1;
    g_incoming_size = g_incoming_total = 0;
    return 1;
}
