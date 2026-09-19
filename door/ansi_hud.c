/*
 * ansi_hud.c -- reads Doom's state for the ANSI mode's text status line. See ansi_hud.h.
 *
 * Built with the game's own flags and headers (it is part of the game side of the Makefile), since it looks straight
 * into Doom's globals. It only reads, apart from taking the player's message once it has been shown.
 */

#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_items.h"
#include "p_saveg.h"

#include "ansi_hud.h"

extern boolean menuactive;          /* m_menu.c */
extern int     saveStringEnter;     /* m_menu.c */
extern boolean automapactive;       /* am_map.c */

/* ---- Doom's menus, as m_menu.c defines them (it keeps the types to itself, so they're repeated exactly here) ---- */

typedef struct
{
    short       status;             /* -1: a spacer, not an item */
    char        name[10];           /* the picture the item is drawn with, e.g. M_NGAME */
    void        (*routine)(int choice);
    char        alphaKey;
    const char  *alttext;
} menuitem_t;

typedef struct menu_s
{
    short          numitems;
    struct menu_s *prevMenu;
    menuitem_t    *menuitems;
    void          (*routine)();
    short          x, y;
    short          lastOn;
    short          lumps_missing;
} menu_t;

extern menu_t     *currentMenu;
extern short       itemOn;
extern int         messageToPrint;
extern const char *messageString;
extern char        savegamestrings[10][SAVESTRINGSIZE];
extern menu_t      MainDef, EpiDef, NewDef, OptionsDef, SoundDef, LoadDef, SaveDef, ReadDef1, ReadDef2;

/* What each menu picture says */
static const struct { const char *lump, *text; } MENU_TEXT[] = {
    { "M_NGAME", "New Game" },      { "M_OPTION", "Options" },         { "M_LOADG", "Load Game" },
    { "M_SAVEG", "Save Game" },     { "M_RDTHIS", "Read This!" },      { "M_QUITG", "Quit Game" },
    { "M_EPI1", "Knee-Deep in the Dead" }, { "M_EPI2", "The Shores of Hell" }, { "M_EPI3", "Inferno" },
    { "M_EPI4", "Thy Flesh Consumed" },    { "M_EPI5", "Sigil" },              { "M_EPI6", "Sigil II" },
    { "M_JKILL", "I'm too young to die" }, { "M_ROUGH", "Hey, not too rough" }, { "M_HURT", "Hurt me plenty" },
    { "M_ULTRA", "Ultra-Violence" },       { "M_NMARE", "Nightmare!" },
    { "M_ENDGAM", "End Game" },     { "M_MESSG", "Messages" },         { "M_DETAIL", "Graphic Detail" },
    { "M_SCRNSZ", "Screen Size" },  { "M_MSENS", "Mouse Sensitivity" }, { "M_SVOL", "Sound Volume" },
    { "M_CRISPY", "Crispness" },    { "M_SFXVOL", "Sfx Volume" },      { "M_MUSVOL", "Music Volume" },
};

static void read_menu(ansi_menu_t *m)
{
    const menu_t *menu = currentMenu;

    memset(m, 0, sizeof(*m));

    /* A question from the game takes over, as it does on Doom's own screen */
    if (messageToPrint && messageString != NULL)
    {
        const char *p = messageString;
        while (*p && m->message_lines < MESSAGE_LINES)
        {
            const char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= sizeof(m->message[0]))
                len = sizeof(m->message[0]) - 1;
            memcpy(m->message[m->message_lines], p, len);
            m->message[m->message_lines][len] = '\0';
            m->message_lines++;
            if (end == NULL)
                break;
            p = end + 1;
        }
        m->show = true;
        return;
    }

    if (!menuactive || menu == NULL)
        return;

    m->show = true;
    snprintf(m->title, sizeof(m->title), "%s",
             menu == &MainDef ? "DOOM" : menu == &EpiDef ? "Which episode?" : menu == &NewDef ? "Choose skill level" :
             menu == &OptionsDef ? "Options" : menu == &SoundDef ? "Sound volume" : menu == &LoadDef ? "Load game" :
             menu == &SaveDef ? "Save game" : menu == &ReadDef1 || menu == &ReadDef2 ? "Help" : "Crispness");
    m->selected = itemOn;
    m->count = menu->numitems < MENU_MAX_ITEMS ? menu->numitems : MENU_MAX_ITEMS;

    for (int i = 0; i < m->count; i++)
    {
        const menuitem_t *item = &menu->menuitems[i];
        char *out = m->items[i];

        if (item->status == -1)
            continue;                               /* a spacer */
        if (menu == &LoadDef || menu == &SaveDef)
        {
            snprintf(out, MENU_ITEM_LEN, "%s%s", savegamestrings[i],
                     saveStringEnter && i == itemOn ? "_" : "");
            continue;
        }
        for (size_t t = 0; t < sizeof(MENU_TEXT) / sizeof(MENU_TEXT[0]); t++)
            if (!strcmp(item->name, MENU_TEXT[t].lump))
            {
                snprintf(out, MENU_ITEM_LEN, "%s", MENU_TEXT[t].text);
                break;
            }
        if (!out[0])
            snprintf(out, MENU_ITEM_LEN, "%s", item->alttext ? item->alttext : "(option)");
    }
    if (menu == &ReadDef1 || menu == &ReadDef2)
    {
        m->count = 1;
        snprintf(m->items[0], MENU_ITEM_LEN, "Press Enter to go on");
        m->selected = 0;
    }
}

void ansi_hud_read(ansi_hud_t *hud)
{
    player_t *p = &players[consoleplayer];

    memset(hud, 0, sizeof(*hud));
    hud->in_level = gamestate == GS_LEVEL;
    hud->menu = menuactive;
    hud->typing = saveStringEnter != 0;
    hud->automap = automapactive;
    hud->episode = gameepisode;
    hud->map = gamemap;

    read_menu(&hud->menu_view);

    if (!hud->in_level)
        return;

    hud->dead = p->playerstate == PST_DEAD;
    hud->health = p->health;
    hud->armor = p->armorpoints;
    {
        ammotype_t type = weaponinfo[p->readyweapon].ammo;
        hud->ammo = type == am_noammo ? -1 : p->ammo[type];
    }
    for (int i = 0; i < 4; i++)
    {
        hud->ammo_have[i] = p->ammo[i];
        hud->ammo_max[i] = p->maxammo[i];
    }

    /* The status bar's ARMS block: 2 pistol, 3 shotguns, 4 chaingun, 5 rockets, 6 plasma, 7 BFG */
    hud->arms[0] = p->weaponowned[wp_pistol];
    hud->arms[1] = p->weaponowned[wp_shotgun] || p->weaponowned[wp_supershotgun];
    hud->arms[2] = p->weaponowned[wp_chaingun];
    hud->arms[3] = p->weaponowned[wp_missile];
    hud->arms[4] = p->weaponowned[wp_plasma];
    hud->arms[5] = p->weaponowned[wp_bfg];

    switch (p->readyweapon)
    {
    case wp_fist: case wp_chainsaw:          hud->weapon = 1; break;
    case wp_pistol:                          hud->weapon = 2; break;
    case wp_shotgun: case wp_supershotgun:   hud->weapon = 3; break;
    case wp_chaingun:                        hud->weapon = 4; break;
    case wp_missile:                         hud->weapon = 5; break;
    case wp_plasma:                          hud->weapon = 6; break;
    case wp_bfg:                             hud->weapon = 7; break;
    default:                                 hud->weapon = 0; break;
    }

    hud->keys[0] = p->cards[it_bluecard] || p->cards[it_blueskull];
    hud->keys[1] = p->cards[it_yellowcard] || p->cards[it_yellowskull];
    hud->keys[2] = p->cards[it_redcard] || p->cards[it_redskull];

    /* Doom's own messages are switched off in ANSI mode (they'd be unreadable at this size), so they stay on the
     * player until taken here, and are shown as text instead. */
    if (p->message != NULL)
    {
        snprintf(hud->message, sizeof(hud->message), "%s", p->message);
        p->message = NULL;
    }
}
