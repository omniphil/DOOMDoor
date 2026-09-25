/*
 * main.c -- DOOM, as a BBS door.
 *
 * Three ways to play, picked by the player on the start page:
 *   TRACE     the door sends the game and TERMinator runs the whole of Doom in its sandbox on the player's own
 *             machine, so there are no pictures on the wire and it plays at full speed with sound.
 *   JPEG XL   for terminals that speak the CTerm APC picture and sound commands: Doom runs here on the BBS and the
 *             door sends its real 320x200 picture as JPEG XL, with the sound played from the terminal's own cache
 *             (pix_play.c).
 *   ANSI      for every other terminal: Doom runs here and the door sends it as ANSI pictures, in 24-bit, 256 or 16
 *             colours (ansi_play.c). Blocky and silent, but it's Doom in a BBS terminal.
 *
 * Shareware DOOM only for now: the episode id Software gave away and allowed to be passed on unchanged.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi_play.h"
#include "door.h"
#include "jxl_enc.h"
#include "pix_play.h"
#include "pix_sound.h"
#include "saves.h"
#include "trace_doom.h"

#define CSI "\033["

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;31m"
               "        ===============================================\r\n"
               "                D O O M   -   shareware episode\r\n"
               "        ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "                    BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

/* The name in its own colours, the way the client writes it: TERM in magenta, inator in cyan. */
static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

/* Drawn while the WAD goes up the first time, so the wait doesn't look like a hung door. */
static void wad_progress(int percent)
{
    char bar[80];
    int filled = percent * 40 / 100;

    memset(bar, ' ', sizeof(bar));
    memcpy(bar, "  [", 3);
    for (int i = 0; i < 40; i++)
        bar[3 + i] = i < filled ? '#' : '.';
    snprintf(bar + 43, sizeof(bar) - 43, "] %3d%%", percent);
    door_write(CSI "s");                 /* remember where we are */
    door_write("\r");
    door_write(bar);
    door_write(CSI "u");
}

/* ---- choosing how to play ---- */

/* What the caller's terminal turned out to be */
typedef struct
{
    bool trace;             /* TERMinator with TRACE: the real game on their own machine */
    bool cterm;             /* a CTerm terminal: answers "who are you" with its version */
    int  cterm_major, cterm_minor;
    bool jxl;               /* draws JPEG XL pictures */
    bool sound;             /* plays sound files: Ogg Vorbis for the music and 8-bit WAV for the effects */
    bool keys;              /* reports key presses and releases */
    int  px_w, px_h;        /* the screen in pixels */
} terminal_t;

/* CTerm from this version on understands 256 and 24-bit colour. TERMinator reports 1.324 or later, and other CTerm
 * terminals of the last few years a 1.3xx version too. Older, or anything else, is marked UNKNOWN rather than hidden: the test strips on the
 * menu let the player see for themselves. */
#define CTERM_COLOR_MAJOR 1
#define CTERM_COLOR_MINOR 300

/*
 * Asks the terminal who it is (ESC [ c). A CTerm terminal answers ESC [ = 67;84;101;114;109;<major>;<minor> c,
 * "CTerm" in ASCII followed by its version. Others answer differently or not at all.
 */
static void detect_cterm(terminal_t *term)
{
    char reply[64];
    int n = 0, c;
    const char *p;

    door_write(CSI "c");
    c = door_read_char_timeout(500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        if (c == 'c' && n > 2)
            break;
        c = door_read_char_timeout(200);
    }
    reply[n] = '\0';

    p = strstr(reply, "[=67;84;101;114;109;");
    if (p != NULL && sscanf(p + strlen("[=67;84;101;114;109;"), "%d;%d", &term->cterm_major, &term->cterm_minor) == 2)
        term->cterm = true;
}

/* Is this CTerm version at least major.minor? */
static bool cterm_at_least(const terminal_t *term, int major, int minor)
{
    return term->cterm && (term->cterm_major > major || (term->cterm_major == major && term->cterm_minor >= minor));
}

/*
 * For a CTerm terminal, what the JPEG XL mode needs to know. The questions go out together, followed by a status
 * request (ESC [ 5 n) that every terminal answers: answers come back in order, so once that one is in, any question
 * still unanswered is one the terminal doesn't know. Each answer:
 *   Q;JXL                          ESC [ = 1 ; 1 - n       JPEG XL pictures
 *   Q;libsndfile                   ESC [ = 7 ; 100 ; 1 n   sound files
 *   Q;libsndfileFormat;32;96       ESC [ = 7 ; 101 ; 32 ; 96 ; 1 n    Ogg Vorbis
 *   Q;libsndfileFormat;1;5         ESC [ = 7 ; 101 ; 1 ; 5 ; 1 n      WAV, 8-bit
 *   CSI < 0 c                      ESC [ < 0 ; ... c       its features: 8 = key presses and releases
 *   CSI ? 2 ; 1 S                  ESC [ ? 2 ; 0 ; w ; h S the screen in pixels
 */
static void detect_pixels(terminal_t *term)
{
    char reply[512];
    int n = 0, c;
    const char *p;

    if (!term->cterm)
        return;
    door_write(APC_PREFIX "Q;JXL" APC_END APC_PREFIX "Q;libsndfile" APC_END
               APC_PREFIX "Q;libsndfileFormat;32;96" APC_END APC_PREFIX "Q;libsndfileFormat;1;5" APC_END
               CSI "<0c" CSI "?2;1S" CSI "5n");
    c = door_read_char_timeout(1500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        reply[n] = '\0';
        if (strstr(reply, "\033[0n") != NULL)
            break;
        c = door_read_char_timeout(500);
    }
    reply[n] = '\0';

    term->jxl = strstr(reply, "\033[=1;1-n") != NULL;
    term->sound = strstr(reply, "\033[=7;100;1n") != NULL && strstr(reply, "\033[=7;101;32;96;1n") != NULL &&
                  strstr(reply, "\033[=7;101;1;5;1n") != NULL;
    p = strstr(reply, "\033[<0");
    if (p != NULL)
        for (p += 4; *p == ';'; )
        {
            int feature = atoi(++p);
            if (feature == 8)
                term->keys = true;
            while (*p >= '0' && *p <= '9')
                p++;
        }
    p = strstr(reply, "\033[?2;0;");
    if (p == NULL || sscanf(p + 7, "%d;%d", &term->px_w, &term->px_h) != 2)
        term->px_w = 640, term->px_h = 400;     /* 80x25 at 8x16, what nearly every CTerm terminal is */
}

/* The JPEG XL mode: the terminal draws JPEG XL, can take it in the command itself or through its cache (1.329), and
 * this box has libjxl to make it */
static bool can_jxl(const terminal_t *term)
{
    return term->jxl && jxl_enc_available();
}

static bool cterm_has_color(const terminal_t *term)
{
    return term->cterm && (term->cterm_major > CTERM_COLOR_MAJOR ||
                           (term->cterm_major == CTERM_COLOR_MAJOR && term->cterm_minor >= CTERM_COLOR_MINOR));
}

enum { CHOICE_TRACE = 1, CHOICE_JXL, CHOICE_24BIT, CHOICE_256, CHOICE_16 };

/* The player's last choice, kept in their own folder beside their savegames */
static void choice_path(char *out, size_t size)
{
    snprintf(out, size, "%s/display.cfg", saves_folder());
}

static void load_choice(int *choice, bool *utf8)
{
    char path[640];
    FILE *fp;
    int c = 0, u = 0;

    choice_path(path, sizeof(path));
    fp = fopen(path, "r");
    if (fp == NULL)
        return;
    if (fscanf(fp, "mode=%d utf8=%d", &c, &u) >= 1 && c >= CHOICE_TRACE && c <= CHOICE_16)
    {
        *choice = c;
        *utf8 = u != 0;
    }
    else if (rewind(fp), fscanf(fp, "display=%d utf8=%d", &c, &u) >= 1 && c >= 1 && c <= 4)
    {
        /* Saved before the JPEG XL mode came in as [2], when the ANSI choices were 2-4 */
        *choice = c == 1 ? CHOICE_TRACE : c + 1;
        *utf8 = u != 0;
    }
    fclose(fp);
}

static void save_choice(int choice, bool utf8)
{
    char path[640];
    FILE *fp;

    choice_path(path, sizeof(path));
    fp = fopen(path, "w");
    if (fp == NULL)
        return;
    fprintf(fp, "mode=%d utf8=%d\n", choice, utf8 ? 1 : 0);
    fclose(fp);
}

/* ▀ in CP437, or in UTF-8 for a terminal that expects it */
static const char *upper_half(bool utf8)
{
    return utf8 ? "\xE2\x96\x80" : "\xDF";
}

/* A row of half-blocks: a rainbow on top and a grey ramp underneath, in 24-bit or 256 colours. If the terminal
 * understands them, it's a smooth gradient; if it doesn't, it's garbage or flat bands. */
static void test_strip(bool truecolor, bool utf8)
{
    char seq[64];

    for (int i = 0; i < 48; i++)
    {
        float h = i / 48.0f * 6.0f;
        int sector = (int)h;
        float f = h - (float)sector;
        int up = (int)(255 * f), down = 255 - up;
        int r, g, b, grey = 16 + i * 230 / 47;
        switch (sector)
        {
        case 0:  r = 255;  g = up;   b = 0;    break;
        case 1:  r = down; g = 255;  b = 0;    break;
        case 2:  r = 0;    g = 255;  b = up;   break;
        case 3:  r = 0;    g = down; b = 255;  break;
        case 4:  r = up;   g = 0;    b = 255;  break;
        default: r = 255;  g = 0;    b = down; break;
        }
        if (truecolor)
            snprintf(seq, sizeof(seq), CSI "38;2;%d;%d;%d;48;2;%d;%d;%dm", r, g, b, grey, grey, grey);
        else
            snprintf(seq, sizeof(seq), CSI "38;5;%d;48;5;%dm", 16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255),
                     232 + i * 23 / 47);
        door_write(seq);
        door_write(upper_half(utf8));
    }
    door_write(CSI "0m");
}

/* The 16-colour blocks and shades the ANSI picture is made of */
static void block_strip(bool utf8)
{
    static const char *cp437[] = { "\xB0", "\xB1", "\xB2", "\xDB", "\xDC", "\xDF", "\xDD", "\xDE" };
    static const char *utf[] = { "\xE2\x96\x91", "\xE2\x96\x92", "\xE2\x96\x93", "\xE2\x96\x88",
                                 "\xE2\x96\x84", "\xE2\x96\x80", "\xE2\x96\x8C", "\xE2\x96\x90" };
    static const char *colors[] = { "0;31", "0;33", "0;32", "0;36", "0;34", "0;35", "1;31", "1;33", "1;32", "1;36",
                                    "1;34", "1;35" };
    char seq[24];

    for (int i = 0; i < 48; i++)
    {
        snprintf(seq, sizeof(seq), CSI "%s;40m", colors[(i / 8) % 12]);
        door_write(seq);
        door_write(utf8 ? utf[i % 8] : cp437[i % 8]);
    }
    door_write(CSI "0m");
}

static void draw_menu(const terminal_t *term, int recommended, bool utf8)
{
    bool color = cterm_has_color(term);
    char line[32];

    title();
    door_write(CSI "0;37m  Choose how to play:\r\n\r\n");

    if (term->trace)
        door_write(CSI "1;37m  [1] " CSI "1;35mTRACE" CSI "1;37m graphics   " CSI "0;37m(640x400 + Sound)      "
                   CSI "1;32mDETECTED\r\n");
    else
        door_write(CSI "1;30m  [1] TRACE graphics   (640x400 + Sound)      NOT FOUND (needs " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;30m)\r\n");
    if (can_jxl(term))
    {
        door_write(CSI "1;37m  [2] JPEG XL graphics " CSI "0;37m");
        door_write(term->sound ? "(320x200 + Sound)      " : "(320x200, no sound)    ");
        door_write(CSI "1;32mDETECTED\r\n");
    }
    else
    {
        door_write(CSI "1;30m  [2] JPEG XL graphics (320x200 + Sound)      NOT FOUND\r\n");
    }
    door_write(CSI "1;37m  [3] ANSI 24-bit      " CSI "0;37m(best ANSI look)       ");
    door_write(color ? CSI "1;32mDETECTED\r\n" : CSI "1;33mUNKNOWN\r\n");
    door_write(CSI "1;37m  [4] ANSI 256         " CSI "0;37m(faster, near 24-bit)  ");
    door_write(color ? CSI "1;32mDETECTED\r\n" : CSI "1;33mUNKNOWN\r\n");
    door_write(CSI "1;37m  [5] ANSI 16          " CSI "0;37m(works everywhere)     " CSI "1;32mSUPPORTED\r\n\r\n");

    door_write(CSI "0;37m    24-bit  ");
    test_strip(true, utf8);
    door_write("\r\n    256     ");
    test_strip(false, utf8);
    door_write("\r\n    16      ");
    block_strip(utf8);
    door_write("\r\n\r\n");

    snprintf(line, sizeof(line), "[%d]", recommended);
    door_write(CSI "0;37m  Enter = " CSI "1;37m");
    door_write(line);
    door_write(CSI "0;37m     Q = back to the BBS " CSI "0m");
}

/* Returns the choice, or 0 to go back to the BBS. */
static int choose_display(const terminal_t *term, bool *utf8)
{
    int recommended = term->trace ? CHOICE_TRACE : can_jxl(term) ? CHOICE_JXL
                    : cterm_has_color(term) ? CHOICE_24BIT : CHOICE_16;
    int remembered = 0;

    load_choice(&remembered, utf8);
    if (remembered != 0 && (remembered != CHOICE_TRACE || term->trace) && (remembered != CHOICE_JXL || can_jxl(term)))
        recommended = remembered;

    for (;;)
    {
        int c;

        draw_menu(term, recommended, *utf8);
        c = door_read_char();
        if (c < 0 || c == 'q' || c == 'Q')
            return 0;
        /* Not shown on the menu any more (2026-09-19), but still there for a terminal that wants UTF-8 blocks */
        if (c == 'u' || c == 'U')
        {
            *utf8 = !*utf8;
            continue;
        }
        if (c == '\r' || c == '\n')
            c = '0' + recommended;
        if (c == '1' && !term->trace)
            continue;
        if (c == '2' && !can_jxl(term))
            continue;
        if (c >= '1' && c <= '5')
        {
            save_choice(c - '0', *utf8);
            return c - '0';
        }
    }
}

static void goodbye(void)
{
    cls();
    door_write(CSI "1;31m\r\n  Thanks for playing DOOM.\r\n\r\n" CSI "0m");
    door_write(CSI "1;34m" "  BBS door by JSONBourne\r\n\r\n" CSI "0m");

    /* DOOM here is Crispy Doom, which is GPL-2: players are entitled to its source */
    door_write(CSI "0;37m  DOOM here is Crispy Doom, free software under the GNU GPL v2.\r\n");
    door_write("  Source: " CSI "1;37m" "https://github.com/omniphil/DOOMDoor\r\n" CSI "0m");
    press_any_key();
}

/* TRACE: the whole game goes to TERMinator and runs on the player's own machine. */
static int play_trace(void)
{
    title();
    door_write(CSI "1;32m  ");
    write_terminator();
    door_write(CSI "1;32m found. Sending the game...\r\n" CSI "0m");

    /* Whose saved games these are. "player" means the BBS didn't tell us who is calling (no drop file), and everyone
     * would then share one set of saves, so it's worth the sysop seeing it. */
    door_write(CSI "0;37m  Saved games for: ");
    door_write(saves_player());
    door_write("\r\n");

    /* DOOM here is Crispy Doom, which is GPL-2: players are entitled to the source of what they were just sent */
    door_write("  Crispy Doom, GPL-2: https://github.com/omniphil/DOOMDoor\r\n\r\n" CSI "0m");

    /* The WAD is 4 MB and only travels once: after that it's cached on the player's machine for good. */
    door_write(CSI "0;37m  Checking whether you already have the game data...\r\n" CSI "0m");
    if (!trace_doom_send_wad(wad_progress)) {
        door_write(CSI "1;33m\r\n  The game data couldn't be sent. Please try again later.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }

    door_write(CSI "0;37m\r\n  Starting DOOM. Use ESC for the menu, and quit from there to come back.\r\n" CSI "0m");

    if (!trace_doom_open()) {
        door_write(CSI "1;33m\r\n  DOOM couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }

    /*
     * The game now covers the terminal, so clear what's underneath it. Otherwise, the moment the player quits, the
     * text from a moment ago ("Sending the game...") flashes up before this door can draw its closing screen.
     */
    cls();

    /* From here the player's terminal has the keyboard and the screen; we wait for them to quit. */
    trace_doom_wait();
    trace_doom_close();
    goodbye();
    return 0;
}

/* Drawn while the sound effects go up the first time */
static void sound_progress(int percent)
{
    wad_progress(percent);
}

/* JPEG XL: the game runs here on the BBS, and the player sees its real picture and hears its sound. */
static int play_jxl(const terminal_t *term)
{
    pix_caps_t caps = { 0 };
    play_result_t result;

    caps.blob = cterm_at_least(term, 1, 329);
    caps.zoom = cterm_at_least(term, 1, 332);
    caps.keys = term->keys;
    caps.px_w = term->px_w;
    caps.px_h = term->px_h;

    title();
    door_write(CSI "0;37m  Saved games for: ");
    door_write(saves_player());
    door_write("\r\n\r\n");
    if (term->sound)
    {
        door_write("  Checking whether you already have the sounds...\r\n");
        caps.sound = pix_sound_prepare(sound_progress);
        door_write("\r\n\r\n");
    }
    if (caps.keys)
    {
        door_write("  The keys are DOOM's own: arrows move, Ctrl fires, Space opens doors,\r\n");
        door_write("  Alt with the arrows steps sideways, 1-7 pick a weapon, Tab shows the map\r\n");
        door_write("  and Esc brings up the menu. Quit from the menu, or press Ctrl-Q, to\r\n");
        door_write("  come back.\r\n\r\n");
    }
    else
    {
        door_write("  Arrows or WASD move, F fires, Space opens doors, 1-7 pick a weapon,\r\n");
        door_write("  Tab shows the map and Esc brings up the menu. Quit from the menu, or\r\n");
        door_write("  press Ctrl-Q, to come back.\r\n\r\n");
    }
    door_write(CSI "1;37m  Press any key to start." CSI "0m");
    if (door_read_char() < 0)
        return 0;

    result = pix_play(&caps);
    if (result == PLAY_HANGUP)
        return 0;
    if (result == PLAY_FAILED)
    {
        title();
        door_write(CSI "1;33m  DOOM couldn't be started. Please tell the sysop.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    goodbye();
    return 0;
}

/* ANSI: the game runs here on the BBS, and the player sees it as text-mode pictures. */
static int play_ansi(int choice, bool utf8)
{
    ansi_mode_t mode = choice == CHOICE_24BIT ? MODE_24BIT : choice == CHOICE_256 ? MODE_256 : MODE_16;
    play_result_t result;

    title();
    door_write(CSI "0;37m  Saved games for: ");
    door_write(saves_player());
    door_write("\r\n\r\n");
    door_write("  Arrows or WASD move, F fires, Space opens doors, 1-7 pick a weapon,\r\n");
    door_write("  Tab shows the map and Esc brings up the menu. Quit from the menu, or\r\n");
    door_write("  press Ctrl-Q, to come back. ` shows frames per second.\r\n\r\n");
    door_write(CSI "1;37m  Press any key to start." CSI "0m");
    if (door_read_char() < 0)
        return 0;

    result = ansi_play(mode, utf8);
    if (result == PLAY_HANGUP)
        return 0;
    if (result == PLAY_FAILED)
    {
        title();
        door_write(CSI "1;33m  DOOM couldn't be started. Please tell the sysop.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    goodbye();
    return 0;
}

int main(int argc, char *argv[])
{
    terminal_t term = { 0 };
    bool utf8 = false;
    int choice, status;

    door_init(argc > 1 ? argv[1] : NULL);

    /* Saved games belong to the player, not to the machine they called from */
    saves_init(door_info.handle, door_info.user_record);

    title();

    if (!trace_doom_load_files()) {
        door_write(CSI "1;33m  This door isn't installed properly: doom.wasm or doom1.wad is missing.\r\n" CSI "0m");
        door_write("  Please tell the sysop.\r\n");
        press_any_key();
        door_cleanup();
        return 1;
    }

    /* No screen of its own: the menu shows what was found */
    term.trace = trace_doom_detect();
    detect_cterm(&term);
    detect_pixels(&term);

    choice = choose_display(&term, &utf8);
    if (choice == 0)
        status = 0;
    else if (choice == CHOICE_TRACE)
        status = play_trace();
    else if (choice == CHOICE_JXL)
        status = play_jxl(&term);
    else
        status = play_ansi(choice, utf8);

    door_cleanup();
    return status;
}
