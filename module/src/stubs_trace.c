/*
 * stubs_trace.c -- the rest of what Doom expects from a PC, answered for the sandbox.
 *
 * There is no file system, no window, no joystick and no network here: the module can only draw, make sound, read the
 * door's WAD and keep a small settings blob. These are the stand-ins for everything else, replacing Crispy Doom's
 * i_main.c, i_input.c, i_joystick.c, i_endoom.c, i_musicpack.c, d_iwad.c and the SDL parts of i_system.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "d_iwad.h"
#include "d_mode.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "net_defs.h"

#include "trace_api.h"
#include "tracedoom.h"

/* ---- the IWAD ----
 * Doom normally hunts through folders for a WAD. Here there is exactly one: the shareware IWAD the door uploaded.
 */

static const iwad_t g_shareware_iwad = { TRACEDOOM_IWAD_NAME, doom, shareware, "Doom Shareware" };

boolean D_IsIWADName(const char *name)
{
    return !strcasecmp(name, TRACEDOOM_IWAD_NAME);
}

char *D_FindWADByName(const char *filename)
{
    return D_IsIWADName(filename) ? M_StringDuplicate(filename) : NULL;
}

char *D_TryFindWADByName(const char *filename)
{
    char *result = D_FindWADByName(filename);
    return result != NULL ? result : M_StringDuplicate(filename);
}

char *D_FindIWAD(int mask, GameMission_t *mission)
{
    (void)mask;
    *mission = doom;
    return M_StringDuplicate(TRACEDOOM_IWAD_NAME);
}

const iwad_t **D_FindAllIWADs(int mask)
{
    static const iwad_t *list[2];
    (void)mask;
    list[0] = &g_shareware_iwad;
    list[1] = NULL;
    return list;
}

const char *D_SaveGameIWADName(GameMission_t gamemission, GameVariant_t gamevariant)
{
    (void)gamemission; (void)gamevariant;
    return TRACEDOOM_IWAD_NAME;
}

const char *D_SuggestIWADName(GameMission_t mission, GameMode_t mode)
{
    (void)mission; (void)mode;
    return TRACEDOOM_IWAD_NAME;
}

const char *D_SuggestGameName(GameMission_t mission, GameMode_t mode)
{
    (void)mission; (void)mode;
    return "Doom Shareware";
}

void D_CheckCorrectIWAD(GameMission_t mission)
{
    (void)mission;
}

/* ---- settings ----
 * m_config.c wants somewhere to keep the config file. The module's own settings blob is the right place, but saving
 * the whole of Doom's config into 64 KB of key/value text is for later; for now the defaults are used every time.
 */

char *SDL_GetPrefPath(const char *org, const char *app)
{
    (void)org; (void)app;
    return M_StringDuplicate("");
}

/* ---- errors and shutdown ---- */

int SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window)
{
    (void)flags; (void)window;
    tracedoom_log("doom: %s: %s", title ? title : "error", message ? message : "");
    return 0;
}

void SDL_Quit(void)
{
}

/* ENDOOM is the text screen the original printed on exit; there is no terminal here to print it to. */
void I_Endoom(byte *endoom_data)
{
    (void)endoom_data;
}

/* ---- input bits that belong to a mouse and joystick we don't have ---- */

float mouse_acceleration = 2.0f;
int   mouse_threshold = 10;
float mouse_acceleration_y = 1.0f;
int   mouse_threshold_y = 0;
int   mouse_y_invert = 0;
int   novert = 0;
int   runcentering = 1;

double I_AccelerateMouse(int val) { return val; }
double I_AccelerateMouseY(int val) { return val; }
void I_ReadMouse(void) { }
void I_ReadMouseUncapped(void) { }
void I_StartTextInput(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void I_StopTextInput(void) { }
void I_BindInputVariables(void) { }

int use_analog = 0;
int joystick_turn_sensitivity = 10;
int joystick_move_sensitivity = 10;
int joystick_look_sensitivity = 10;

void I_InitJoystick(void) { }
void I_ShutdownJoystick(void) { }
void I_UpdateJoystick(void) { }
void I_BindJoystickVariables(void) { }

/* ---- music packs ----
 * Playing OGG files in place of the WAD's music needs a file system, so the substitute module always fails to start
 * and Doom falls back to the OPL music it was written for.
 */

char *music_pack_path = "";
char *timidity_cfg_path = "";
void I_InitTimidityConfig(void) { }

static boolean I_NullMusic_Init(void) { return false; }
static void I_NullMusic_Shutdown(void) { }
static void I_NullMusic_SetVolume(int volume) { (void)volume; }
static void I_NullMusic_Pause(void) { }
static void I_NullMusic_Resume(void) { }
static void *I_NullMusic_Register(void *data, int len) { (void)data; (void)len; return NULL; }
static void I_NullMusic_Unregister(void *handle) { (void)handle; }
static void I_NullMusic_Play(void *handle, boolean looping) { (void)handle; (void)looping; }
static void I_NullMusic_Stop(void) { }
static boolean I_NullMusic_IsPlaying(void) { return false; }
static void I_NullMusic_Poll(void) { }

const music_module_t music_pack_module =
{
    NULL,
    0,
    I_NullMusic_Init,
    I_NullMusic_Shutdown,
    I_NullMusic_SetVolume,
    I_NullMusic_Pause,
    I_NullMusic_Resume,
    I_NullMusic_Register,
    I_NullMusic_Unregister,
    I_NullMusic_Play,
    I_NullMusic_Stop,
    I_NullMusic_IsPlaying,
    I_NullMusic_Poll,
};

/* ---- network play ----
 * There is no socket in the sandbox. Doom's loopback driver still runs a single-player game; multiplayer would come
 * through the door instead (a module can only talk to the door).
 */

net_module_t net_sdl_module =
{
    NULL, NULL, NULL, NULL, NULL, NULL,
};

/* ---- the setup tool's text screen, and file searching ----
 * Neither exists here: there is no console to draw on and no folder to search.
 */

/* net_gui.c draws a text-mode "waiting for players" screen; a single-player game never waits. */
boolean NET_WaitForLaunch(void) { return true; }

typedef struct glob_s glob_t_stub;
void *I_StartMultiGlob(const char *directory, int flags, const char *glob, ...)
{
    (void)directory; (void)flags; (void)glob;
    return NULL;
}
const char *I_NextGlob(void *glob) { (void)glob; return NULL; }
void I_EndGlob(void *glob) { (void)glob; }

/* ---- sound settings that belong to hardware we don't emulate ---- */

char *gus_patch_path = "";
int   gus_ram_kb = 1024;
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* [crispy] widescreen rendering is off: the view stays 4:3, as vanilla drew it */
int WIDESCREENDELTA = 0;
