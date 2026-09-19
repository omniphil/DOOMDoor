/*
 * ansi_input.c -- a terminal's keystrokes turned into Doom's key presses and releases. See ansi_input.h.
 *
 * Two jobs:
 *   1. Bytes to keys: arrows, function keys and so on arrive as escape sequences, in whichever of the common forms
 *      the terminal uses (ESC [ A, ESC O A, ESC [ 11 ~ ...). A lone ESC is the Escape key once nothing follows it.
 *   2. Keys to Doom: Doom wants to know when a key goes down and when it comes up, and a terminal only ever says
 *      "pressed". So a key is held from its first press until a little after its last repeat: tap an arrow and you
 *      step, hold it and the terminal's key repeat keeps you walking.
 */

#include <ctype.h>
#include <string.h>

#include "ansi_host.h"
#include "ansi_input.h"

/* ---- 1. bytes to keys ---- */

#define ESC_WAIT_MS 60          /* how long a lone ESC waits for the rest of a sequence */

static unsigned char g_seq[16];
static int           g_seq_len;
static long          g_seq_started;
static bool          g_after_cr;    /* telnet sends Enter as CR LF or CR NUL: the second byte isn't a key */

static int  g_keys[64];
static int  g_key_head, g_key_tail;

static void push_key(int key)
{
    int next = (g_key_tail + 1) % 64;
    if (next != g_key_head)
    {
        g_keys[g_key_tail] = key;
        g_key_tail = next;
    }
}

/* A finished CSI (ESC [ ...) or SS3 (ESC O x) sequence */
static void decode_sequence(void)
{
    unsigned char final = g_seq[g_seq_len - 1];
    int number = 0;

    if (g_seq_len >= 2 && g_seq[1] == 'O')
    {
        switch (final)
        {
        case 'A': push_key(KEY_T_UP); break;
        case 'B': push_key(KEY_T_DOWN); break;
        case 'C': push_key(KEY_T_RIGHT); break;
        case 'D': push_key(KEY_T_LEFT); break;
        case 'H': push_key(KEY_T_HOME); break;
        case 'F': push_key(KEY_T_END); break;
        case 'P': push_key(KEY_T_F1); break;
        case 'Q': push_key(KEY_T_F2); break;
        case 'R': push_key(KEY_T_F3); break;
        case 'S': push_key(KEY_T_F4); break;
        default: break;
        }
        return;
    }

    for (int i = 2; i < g_seq_len && isdigit(g_seq[i]); i++)
        number = number * 10 + (g_seq[i] - '0');

    switch (final)
    {
    case 'A': push_key(KEY_T_UP); break;
    case 'B': push_key(KEY_T_DOWN); break;
    case 'C': push_key(KEY_T_RIGHT); break;
    case 'D': push_key(KEY_T_LEFT); break;
    case 'H': push_key(KEY_T_HOME); break;
    case 'F': push_key(KEY_T_END); break;
    case 'K': push_key(KEY_T_END); break;     /* some BBS terminals send End as ESC [ K */
    case '~':
        switch (number)
        {
        case 1: case 7:  push_key(KEY_T_HOME); break;
        case 2:          push_key(KEY_T_INSERT); break;
        case 3:          push_key(KEY_T_DELETE); break;
        case 4: case 8:  push_key(KEY_T_END); break;
        case 5:          push_key(KEY_T_PGUP); break;
        case 6:          push_key(KEY_T_PGDN); break;
        case 11: push_key(KEY_T_F1); break;  case 12: push_key(KEY_T_F2); break;
        case 13: push_key(KEY_T_F3); break;  case 14: push_key(KEY_T_F4); break;
        case 15: push_key(KEY_T_F5); break;  case 17: push_key(KEY_T_F6); break;
        case 18: push_key(KEY_T_F7); break;  case 19: push_key(KEY_T_F8); break;
        case 20: push_key(KEY_T_F9); break;  case 21: push_key(KEY_T_F10); break;
        case 23: push_key(KEY_T_F11); break; case 24: push_key(KEY_T_F12); break;
        default: break;
        }
        break;
    default:
        break;      /* anything else (a terminal's reply to a query, say) is dropped */
    }
}

void ansi_input_feed(const unsigned char *data, int len, long now_ms)
{
    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];

        if (g_seq_len > 0)
        {
            if (g_seq_len == 1)
            {
                if (c == '[' || c == 'O')
                {
                    g_seq[g_seq_len++] = c;
                    continue;
                }
                /* ESC then an ordinary key: Escape, then that key */
                g_seq_len = 0;
                push_key(KEY_T_ESC);
            }
            else
            {
                if (g_seq_len < (int)sizeof(g_seq))
                    g_seq[g_seq_len++] = c;
                /* SS3 is always one byte after the O; CSI ends at its first byte in @..~ */
                if (g_seq[1] == 'O' || (c >= 0x40 && c <= 0x7E))
                {
                    decode_sequence();
                    g_seq_len = 0;
                }
                continue;
            }
        }

        if (c == 27)
        {
            g_seq[0] = c;
            g_seq_len = 1;
            g_seq_started = now_ms;
            g_after_cr = false;
            continue;
        }
        if (g_after_cr && (c == '\n' || c == 0))
        {
            g_after_cr = false;
            continue;
        }
        g_after_cr = c == '\r';
        if (c == '\n')
            c = '\r';
        if (c == 127)
            c = 8;
        push_key(c);
    }
}

int ansi_input_next(long now_ms)
{
    int key;

    if (g_seq_len == 1 && now_ms - g_seq_started >= ESC_WAIT_MS)
    {
        g_seq_len = 0;
        push_key(KEY_T_ESC);
    }
    if (g_key_head == g_key_tail)
        return -1;
    key = g_keys[g_key_head];
    g_key_head = (g_key_head + 1) % 64;
    return key;
}

/* ---- 2. keys to Doom ---- */

/* Set-1 scancodes: what Doom's input layer (module/src/i_video_trace.c) takes */
enum
{
    SC_ESC = 0x01, SC_MINUS = 0x0C, SC_EQUALS = 0x0D, SC_BACKSPACE = 0x0E, SC_TAB = 0x0F, SC_ENTER = 0x1C,
    SC_CTRL = 0x1D, SC_COMMA = 0x33, SC_PERIOD = 0x34, SC_SPACE = 0x39, SC_PAUSE = 0x45,
    SC_UP = 0x48, SC_LEFT = 0x4B, SC_RIGHT = 0x4D, SC_DOWN = 0x50,
    SC_HOME = 0x47, SC_END = 0x4F, SC_PGUP = 0x49, SC_PGDN = 0x51, SC_INSERT = 0x52, SC_DELETE = 0x53,
};

static const unsigned char LETTER_SC[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,   /* a..m */
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* n..z */
};

/*
 * How long a key stays down: from a first press (long enough to register as a step, not so long that a tap turns you
 * a quarter circle), and from each repeat after it (longer than the gap between a terminal's repeats, with room for
 * the network bunching them up). Tuned by feel; the numbers are here to change.
 */
#define HOLD_MOVE_MS    300     /* forward, back, strafe */
#define HOLD_TURN_MS    160
#define HOLD_FIRE_MS    150
#define HOLD_TAP_MS     90      /* use, weapons, map: one press */
#define HOLD_MENU_MS    50      /* menus only look at the press */
#define HOLD_REPEAT_MS  150

typedef struct { bool down; long release_at; } held_t;
static held_t g_held[128];

static void key_up(int sc)
{
    if (g_held[sc].down)
    {
        g_held[sc].down = false;
        ansi_host_key(sc, false);
    }
}

static void press(int sc, int first_ms, long now_ms)
{
    if (sc <= 0 || sc >= 128)
        return;
    if (g_held[sc].down)
    {
        /* A repeat: keep holding, without cutting short the first press's time */
        if (g_held[sc].release_at < now_ms + HOLD_REPEAT_MS)
            g_held[sc].release_at = now_ms + HOLD_REPEAT_MS;
        return;
    }
    g_held[sc].down = true;
    g_held[sc].release_at = now_ms + first_ms;
    ansi_host_key(sc, true);
}

/* A key and its opposite can't both be held: turning right lets go of left at once */
static void press_against(int sc, int opposite, int first_ms, long now_ms)
{
    key_up(opposite);
    press(sc, first_ms, now_ms);
}

static int function_scancode(int key)
{
    if (key >= KEY_T_F1 && key <= KEY_T_F10)
        return 0x3B + (key - KEY_T_F1);
    if (key == KEY_T_F11)
        return 0x57;
    if (key == KEY_T_F12)
        return 0x58;
    return 0;
}

/* The key as itself, for menus and typing */
static int plain_scancode(int key)
{
    switch (key)
    {
    case KEY_T_UP: return SC_UP;         case KEY_T_DOWN: return SC_DOWN;
    case KEY_T_LEFT: return SC_LEFT;     case KEY_T_RIGHT: return SC_RIGHT;
    case KEY_T_HOME: return SC_HOME;     case KEY_T_END: return SC_END;
    case KEY_T_PGUP: return SC_PGUP;     case KEY_T_PGDN: return SC_PGDN;
    case KEY_T_INSERT: return SC_INSERT; case KEY_T_DELETE: return SC_DELETE;
    case KEY_T_ESC: return SC_ESC;
    case '\r': return SC_ENTER;
    case 8: return SC_BACKSPACE;
    case '\t': return SC_TAB;
    case ' ': return SC_SPACE;
    case '-': case '_': return SC_MINUS;
    case '=': case '+': return SC_EQUALS;
    case ',': case '<': return SC_COMMA;
    case '.': case '>': return SC_PERIOD;
    default: break;
    }
    if (key >= '1' && key <= '9')
        return 0x02 + (key - '1');
    if (key == '0')
        return 0x0B;
    if (key < 128 && isalpha(key))
        return LETTER_SC[tolower(key) - 'a'];
    return function_scancode(key);
}

void ansi_input_key(int key, bool menu, bool typing, long now_ms)
{
    if (menu || typing)
    {
        press(plain_scancode(key), HOLD_MENU_MS, now_ms);
        return;
    }

    switch (key)
    {
    /* Moving: arrows, or WASD with the arrows turning */
    case KEY_T_UP: case 'w': case 'W':
        press_against(SC_UP, SC_DOWN, HOLD_MOVE_MS, now_ms);
        return;
    case KEY_T_DOWN: case 's': case 'S':
        press_against(SC_DOWN, SC_UP, HOLD_MOVE_MS, now_ms);
        return;
    case KEY_T_LEFT:
        press_against(SC_LEFT, SC_RIGHT, HOLD_TURN_MS, now_ms);
        return;
    case KEY_T_RIGHT:
        press_against(SC_RIGHT, SC_LEFT, HOLD_TURN_MS, now_ms);
        return;
    case 'a': case 'A': case ',': case '<':
        press_against(SC_COMMA, SC_PERIOD, HOLD_MOVE_MS, now_ms);
        return;
    case 'd': case 'D': case '.': case '>':
        press_against(SC_PERIOD, SC_COMMA, HOLD_MOVE_MS, now_ms);
        return;

    /* Fire is Ctrl in Doom, which a terminal can't send on its own */
    case 'f': case 'F': case 'j': case 'J':
        press(SC_CTRL, HOLD_FIRE_MS, now_ms);
        return;

    /* Use (open doors, flip switches) */
    case ' ': case 'e': case 'E':
        press(SC_SPACE, HOLD_TAP_MS, now_ms);
        return;

    case 'p': case 'P':
        press(SC_PAUSE, HOLD_TAP_MS, now_ms);
        return;

    /* Zooming the map is held like movement */
    case '-': case '_':
        press(SC_MINUS, HOLD_MOVE_MS, now_ms);
        return;
    case '=': case '+':
        press(SC_EQUALS, HOLD_MOVE_MS, now_ms);
        return;

    default:
        press(plain_scancode(key), HOLD_TAP_MS, now_ms);
        return;
    }
}

void ansi_input_release_due(long now_ms)
{
    for (int sc = 1; sc < 128; sc++)
        if (g_held[sc].down && now_ms >= g_held[sc].release_at)
            key_up(sc);
}

void ansi_input_release_all(void)
{
    for (int sc = 1; sc < 128; sc++)
        key_up(sc);
}
