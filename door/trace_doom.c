/*
 * trace_doom.c -- the TRACE side of the Doom door: sending the game to the player's terminal and starting it.
 *
 * The door carries two files next to its binary:
 *   doom.wasm   the whole game, built from ../module (renderer, sound and all)
 *   doom1.wad   the shareware IWAD id Software gave away, which may be passed on unchanged
 *
 * Both go to TERMinator once, which caches them by SHA-256; every call after that starts straight away. Then the
 * module is opened over the whole screen with the keyboard, and the door waits until the player quits.
 */

#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "trace_doom.h"
#include "door.h"
#include "saves.h"
#include "sha256.h"

#define APC "\033_TERMinator:TRACE;"
#define ST  "\033\\"

#define MODULE_FILE  "doom.wasm"
#define WAD_FILE     "doom1.wad"
#define CHUNK_BYTES  3072   /* 4096 base64 characters per Put, well under TERMinator's 8 KB limit per command */

typedef struct {
    uint8_t *data;
    size_t   size;
    char     hash[65];
} blob_t;

static blob_t module_blob, wad_blob;
static bool   is_open = false;

/* ---- reading the files that ship with the door ---- */

static bool load_blob(const char *filename, blob_t *blob, size_t max_size)
{
    char path[PATH_MAX];
    FILE *fp = NULL;

    /* next to the door binary first, then the current directory */
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 64);
    if (len > 0) {
        path[len] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", filename);
            fp = fopen(path, "rb");
        }
    }
    if (!fp) fp = fopen(filename, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size > 0 && (size_t)size <= max_size) {
        blob->data = (uint8_t *)malloc((size_t)size);
        if (blob->data && fread(blob->data, 1, (size_t)size, fp) == (size_t)size) {
            blob->size = (size_t)size;
            sha256_hex(blob->data, blob->size, blob->hash);
        } else {
            free(blob->data);
            blob->data = NULL;
        }
    }
    fclose(fp);
    return blob->data != NULL;
}

bool trace_doom_load_files(void)
{
    return load_blob(MODULE_FILE, &module_blob, 16 * 1024 * 1024)
        && load_blob(WAD_FILE, &wad_blob, 64 * 1024 * 1024);
}

size_t trace_doom_wad_size(void)
{
    return wad_blob.size;
}

/* ---- talking to TERMinator ---- */

bool trace_doom_detect(void)
{
    char reply[512];
    int n = 0;

    door_write(APC "Query" ST);

    /* Up to half a second for the reply to start, then read it to its closing ESC \ so none of it is later
     * mistaken for keypresses. Terminals without TRACE never answer at all. */
    int c = door_read_char_timeout(500);
    while (c >= 0 && n < (int)sizeof(reply) - 1) {
        reply[n++] = (char)c;
        if (n >= 2 && reply[n - 2] == '\033' && reply[n - 1] == '\\') break;
        c = door_read_char_timeout(250);
    }
    reply[n] = '\0';

    /* Doom needs all three: a module of its own, the WAD as an asset, and sound */
    return strstr(reply, "TERMinator:TRACE") != NULL
        && strstr(reply, "wasm=1") != NULL
        && strstr(reply, "assets=1") != NULL
        && strstr(reply, "audio=1") != NULL;
}

/* base64 back to bytes, for what the module sends up. Returns the length, or 0 if it isn't valid base64. */
static size_t decode_base64(const char *text, unsigned char *out, size_t max)
{
    static signed char table[256];
    static bool ready = false;
    size_t len = 0;
    uint32_t bits = 0;
    int have = 0;

    if (!ready)
    {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        memset(table, -1, sizeof(table));
        for (int i = 0; i < 64; i++)
            table[(unsigned char)b64[i]] = (signed char)i;
        ready = true;
    }

    for (const char *p = text; *p != '\0'; p++)
    {
        signed char value;
        if (*p == '=')
            break;
        value = table[(unsigned char)*p];
        if (value < 0)
            continue;              /* whitespace and anything else is skipped */
        bits = (bits << 6) | (uint32_t)value;
        have += 6;
        if (have >= 8)
        {
            have -= 8;
            if (len >= max)
                return 0;
            out[len++] = (unsigned char)((bits >> have) & 0xFF);
        }
    }
    return len;
}

/*
 * A message from the game itself (it can only talk to us):
 *   get slot=<n>                          send this player's save back
 *   put slot=<n> off=<o> total=<t>\n...   a piece of a save to keep for them
 */
static void handle_module_message(const unsigned char *data, size_t len)
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
        saves_send_slot(slot, trace_doom_send);
    else if (!strncmp(head, "put", 3) && payload != NULL)
        saves_receive_chunk(slot, (size_t)off, (size_t)total, payload, payload_len);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Waits for TERMinator's next answer about our module, ignoring anything else.
 * Returns 'R' Ready, 'N' Need (send the module), 'H' Have (the asset is cached), 'A' NeedAsset, 'C' Closed, 0 nothing.
 */
static int wait_reply(int timeout_ms)
{
    static char buf[SAVES_CHUNK * 2 + 512];   /* big enough for a savegame piece coming back up, base64 and all */
    int n = 0, prev = -1;
    bool in_apc = false;
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        int c = door_read_char_timeout((int)left);
        if (c < 0) return 0;

        if (prev == 27 && c == '_') {
            in_apc = true;
            n = 0;
        } else if (in_apc && prev == 27 && c == '\\') {
            in_apc = false;
            if (n > 0) n--;            /* drop the ESC */
            buf[n] = '\0';
            {
                /* What the game sent up, relayed by TERMinator as base64 */
                const char *b64 = strstr(buf, "TERMinator:TRACE;Data;module=doom;b64=");
                if (b64 != NULL)
                {
                    static unsigned char message[SAVES_CHUNK + 4096];   /* room to spare: a piece too big to decode would be lost */
                    size_t got = decode_base64(b64 + strlen("TERMinator:TRACE;Data;module=doom;b64="),
                                               message, sizeof(message));
                    if (got > 0)
                        handle_module_message(message, got);
                    continue;
                }
            }
            if (strstr(buf, "TERMinator:TRACE;Ready;module=doom")) return 'R';
            if (strstr(buf, "TERMinator:TRACE;Need;module=doom")) return 'N';
            if (strstr(buf, "TERMinator:TRACE;Have;module=doom")) return 'H';
            if (strstr(buf, "TERMinator:TRACE;NeedAsset;module=doom")) return 'A';
            if (strstr(buf, "TERMinator:TRACE;Closed;module=doom")) return 'C';
        } else if (in_apc && n < (int)sizeof(buf) - 1) {
            buf[n++] = (char)c;
        }
        prev = c;
    }
}

/* Sends one blob in Put chunks. asset_hash is NULL for the module itself. progress is called with 0-100. */
static void upload(const blob_t *blob, const char *asset_hash, void (*progress)(int))
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char head[128];
    char *line = (char *)malloc(CHUNK_BYTES / 3 * 4 + 8);
    int last_percent = -1;

    if (!line) return;
    for (size_t off = 0; off < blob->size; off += CHUNK_BYTES) {
        size_t len = blob->size - off < CHUNK_BYTES ? blob->size - off : CHUNK_BYTES;
        const uint8_t *p = blob->data + off;
        size_t o = 0;
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)p[i] << 16;
            if (i + 1 < len) v |= (uint32_t)p[i + 1] << 8;
            if (i + 2 < len) v |= p[i + 2];
            line[o++] = b64[(v >> 18) & 63];
            line[o++] = b64[(v >> 12) & 63];
            line[o++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
            line[o++] = i + 2 < len ? b64[v & 63] : '=';
        }
        line[o] = '\0';
        if (asset_hash)
            snprintf(head, sizeof(head), APC "Put;module=doom;asset=%s;offset=%zu;data=", asset_hash, off);
        else
            snprintf(head, sizeof(head), APC "Put;module=doom;offset=%zu;data=", off);
        door_write(head);
        door_write(line);
        door_write(ST);

        if (progress) {
            int percent = (int)((off + len) * 100 / blob->size);
            if (percent != last_percent) {
                progress(percent);
                last_percent = percent;
            }
        }
    }
    free(line);

    if (asset_hash) {
        snprintf(head, sizeof(head), APC "PutDone;module=doom;asset=%s" ST, asset_hash);
        door_write(head);
    } else {
        door_write(APC "PutDone;module=doom" ST);
    }
}

/*
 * Sends a message to the module: a line of text, and optionally a payload after it. TERMinator hands the module
 * exactly these bytes, so a savegame travels as itself rather than as text.
 */
void trace_doom_send(const char *head, const void *payload, size_t len)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t head_len = strlen(head);
    size_t total = head_len + (payload != NULL ? 1 + len : 0);
    unsigned char *message = malloc(total);
    char *encoded = malloc(total / 3 * 4 + 8);

    if (message == NULL || encoded == NULL)
    {
        free(message);
        free(encoded);
        return;
    }
    memcpy(message, head, head_len);
    if (payload != NULL)
    {
        message[head_len] = '\n';
        memcpy(message + head_len + 1, payload, len);
    }
    {
        size_t o = 0;
        for (size_t i = 0; i < total; i += 3)
        {
            uint32_t v = (uint32_t)message[i] << 16;
            if (i + 1 < total) v |= (uint32_t)message[i + 1] << 8;
            if (i + 2 < total) v |= message[i + 2];
            encoded[o++] = b64[(v >> 18) & 63];
            encoded[o++] = b64[(v >> 12) & 63];
            encoded[o++] = i + 1 < total ? b64[(v >> 6) & 63] : '=';
            encoded[o++] = i + 2 < total ? b64[v & 63] : '=';
        }
        encoded[o] = '\0';
    }
    door_write(APC "Data;module=doom;b64=");
    door_write(encoded);
    door_write(ST);
    free(message);
    free(encoded);
}

/* The WAD: offered by hash, uploaded only if this player hasn't had it before. */
bool trace_doom_send_wad(void (*progress)(int))
{
    char buf[160];
    int reply;

    snprintf(buf, sizeof(buf), APC "Asset;module=doom;sha256=%s;size=%zu" ST, wad_blob.hash, wad_blob.size);
    door_write(buf);

    reply = wait_reply(5000);
    if (reply == 'H') return true;          /* already cached from an earlier call */
    if (reply != 'A') return false;

    upload(&wad_blob, wad_blob.hash, progress);
    return wait_reply(60000) == 'H';
}

bool trace_doom_open(void)
{
    char buf[256];
    int reply;

    snprintf(buf, sizeof(buf), APC "Open;module=doom;wasm=%s;size=%zu;exclusive=1" ST,
             module_blob.hash, module_blob.size);
    door_write(buf);

    reply = wait_reply(5000);
    if (reply == 'N') {
        /* First time on this PC: send the game, TERMinator checks its hash, caches it and starts it */
        upload(&module_blob, NULL, NULL);
        reply = wait_reply(30000);
    }
    if (reply != 'R') return false;

    /* Now the module knows which WAD to play, and the game starts */
    snprintf(buf, sizeof(buf), APC "Data;module=doom;wad=%s" ST, wad_blob.hash);
    door_write(buf);

    /* And which saved games this player has, so Doom's Load menu is right from the start. Only the descriptions
     * travel now; a save itself is only sent if the player opens it. */
    saves_send_list(trace_doom_send);

    is_open = true;
    return true;
}

/* Waits until the player quits Doom (TERMinator sends Closed), or the call runs out of time. */
void trace_doom_wait(void)
{
    while (is_open) {
        if (wait_reply(2000) == 'C')
            break;
        if (door_time_remaining() <= 0)
            break;
    }
    is_open = false;
}

void trace_doom_close(void)
{
    if (!is_open) return;
    door_write(APC "Close;module=doom" ST);
    is_open = false;
}
