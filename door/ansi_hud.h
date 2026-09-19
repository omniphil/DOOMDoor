/*
 * ansi_hud.h -- what the ANSI mode shows as text instead of Doom's own status bar: health, armour, ammo, weapons,
 * keys and the latest message. Read straight from the game (ansi_hud.c), which runs in this process in ANSI mode.
 */

#ifndef ANSI_HUD_H
#define ANSI_HUD_H

#include <stdbool.h>

/* Doom's menus are drawn as pictures, which can't be read at this size, so the door redraws them as text */
#define MENU_MAX_ITEMS  10
#define MENU_ITEM_LEN   40
#define MESSAGE_LINES   6

typedef struct
{
    bool show;                              /* a menu is up */
    char title[32];
    int  count;                             /* items, including the blanks Doom uses as spacers */
    char items[MENU_MAX_ITEMS][MENU_ITEM_LEN];   /* an empty name is a spacer */
    int  selected;
    int  message_lines;                     /* a question from the game ("quit? press y or n"), shown instead */
    char message[MESSAGE_LINES][72];
} ansi_menu_t;

typedef struct
{
    bool in_level;          /* playing a level, rather than the title, intermission or finale */
    bool menu;              /* Doom's menu is up: keys go to it as they are */
    bool typing;            /* typing a savegame name: letters must stay letters */
    bool automap;
    bool dead;
    int  episode, map;
    int  health, armor;
    int  ammo;              /* for the weapon in hand; -1 for fist and chainsaw */
    int  ammo_have[4], ammo_max[4];   /* bullets, shells, cells, rockets (Doom's own order) */
    bool arms[6];           /* weapons 2..7 as on Doom's status bar */
    int  weapon;            /* the one in hand, 1..7 */
    bool keys[3];           /* blue, yellow, red (card or skull) */
    char message[80];       /* the newest pickup/game message, or empty when there isn't a new one */
    ansi_menu_t menu_view;
} ansi_hud_t;

/* Takes a snapshot of the game. Called from the door's thread while Doom runs on its own; a value can be a tic old,
 * which is fine for a status line. A message is handed over once and then cleared. */
void ansi_hud_read(ansi_hud_t *hud);

#endif
