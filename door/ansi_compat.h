/*
 * ansi_compat.h -- forced into the game's files after the module's own tracedoom_compat.h, for the native build only.
 *
 * The module sends Doom's file calls to its in-memory files, all except fscanf, which Doom uses for one thing:
 * reading its config. In the sandbox there is no config to read, so it never mattered. Here the door hands Doom a
 * config (ansi_host.c), so fscanf has to read the in-memory file too. Doom reads one "name value" line per call.
 * ansi_compat.c also sets up what Crispy's own main() would have, which the sandbox build gets away without.
 */

#ifndef ANSI_COMPAT_H
#define ANSI_COMPAT_H

#include <stdio.h>      /* before the macro, so stdio.h's own declaration isn't rewritten */

int ansi_fscanf(void *stream, const char *format, ...);
#define fscanf(stream, ...) ansi_fscanf((stream), __VA_ARGS__)

#endif
