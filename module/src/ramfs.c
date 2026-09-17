/*
 * ramfs.c -- a few files kept in memory, because the sandbox has no file system.
 *
 * Doom writes and reads real files in a handful of places: it converts each MUS track to a temporary MIDI file before
 * the OPL player loads it, it saves games, and it keeps a config file. Rather than change any of that, the module
 * redirects fopen and friends here (include/tracedoom_compat.h) and keeps those files in memory.
 *
 * It is not a file system in any real sense: a fixed number of files, named by whatever Doom calls them, no folders,
 * and everything disappears when the session ends. Savegames that outlive a call belong on the BBS and will go
 * through the door instead. Nothing here can reach the player's PC.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracedoom.h"

#define MAX_FILES 12
#define MAX_NAME  128

typedef struct
{
    char           name[MAX_NAME];
    unsigned char *data;
    size_t         size, capacity, pos;
    int            open;
    int            used;
    int            written;    /* changed since it was opened: worth sending to the door on close */
    int            partial;    /* only the header the door sent; the rest has to be fetched before it can be read */
} ram_file_t;

static ram_file_t g_files[MAX_FILES];

/* Doom runs on its own thread while TERMinator's thread delivers what the door sends, and both touch these files. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static ram_file_t *find(const char *name)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (g_files[i].used && !strcmp(g_files[i].name, name))
            return &g_files[i];
    return NULL;
}

static ram_file_t *create(const char *name)
{
    for (int i = 0; i < MAX_FILES; i++)
    {
        if (!g_files[i].used)
        {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            snprintf(g_files[i].name, sizeof(g_files[i].name), "%s", name);
            g_files[i].used = 1;
            return &g_files[i];
        }
    }
    return NULL;
}

/* True when this is one of ours rather than stdout or stderr. */
static ram_file_t *as_ram(void *stream)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (stream == (void *)&g_files[i])
            return &g_files[i];
    return NULL;
}

static int grow(ram_file_t *f, size_t needed)
{
    if (needed <= f->capacity)
        return 1;
    {
        size_t capacity = f->capacity ? f->capacity : 4096;
        unsigned char *data;
        while (capacity < needed)
            capacity *= 2;
        data = realloc(f->data, capacity);
        if (data == NULL)
            return 0;
        f->data = data;
        f->capacity = capacity;
    }
    return 1;
}

/* Is this stream one of our in-memory files, rather than stdout or stderr? */
int tracedoom_is_ram_file(void *stream)
{
    return as_ram(stream) != NULL;
}

void *tracedoom_fopen(const char *name, const char *mode)
{
    ram_file_t *f;
    int writing = mode != NULL && (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL);

    /* A savegame the door only sent the header of: fetch the rest before Doom starts reading it. This waits, so it
     * must happen outside the lock and before we hand the file over. */
    if (!writing)
    {
        int partial;
        pthread_mutex_lock(&g_lock);
        f = find(name);
        partial = f != NULL && f->partial;
        pthread_mutex_unlock(&g_lock);
        if (partial)
            tracedoom_fetch_save(name);
    }

    pthread_mutex_lock(&g_lock);
    f = find(name);
    if (f == NULL)
    {
        if (!writing)
        {
            pthread_mutex_unlock(&g_lock);
            return NULL;         /* reading something that was never written */
        }
        f = create(name);
        if (f == NULL)
        {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
    }
    if (writing && strchr(mode, 'w') != NULL)
    {
        f->size = 0;             /* truncate, as "w" does */
        f->partial = 0;
    }
    f->pos = (mode != NULL && strchr(mode, 'a') != NULL) ? f->size : 0;
    f->open = 1;
    f->written = 0;
    pthread_mutex_unlock(&g_lock);
    return f;
}

int tracedoom_fclose(void *stream)
{
    ram_file_t *f = as_ram(stream);
    char name[MAX_NAME];
    int written = 0;

    if (f == NULL)
        return 0;
    pthread_mutex_lock(&g_lock);
    f->open = 0;
    written = f->written;
    f->written = 0;
    memcpy(name, f->name, sizeof(name));
    pthread_mutex_unlock(&g_lock);

    if (written)
        tracedoom_file_written(name);   /* a savegame goes to the door from here */
    return 0;
}

size_t tracedoom_fread(void *buffer, size_t size, size_t count, void *stream)
{
    ram_file_t *f = as_ram(stream);
    size_t want, have;

    if (f == NULL || size == 0)
        return 0;
    want = size * count;
    have = f->size > f->pos ? f->size - f->pos : 0;
    if (want > have)
        want = have - (have % size);
    memcpy(buffer, f->data + f->pos, want);
    f->pos += want;
    return want / size;
}

size_t tracedoom_fwrite(const void *buffer, size_t size, size_t count, void *stream)
{
    ram_file_t *f = as_ram(stream);
    size_t bytes = size * count;

    if (f == NULL)
        return count;            /* writing to stdout/stderr: the log has it already */
    if (size == 0 || !grow(f, f->pos + bytes))
        return 0;
    memcpy(f->data + f->pos, buffer, bytes);
    f->pos += bytes;
    if (f->pos > f->size)
        f->size = f->pos;
    f->written = 1;
    return count;
}

int tracedoom_fseek(void *stream, long offset, int whence)
{
    ram_file_t *f = as_ram(stream);
    long target;

    if (f == NULL)
        return -1;
    target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (long)f->pos + offset : (long)f->size + offset;
    if (target < 0)
        return -1;
    f->pos = (size_t)target;
    return 0;
}

long tracedoom_ftell(void *stream)
{
    ram_file_t *f = as_ram(stream);
    return f != NULL ? (long)f->pos : -1;
}

int tracedoom_feof(void *stream)
{
    ram_file_t *f = as_ram(stream);
    return f != NULL ? (f->pos >= f->size) : 1;
}

int tracedoom_fgetc(void *stream)
{
    ram_file_t *f = as_ram(stream);
    if (f == NULL || f->pos >= f->size)
        return -1;
    return f->data[f->pos++];
}

int tracedoom_fputc(int c, void *stream)
{
    unsigned char byte = (unsigned char)c;
    return tracedoom_fwrite(&byte, 1, 1, stream) == 1 ? c : -1;
}

char *tracedoom_fgets(char *buffer, int size, void *stream)
{
    ram_file_t *f = as_ram(stream);
    int i = 0;

    if (f == NULL || size <= 1 || f->pos >= f->size)
        return NULL;
    while (i < size - 1 && f->pos < f->size)
    {
        char c = (char)f->data[f->pos++];
        buffer[i++] = c;
        if (c == '\n')
            break;
    }
    buffer[i] = 0;
    return buffer;
}

int tracedoom_remove(const char *name)
{
    ram_file_t *f = find(name);
    if (f == NULL)
        return -1;
    free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}

int tracedoom_rename(const char *from, const char *to)
{
    ram_file_t *f;

    pthread_mutex_lock(&g_lock);
    f = find(from);
    if (f == NULL)
    {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    {
        ram_file_t *old = find(to);
        if (old != NULL)
        {
            free(old->data);
            memset(old, 0, sizeof(*old));
        }
    }
    snprintf(f->name, sizeof(f->name), "%s", to);
    pthread_mutex_unlock(&g_lock);

    /* Doom writes a savegame to a temporary file and renames it into place, so this is where a finished save appears */
    tracedoom_file_written(to);
    return 0;
}

/* ---- what the door's savegames need ---- */

/* Puts a file into memory: the 24-byte header for a slot the door has, or a whole savegame it just sent. */
void tracedoom_ramfs_put(const char *name, const void *data, size_t size, int partial)
{
    ram_file_t *f;

    pthread_mutex_lock(&g_lock);
    f = find(name);
    if (f == NULL)
        f = create(name);
    if (f != NULL && grow(f, size))
    {
        memcpy(f->data, data, size);
        f->size = size;
        f->pos = 0;
        f->partial = partial;
        f->written = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

/* Adds to a file being streamed in from the door. Returns how big it is now. */
size_t tracedoom_ramfs_append(const char *name, size_t offset, const void *data, size_t size)
{
    ram_file_t *f;
    size_t total = 0;

    pthread_mutex_lock(&g_lock);
    f = find(name);
    if (f == NULL)
        f = create(name);
    if (f != NULL && grow(f, offset + size))
    {
        memcpy(f->data + offset, data, size);
        if (offset + size > f->size)
            f->size = offset + size;
        total = f->size;
    }
    pthread_mutex_unlock(&g_lock);
    return total;
}

/* The whole file is here now, so it can be read normally. */
void tracedoom_ramfs_complete(const char *name)
{
    ram_file_t *f;
    pthread_mutex_lock(&g_lock);
    f = find(name);
    if (f != NULL)
        f->partial = 0;
    pthread_mutex_unlock(&g_lock);
}

int tracedoom_ramfs_is_partial(const char *name)
{
    ram_file_t *f;
    int partial;
    pthread_mutex_lock(&g_lock);
    f = find(name);
    partial = f != NULL && f->partial;
    pthread_mutex_unlock(&g_lock);
    return partial;
}

/* Copies a file's contents out, for sending to the door. Returns its size, or 0 if there is no such file. */
size_t tracedoom_ramfs_read_all(const char *name, unsigned char **out)
{
    ram_file_t *f;
    size_t size = 0;

    pthread_mutex_lock(&g_lock);
    f = find(name);
    if (f != NULL && f->size > 0)
    {
        *out = (unsigned char *)malloc(f->size);
        if (*out != NULL)
        {
            memcpy(*out, f->data, f->size);
            size = f->size;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return size;
}
