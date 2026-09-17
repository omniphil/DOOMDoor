# Installing the DOOM door on the BBS

The door doesn't run the game. It sends it: TERMinator runs the whole of DOOM on the player's own machine, so the BBS
only pays for one upload per player, ever. A player on any other terminal gets a polite screen explaining that.

## What the BBS needs

| File | Where it comes from | Size |
|---|---|---|
| `doomdoor` | built here with `make` | ~30 KB |
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

## What a player sees

- **First call:** the WAD goes up once, with a progress bar (4.2 MB, about half a minute on a typical link). Then DOOM
  starts.
- **Every call after that:** both files are already cached on their machine, keyed by hash, so the game starts at once
  and almost nothing crosses the wire while they play.
- **Quitting:** they quit from DOOM's own menu, and the door takes them back to the BBS.

## Requirements and limits

- The player needs a **TERMinator build with TRACE** (1.1.2 or newer). Older versions, and every other terminal, get
  the "this door needs TERMinator" screen.
- **Shareware DOOM only.** `doom1.wad` is the episode id Software gave away and allowed to be passed on unchanged.
  Never put a commercial IWAD here: those may not be distributed, and the door would be handing out copies.
- **Licensing: see `LICENSE.md`.** The game is GPL-2 (Crispy Doom), so the source has to be available to players;
  `COPYING-crispy-doom.md` and `AUTHORS-crispy-doom.txt` travel with the door.
- Single player for now.
