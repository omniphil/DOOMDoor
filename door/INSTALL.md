# Installing the DOOM door on the BBS

The door doesn't run the game. It sends it: TERMinator runs the whole of DOOM on the player's own machine, so the BBS
only pays for one upload per player, ever. A player on any other terminal gets a polite screen explaining that.

## What the BBS needs

| File | Where it comes from | Size |
|---|---|---|
| `doomdoor` | built here with `make` (the door plus DOOM itself, for ANSI mode) | ~1.3 MB |
| `doom.wasm` | `../module` (`make` there), copied by `make install` | ~950 KB |
| `doom1.wad` | `../wads`, copied by `make install` | 4.2 MB |

All three sit in the same folder. The door looks for the other two beside its own binary.

## Steps

1. **Copy the `door/` folder to the BBS box.** Build it there rather than copying a binary over: the development
   machine runs a newer Ubuntu than the board, and a binary built on the newer one may not start on it.
2. On the BBS, in that folder:

   ```
   make clean      # throws away anything built on the other machine
   make            # builds doomdoor
   make install    # checks doom.wasm and doom1.wad are here, and says so if either is missing
   ```

   `make clean` matters: if the folder came from the development machine it has object files built there, and make
   would otherwise reuse them. `make install` doesn't touch anything else on the BBS.
3. **Add a door entry in Mystic** that runs `doomdoor` with the drop-file path as its only argument, the same shape as
   the other doors here:

   ```
   /path/to/doom/doomdoor %# 
   ```

   The door reads `door32.sys` from the folder it's given (or from the current directory), for the player's name and
   how much time they have left. Without a drop file it runs in local mode, which is handy for testing over SSH.

## Checking it works

Before letting players in, run the test that impersonates TERMinator. It needs no BBS and no terminal:

```
python3 test_door.py            # the full exchange: the game and the WAD must arrive intact
python3 test_door.py --plain    # a terminal with no TRACE: the door should bow out politely
```

## Savegames

The game has nowhere to keep a save of its own, so saves come back here and are kept **per player**, in
`saves/<handle>-<user number>/doomsav<n>.dsg` beside the door binary, made on the first call. Both parts come from the
drop file: the handle cut down to plain characters, and the BBS's own user number, which keeps two players apart even
if their handles reduce to the same thing. A player's games therefore follow them, whichever machine they call from.

Nothing else travels while they play: at the start the door sends only each save's 24-character description, which is
what Doom's Load menu shows, and a save itself is only sent if they actually open it.

### Checking it's really per player

The door prints **"Saved games for: &lt;name&gt;"** as the game starts. On a real call that should be the caller's handle
followed by their BBS user number, e.g. `phil-1`.

If it says **`player`**, the door didn't find a `door32.sys` drop file, so it doesn't know who is calling — and every
caller would then share one set of saves. That's expected when you run `./doomdoor` yourself over SSH, but on a real
call it means the Mystic door entry isn't giving the door its drop file. Copy the settings from a door that already
works (Fractals uses the same drop file and the same code), or pass the drop-file folder as the door's argument.

## ANSI mode: DOOM for every other terminal (added 2026-09-19)

Callers without TRACE (SyncTERM, NetRunner, PuTTY...) can still play: the door runs DOOM **here on the BBS** and sends
it to them as ANSI pictures. The game is the same code as `doom.wasm`, compiled into `doomdoor` itself (`ansi_*.c`),
so the folder carries the game's sources in `native/` (about 4 MB) and `make` builds them. On the development machine,
run `make bundle` in `door/` to refresh `native/` before copying the folder over.

The start page lets each caller pick, and marks what their terminal was detected as supporting:

| Choice | What it is | Bandwidth while moving (measured locally) |
|---|---|---|
| 1. TRACE graphics (640x400 + Sound) | the game runs on their own machine, as before. Only offered to TERMinator with TRACE | almost none |
| 2. JPEG XL graphics (320x200 + Sound) | DOOM's real picture and sound, run here (see below) | 115-700 KB/s, set by the link |
| 3. ANSI 24-bit (best ANSI look) | half-blocks in exact colour: the best-looking ANSI | ~700 KB/s |
| 4. ANSI 256 | the same half-blocks in xterm's 256 colours | ~350 KB/s |
| 5. ANSI 16 | CP437 blocks and shades, fitted per cell. Works in any BBS terminal | ~75 KB/s |

- **24-bit and 256** are marked DETECTED when the terminal answers `ESC [ c` as SyncTERM's CTerm 1.300 or newer
  (TERMinator, SyncTERM 1.2+); otherwise UNKNOWN. Test strips on the menu let the player see which modes work.
- **U** (not shown on the menu) switches the menu and game to UTF-8 block characters, for terminals that don't do CP437.
- The choice is remembered per player in `saves/<player>/display.cfg`.
- **The link paces it:** frames are skipped, never queued, when the connection can't keep up, so a slow link gets
  fewer frames rather than a picture that lags behind.
- **CPU:** about 5-12% of one core per player on a Ryzen 5900X (the 16-colour fit costs the most). No sound is mixed.
- **Screen:** 80x24. Rows 1-22 are the picture; DOOM's status bar and messages are drawn as text on rows 23-24, and
  DOOM's menus as text boxes, since the originals are pictures of text that can't be read at this size.
- **Keys:** arrows or WASD move, F fires, Space opens, 1-7 weapons, Tab map, Esc menu, Ctrl-Q straight back to the
  BBS, ` shows frames per second. A terminal never says when a key is let go, so keys are held until shortly after
  their last repeat (`ansi_input.c`: the hold times are there to tune).
- **Saves** work the same as TRACE's: same folder, same files, whichever way the player plays.
- **Debugging:** `DOOMDOOR_LOG=/some/file ./doomdoor` writes DOOM's own startup messages there.

## JPEG XL mode: DOOM's real picture and sound, run here (added 2026-09-25)

For terminals that speak the CTerm APC picture and sound commands but not TRACE. DOOM runs here as in ANSI mode, but
the caller sees its real 320x200 picture, scaled up by their terminal, with DOOM's own status bar, menus and messages,
and hears its sound effects and music. Sources: `pix_play.c` (pictures, pacing, keys), `pix_sound.c` (sound),
`pix_hooks.c` (catches DOOM's sound calls), `jxl_enc.c`, `apc.c`.

**What the BBS box needs:** libjxl's shared library, which the door loads when it starts (`libjxl.so.0.11` on Ubuntu
25.10, already there as a dependency of other packages). Nothing else to install: libjxl's headers are in
`jxl_include/`. If the library is ever missing, the door still runs and the menu shows this mode as NOT FOUND.

**When it's offered:** the terminal must answer `ESC [ c` as CTerm, then say it draws JPEG XL (`Q;JXL`). Sound is used
when it also plays sound files, Ogg Vorbis and 8-bit WAV (`Q;libsndfile`, `Q;libsndfileFormat`); otherwise the menu
says "no sound". CTerm 1.332+ scales the pictures itself; older ones are sent them pre-scaled, at about twice the bytes.

**How it keeps up** (measured with a simulated terminal, `DOOMDOOR_LOG` numbers):

| Link | Frames a second while moving | Quality it settles at |
|---|---|---|
| 2 MB/s, 20 ms ping | 35 (DOOM's own rate) | JPEG XL distance 1, visually lossless |
| 500 KB/s, 120 ms ping | 34-35 | distance ~2.5 |
| 300 KB/s, 200 ms ping | 33-35 | distance ~5 |
| 150 KB/s, 60 ms ping | 30-35 | distance 8, soft but playable |

- **Only what changed is sent**: the picture is cut into 32x8 tiles; standing still costs 1-3 KB/s.
- **Pacing**: a cursor-position request follows each frame and its answer says it arrived. As many frames are allowed
  on their way as the link's speed times its round trip, so a long ping doesn't cap the frame rate.
- **Quality follows the link**: sharper while every frame goes and no queue builds, softer as soon as one does; a
  still picture is sent once more, sharp, when it settles.
- **Sound**: the 55 effects go up once per caller (about 710 KB, a few seconds, with a progress bar; checked by md5 on
  later calls) and are played by commands of a few dozen bytes. Music is in `music/`: every track rendered ahead of
  time on DOOM's OPL chip and cut into 5-second Ogg Vorbis pieces (8.3 MB in all), each sent only when it's about to be
  played, or earlier while the link has room to spare, and kept in the caller's cache. About 6 KB/s while a track is
  new to them.
- **Keys**: DOOM's own keys, with real presses and releases where the terminal reports them (`CSI = 1 h`); otherwise the
  ANSI mode's keys. Ctrl-Q goes straight back to the BBS. Always-run is on.
- **Debugging**: `DOOMDOOR_LOG=/some/file` also gets frames a second, KB/s, quality and round trip every 5 seconds.
- **Trying a slower link**: a file `saves/<player>/linktest.cfg` with `kbps=500` and `ping=80` (any numbers) makes
  that player's games go through a modelled link of that speed (KB/s) and round trip (ms), keys included. Nobody
  else is affected; delete the file to go back. Their `saves/<player>/jxl.log` shows how the door coped.
- **Re-making the music** (development machine, only if the WAD changes): `make musrender`, then
  `./musrender /tmp/tracks && python3 tools/make_music.py /tmp/tracks music` (needs ffmpeg with libvorbis).

## What a player sees

- **First call:** the WAD goes up once, with a progress bar (4.2 MB, about half a minute on a typical link). Then DOOM
  starts.
- **Every call after that:** both files are already cached on their machine, keyed by hash, so the game starts at once
  and almost nothing crosses the wire while they play.
- **Quitting:** they quit from DOOM's own menu, and the door takes them back to the BBS.

## Requirements and limits

- **TRACE mode** needs a **TERMinator build with TRACE** (1.1.2 or newer). Every other terminal gets the ANSI modes.
- **Shareware DOOM only.** `doom1.wad` is the episode id Software gave away and allowed to be passed on unchanged.
  Never put a commercial IWAD here: those may not be distributed, and the door would be handing out copies.
- **Licensing: see `LICENSE.md`.** The game is GPL-2 (Crispy Doom), and its source is published at
  https://github.com/omniphil/DOOMDoor; `COPYING-crispy-doom.md` and `AUTHORS-crispy-doom.txt` travel with the door.
- Single player for now.

## Parked idea: smoother held keys in ANSI (not applied, 2026-09-19)

A terminal sends one press, pauses for the keyboard's repeat delay (~500 ms), then repeats. `ansi_input.c` lets go
before the repeats arrive, so a held key is briefly released:

| Held 1.2 s, 500 ms repeat delay | down | let go | down again |
|---|---|---|---|
| forward | 0 ms | 300 ms | 500 ms |
| turn | 0 ms | 160 ms | 500 ms |

In play it barely shows: Doom's momentum carries movement through the gap, and its slow start on turns hides most of
the turning pause. The Wolfenstein 3D door fixed the same thing (`../../Wolf3D/door/ansi_input.c`, "How held keys
work" in its INSTALL.md). If it's ever wanted here, the recommended port is **the turn part only** (pulse turns
through the repeat delay), which leaves taps, movement and always-run (`joyb_speed 29`) exactly as they are. Don't
port Wolfenstein's one-shot-per-press fire: Doom fires continuously while fire is held.
