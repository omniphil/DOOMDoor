/*
 * ansi_compat.c -- what the game needs to run natively that the sandbox never asked for. See ansi_compat.h.
 * Built with the game's flags, so fgets here is the module's in-memory one and Crispy's headers are at hand.
 */

#include <stdarg.h>
#include <stdio.h>

#include "crispy.h"
#include "m_argv.h"

int ansi_fscanf(void *stream, const char *format, ...)
{
    char line[512];
    va_list args;
    int n;

    if (fgets(line, sizeof(line), stream) == NULL)
        return EOF;
    va_start(args, format);
    n = vsscanf(line, format, args);
    va_end(args);
    return n;
}

/*
 * Crispy's own main() (i_main.c, which the module replaces) fills these in before starting the game. The module
 * never does: in WebAssembly, memory starts at address 0, so reading through the NULLs they're left as quietly
 * works. On a real CPU it crashes, so the native build sets them the way i_main.c would.
 */
void ansi_game_prepare(void)
{
    exedir = (char *)"";
    crispy->platform = "BBS";
    crispy->sdlversion = (char *)"none";
}
