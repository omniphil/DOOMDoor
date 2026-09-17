/*
 * SDL.h -- a stand-in for the few SDL calls Crispy Doom's shared files make, so they build unchanged for WebAssembly.
 *
 * The module has no SDL: video, input, timing and sound all go through the TRACE API instead (src/i_*_trace.c).
 * What's left in the shared files is byte swapping, qsort, a path lookup and a message box, and that's all this gives
 * them. Anything else won't compile, which is deliberate: it means a new SDL dependency crept in.
 */

#ifndef TRACEDOOM_SDL_SHIM_H
#define TRACEDOOM_SDL_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    22
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) (SDL_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))

/* wasm32 is little-endian */
static inline uint16_t SDL_Swap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t SDL_Swap32(uint32_t x)
{
    return (x << 24) | ((x << 8) & 0x00FF0000u) | ((x >> 8) & 0x0000FF00u) | (x >> 24);
}
#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_SwapBE16(x) SDL_Swap16(x)
#define SDL_SwapBE32(x) SDL_Swap32(x)

#define SDL_qsort qsort
/* i_sound.c prints which audio driver is in use; there is only one way out of the module */
static inline const char *SDL_GetCurrentAudioDriver(void) { return "TRACE"; }
#define SDL_malloc malloc
#define SDL_free   free
#define SDL_strdup strdup

/* Enough of SDL's types for the headers that mention them; nothing here is ever called. */
typedef union { int type; } SDL_Event;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Color { unsigned char r, g, b, a; } SDL_Color;
typedef int SDL_GameController;
typedef int SDL_Joystick;
typedef int SDL_JoystickID;
typedef int SDL_Keycode;
typedef int SDL_Scancode;
typedef int SDL_GameControllerAxis;
typedef int SDL_GameControllerButton;
typedef struct { unsigned char data[16]; } SDL_JoystickGUID;
#define SDL_CONTROLLER_BUTTON_MAX  21
#define SDL_CONTROLLER_AXIS_MAX     6
#define SDL_CONTROLLER_BUTTON_INVALID (-1)
#define SDL_CONTROLLER_AXIS_INVALID   (-1)

/* i_system.c: there is no window to put a message box on, so errors go to the log the sandbox forwards */
#define SDL_MESSAGEBOX_ERROR 0
int  SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window);
void SDL_Quit(void);

/*
 * opl.c waits for a music callback to come due before it carries on (OPL_Delay). Normally another thread is
 * rendering the chip and will fire it; here the one thread does everything, so waiting means rendering a little
 * more music, which is what tracedoom_opl_pump does (src/opl_trace.c).
 */
typedef int SDL_mutex;
typedef int SDL_cond;
void tracedoom_opl_pump(void);
static inline SDL_mutex *SDL_CreateMutex(void) { return 0; }
static inline SDL_cond  *SDL_CreateCond(void)  { return 0; }
static inline void SDL_DestroyMutex(SDL_mutex *m) { (void)m; }
static inline void SDL_DestroyCond(SDL_cond *c)   { (void)c; }
static inline int  SDL_LockMutex(SDL_mutex *m)    { (void)m; return 0; }
static inline int  SDL_UnlockMutex(SDL_mutex *m)  { (void)m; return 0; }
static inline int  SDL_CondSignal(SDL_cond *c)    { (void)c; return 0; }
static inline int  SDL_CondWait(SDL_cond *c, SDL_mutex *m) { (void)c; (void)m; tracedoom_opl_pump(); return 0; }

/* m_config.c asks where to keep settings; the module keeps them in its TRACE settings blob instead */
char *SDL_GetPrefPath(const char *org, const char *app);

#endif
