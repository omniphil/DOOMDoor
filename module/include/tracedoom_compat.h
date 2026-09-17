/*
 * tracedoom_compat.h -- forced into every file with -include, so Crispy Doom's sources build unchanged.
 *
 * Two things are missing when Crispy is built without SDL: a few headers it gets indirectly from SDL.h, and the
 * handful of SDL calls left outside its #ifdefs. Both are answered here rather than by editing ../third_party.
 */

#ifndef TRACEDOOM_COMPAT_H
#define TRACEDOOM_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "SDL.h"

/*
 * Doom writes its startup messages and warnings with printf. There is no console in the sandbox and WASI throws
 * stdout away, so they are sent to TERMinator's debug output instead.
 */
#include <stdarg.h>
int tracedoom_printf(const char *fmt, ...);
int tracedoom_vprintf(const char *fmt, va_list args);
int tracedoom_fprintf(void *stream, const char *fmt, ...);
/*
 * Doom also reads and writes real files: a temporary MIDI file per music track, savegames, its config. There is no
 * file system in the sandbox, so those go to a handful of files kept in memory (src/ramfs.c).
 */
void  *tracedoom_fopen(const char *name, const char *mode);
int    tracedoom_fclose(void *stream);
size_t tracedoom_fread(void *buffer, size_t size, size_t count, void *stream);
size_t tracedoom_fwrite(const void *buffer, size_t size, size_t count, void *stream);
int    tracedoom_fseek(void *stream, long offset, int whence);
long   tracedoom_ftell(void *stream);
int    tracedoom_feof(void *stream);
int    tracedoom_fgetc(void *stream);
int    tracedoom_fputc(int c, void *stream);
char  *tracedoom_fgets(char *buffer, int size, void *stream);
int    tracedoom_remove(const char *name);
int    tracedoom_rename(const char *from, const char *to);
int    tracedoom_is_ram_file(void *stream);

#define fopen(name, mode)   ((FILE *)tracedoom_fopen((name), (mode)))
#define fclose(stream)      tracedoom_fclose(stream)
#define fread(b, s, c, f)   tracedoom_fread((b), (s), (c), (f))
#define fwrite(b, s, c, f)  tracedoom_fwrite((b), (s), (c), (f))
#define fseek(f, o, w)      tracedoom_fseek((f), (o), (w))
#define ftell(f)            tracedoom_ftell(f)
#define feof(f)             tracedoom_feof(f)
#define fgetc(f)            tracedoom_fgetc(f)
#define fputc(c, f)         tracedoom_fputc((c), (f))
#define fgets(b, n, f)      tracedoom_fgets((b), (n), (f))
#define remove(name)        tracedoom_remove(name)
#define rename(a, b)        tracedoom_rename((a), (b))
#define fflush(f)           (0)

/* Doom exits with -1 on a fatal error, which WebAssembly won't accept; this says why and leaves cleanly. */
void tracedoom_exit(int code);
#define exit(code) tracedoom_exit(code)

#define printf   tracedoom_printf
#define vprintf  tracedoom_vprintf
#define fprintf  tracedoom_fprintf
#define vfprintf(stream, fmt, args) tracedoom_vprintf((fmt), (args))
#define puts(s)  tracedoom_printf("%s\n", (s))
#define fputs(s, stream) tracedoom_fputs((s), (stream))
int tracedoom_fputs(const char *text, void *stream);

#endif
