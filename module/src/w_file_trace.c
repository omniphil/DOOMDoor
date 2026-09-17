/*
 * w_file_trace.c -- where the WAD comes from: the asset the door uploaded, read through TRACE by its SHA-256.
 * Replaces Crispy Doom's w_file_stdc.c. The module has no file system, so there is nothing else it could open.
 */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

#include "trace_api.h"
#include "tracedoom.h"

/* Registered under the name w_file.c already looks for, so that file stays unmodified. */
extern wad_file_class_t stdc_wad_file;

typedef struct
{
    wad_file_t wad;
    char hash[65];
} trace_wad_file_t;

static wad_file_t *W_Trace_OpenFile(const char *path)
{
    trace_wad_file_t *file;
    const char *hash = tracedoom_wad_hash();
    int32_t size;

    /* Only the door's WAD exists. Doom asks for it by the name we gave it on the command line. */
    if (hash == NULL || path == NULL || strstr(path, TRACEDOOM_IWAD_NAME) == NULL)
        return NULL;
    size = trace_asset_size(hash);
    if (size <= 0)
        return NULL;

    file = Z_Malloc(sizeof(*file), PU_STATIC, 0);
    memset(file, 0, sizeof(*file));
    file->wad.file_class = &stdc_wad_file;
    file->wad.mapped = NULL;           /* not memory-mapped: reads go through TRACE */
    file->wad.length = (unsigned int)size;
    file->wad.path = M_StringDuplicate(path);
    memcpy(file->hash, hash, 65);
    return &file->wad;
}

static void W_Trace_CloseFile(wad_file_t *wad)
{
    Z_Free(wad->path);
    Z_Free(wad);
}

static size_t W_Trace_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len)
{
    trace_wad_file_t *file = (trace_wad_file_t *)wad;
    int32_t got = trace_asset_read(file->hash, (int32_t)offset, buffer, (int32_t)buffer_len);
    return got > 0 ? (size_t)got : 0;
}

wad_file_class_t stdc_wad_file =
{
    W_Trace_OpenFile,
    W_Trace_CloseFile,
    W_Trace_Read,
};
