/*
 * i_video_trace.c -- Doom's screen, drawn by TRACE instead of SDL. Replaces Crispy Doom's i_video.c.
 *
 * Doom draws into an 8-bit paletted buffer; this turns each finished frame into the BGRA one trace_present takes,
 * and hands it to TERMinator with the 4:3 flag, so it's shown with the same tall pixels as the original.
 * Keyboard events come from TERMinator (tracedoom.c) and are turned into Doom events here.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "d_event.h"
#include "d_loop.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "crispy.h"

#include "trace_api.h"
#include "tracedoom.h"

/* ---- what the rest of Doom expects to find here ---- */
int SCREENWIDTH, SCREENHEIGHT, SCREENHEIGHT_4_3;
int NONWIDEWIDTH;
pixel_t *I_VideoBuffer;
boolean screenvisible = true;
int screen_width = 0, screen_height = 0;
int fullscreen = 0, aspect_ratio_correct = 1, integer_scaling = 0, smooth_pixel_scaling = 0;
int vga_porch_flash = 0, force_software_renderer = 0, png_screenshots = 0;
int usegamma = 0;
int usemouse = 0;                    /* TERMinator doesn't capture the mouse yet */
int vanilla_keyboard_mapping = 1;
boolean screensaver_mode = false;
char *video_driver = "";
char *window_position = "center";
unsigned int joywait = 0;
fixed_t fractionaltic;
byte gamma2table[18][256];

/* [crispy] the nine darker gamma levels Crispy adds below vanilla's own five */
static const float gammalevels[9] =
{
    0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f,
};

static uint32_t *g_frame;            /* the BGRA frame handed to TERMinator */
static uint32_t  g_palette[256];
static int       g_frame_pixels;
static grabmouse_callback_t g_grab_mouse;   /* unused: no mouse yet */


/* ---- the screen ---- */

/* [crispy] SCREENWIDTH/HEIGHT follow the hires setting, as in Crispy's own I_GetScreenDimensions */
void I_GetScreenDimensions(void)
{
    SCREENWIDTH = ORIGWIDTH << crispy->hires;
    SCREENHEIGHT = ORIGHEIGHT << crispy->hires;
    SCREENHEIGHT_4_3 = ORIGHEIGHT_4_3 << crispy->hires;
    NONWIDEWIDTH = SCREENWIDTH;
    WIDESCREENDELTA = 0;             /* 4:3 only for now: widescreen would change what players can see */
}

void I_InitGraphics(void)
{
    I_GetScreenDimensions();
    V_Init();      /* works out the patch scaling from the size we just set, as Crispy's own i_video.c does */

    I_VideoBuffer = (pixel_t *)Z_Malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t), PU_STATIC, NULL);
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    V_RestoreBuffer();

    g_frame_pixels = SCREENWIDTH * SCREENHEIGHT;
    g_frame = (uint32_t *)malloc((size_t)g_frame_pixels * 4);
    if (!g_frame)
        I_Error("Not enough memory for the screen");
    memset(g_frame, 0, (size_t)g_frame_pixels * 4);

    I_SetGammaTable();
    screenvisible = true;
    tracedoom_log("doom: %dx%d", SCREENWIDTH, SCREENHEIGHT);
}

/* [crispy] the gamma curve Doom applies to the palette: 9 calculated levels, then vanilla's 5 with 4 in between */
void I_SetGammaTable(void)
{
    int i, j, k;

    for (i = 0; i < 9; ++i)
        for (j = 0; j < 256; ++j)
            gamma2table[i][j] = (byte)(pow(j / 255.0, 1.0 / gammalevels[i]) * 255.0 + 0.5);

    for (i = 9, k = 0; i < 18 && k < 5; i += 2, k++)
        memcpy(gamma2table[i], gammatable[k], 256);

    for (i = 10, k = 0; i < 18 && k < 4; i += 2, k++)
        for (j = 0; j < 256; j++)
            gamma2table[i][j] = (gammatable[k][j] + gammatable[k + 1][j]) / 2;
}

/* [crispy] the picture is re-made whenever the resolution changes; TERMinator scales whatever arrives */
void I_ReInitGraphics(int reinit)
{
    (void)reinit;
    I_ShutdownGraphics();
    I_InitGraphics();
}

void I_ToggleVsync(void) { }

void I_ShutdownGraphics(void)
{
    free(g_frame);
    g_frame = NULL;
}

void I_SetPalette(byte *doompalette)
{
    /* crispy->gamma, not usegamma: Crispy's own level, where 9 is "off" and 0 is the darkest of the levels it adds
     * below vanilla. The bottom two bits go the way the VGA card dropped them, which is what the original looked like. */
    for (int i = 0; i < 256; i++)
    {
        byte r = gamma2table[crispy->gamma][*doompalette++] & ~3;
        byte g = gamma2table[crispy->gamma][*doompalette++] & ~3;
        byte b = gamma2table[crispy->gamma][*doompalette++] & ~3;
        g_palette[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

int I_GetPaletteIndex(int r, int g, int b)
{
    int best = 0, best_diff = INT_MAX;

    for (int i = 0; i < 256; i++)
    {
        int pr = (int)((g_palette[i] >> 16) & 0xFF);
        int pg = (int)((g_palette[i] >> 8) & 0xFF);
        int pb = (int)(g_palette[i] & 0xFF);
        int diff = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
            if (diff == 0)
                break;
        }
    }
    return best;
}

void I_FinishUpdate(void)
{
    const pixel_t *src = I_VideoBuffer;

    if (!g_frame)
        return;

    /* [crispy] the frame rate Crispy's own display shows; type "idrate" in game to see it */
    {
        static int frames, last_ms;
        int now = trace_time_ms();
        frames++;
        if (now - last_ms >= 1000)
        {
            crispy->fps = (frames * 1000) / (now - last_ms);
            frames = 0;
            last_ms = now;
        }
    }

    for (int i = 0; i < g_frame_pixels; i++)
        g_frame[i] = g_palette[src[i]];

    trace_present(g_frame, SCREENWIDTH, SCREENHEIGHT, TRACE_PRESENT_ASPECT_4_3);

    /* Sound is written from this thread too, right after the picture, so the two stay in step */
    tracedoom_pump_audio();

    /* And whatever the savegames need: filling in the Load menu, or sending a save to the BBS a piece at a time */
    tracedoom_saves_update();
}

void I_ReadScreen(pixel_t *scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
}

/* ---- keyboard ----
 *
 * TERMinator sends physical keys as set-1 scancodes (engine contract TE_IN_KEY), which is what a PC keyboard
 * controller sent in 1993, so this is close to what vanilla Doom read. Text typing (save names, chat) comes
 * separately as TE_IN_TEXT.
 */

static const struct { int scancode; int key; } g_keymap[] = {
    { 0x01, KEY_ESCAPE },   { 0x1C, KEY_ENTER },     { 0x0F, KEY_TAB },      { 0x0E, KEY_BACKSPACE },
    { 0x39, ' ' },          { 0x1D, KEY_RCTRL },     { 0x38, KEY_LALT },     { 0x2A, KEY_RSHIFT },
    { 0x36, KEY_RSHIFT },   { 0x3A, KEY_CAPSLOCK },
    /* arrows and the navigation block arrive with the extended flag; both forms map the same way */
    { 0x48, KEY_UPARROW },  { 0x50, KEY_DOWNARROW }, { 0x4B, KEY_LEFTARROW },{ 0x4D, KEY_RIGHTARROW },
    { 0x52, KEY_INS },      { 0x53, KEY_DEL },       { 0x47, KEY_HOME },     { 0x4F, KEY_END },
    { 0x49, KEY_PGUP },     { 0x51, KEY_PGDN },      { 0x45, KEY_PAUSE },
    { 0x3B, KEY_F1 },       { 0x3C, KEY_F2 },        { 0x3D, KEY_F3 },       { 0x3E, KEY_F4 },
    { 0x3F, KEY_F5 },       { 0x40, KEY_F6 },        { 0x41, KEY_F7 },       { 0x42, KEY_F8 },
    { 0x43, KEY_F9 },       { 0x44, KEY_F10 },       { 0x57, KEY_F11 },      { 0x58, KEY_F12 },
    { 0x02, '1' }, { 0x03, '2' }, { 0x04, '3' }, { 0x05, '4' }, { 0x06, '5' },
    { 0x07, '6' }, { 0x08, '7' }, { 0x09, '8' }, { 0x0A, '9' }, { 0x0B, '0' },
    { 0x0C, KEY_MINUS },    { 0x0D, KEY_EQUALS },
    { 0x10, 'q' }, { 0x11, 'w' }, { 0x12, 'e' }, { 0x13, 'r' }, { 0x14, 't' }, { 0x15, 'y' },
    { 0x16, 'u' }, { 0x17, 'i' }, { 0x18, 'o' }, { 0x19, 'p' }, { 0x1A, '[' }, { 0x1B, ']' },
    { 0x1E, 'a' }, { 0x1F, 's' }, { 0x20, 'd' }, { 0x21, 'f' }, { 0x22, 'g' }, { 0x23, 'h' },
    { 0x24, 'j' }, { 0x25, 'k' }, { 0x26, 'l' }, { 0x27, ';' }, { 0x28, '\'' }, { 0x29, '`' },
    { 0x2B, '\\' },
    { 0x2C, 'z' }, { 0x2D, 'x' }, { 0x2E, 'c' }, { 0x2F, 'v' }, { 0x30, 'b' }, { 0x31, 'n' },
    { 0x32, 'm' }, { 0x33, ',' }, { 0x34, '.' }, { 0x35, '/' },
};

static int scancode_to_key(int scancode)
{
    for (size_t i = 0; i < arrlen(g_keymap); i++)
        if (g_keymap[i].scancode == scancode)
            return g_keymap[i].key;
    return 0;
}

/* Events from TERMinator (engine contract TE_IN_*), turned into Doom events. */
void I_GetEvent(void)
{
    trace_event_t in;

    while (tracedoom_next_event(&in))
    {
        event_t ev = { 0 };

        switch (in.type)
        {
        case 1:   /* TE_IN_KEY: flags bit0 pressed, a = set-1 scancode */
            ev.data1 = scancode_to_key(in.a);
            if (ev.data1 == 0)
                break;
            ev.type = (in.flags & 1) ? ev_keydown : ev_keyup;
            /* data2/data3 are the typed character vanilla would have produced */
            ev.data2 = ev.data3 = (ev.data1 >= 32 && ev.data1 < 127) ? ev.data1 : 0;
            D_PostEvent(&ev);
            break;

        case 6:   /* TE_IN_QUIT: the player closed the window or the door went away */
            I_Quit();
            break;

        default:
            break;
        }
    }
}

void I_StartTic(void)
{
    I_GetEvent();
}

/* ---- the rest of i_video.h: things a windowed port needs and this one doesn't ---- */

void I_StartFrame(void) { }
void I_UpdateNoBlit(void) { }
void I_BeginRead(void) { }
void I_StartDisplay(void) { }
void I_UpdateFracTic(void) { fractionaltic = 0; }
void I_SetWindowTitle(const char *title) { (void)title; }
void I_InitWindowTitle(void) { }
void I_InitWindowIcon(void) { }
void I_RegisterWindowIcon(const unsigned int *icon, int width, int height) { (void)icon; (void)width; (void)height; }
void I_CheckIsScreensaver(void) { }
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_EnableLoadingDisk(int xoffs, int yoffs) { (void)xoffs; (void)yoffs; }
void I_GraphicsCheckCommandLine(void) { }
void I_GetWindowPosition(int *x, int *y, int w, int h) { (void)w; (void)h; *x = *y = 0; }
void I_SetGrabMouseCallback(grabmouse_callback_t func) { g_grab_mouse = func; }

void I_BindVideoVariables(void)
{
    /* The player's own TERMinator settings decide how the picture is shown, so only what affects Doom is bound */
    M_BindIntVariable("usegamma", &usegamma);
}
