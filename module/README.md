# doom.wasm — the whole of Doom as a TRACE module

Built 2026-09-17. Crispy Doom 7.1 from `../third_party`, compiled to WebAssembly and run by TERMinator's sandbox
(`gamesandbox.exe`) on the player's own machine. The door sends this file and the WAD; nothing else crosses the wire,
so the game runs at its proper 35 frames a second with sound, however slow the BBS link is.

**Game logic is untouched.** Everything replaced is the part that would normally talk to an operating system.

## Build

```
make            # needs wasi-sdk in ~/tools (see ../../../TERMinator-Windows/trace/example/Makefile)
```

Test it headless, without a BBS or a window:

```
cp doom.wasm ../../../TERMinator-Windows/trace/bin/
cd ../../../TERMinator-Windows/trace/bin
gamesandbox_probe.exe gamesandbox.exe doom.wasm -seconds 8 -assets <dir with <sha>.bin> \
    -data wad=<sha256 of doom1.wad> -keys 1@1500,28@2500,28@3500,28@4500 -shot game.bmp
```

(`-keys` presses ESC then Enter three times: the menu, New Game, episode, skill. The screenshot should be E1M1.)

## What's in here

| File | What it replaces | What it does |
|---|---|---|
| `src/tracedoom.c` | `i_main.c` | The TRACE entry points. Doom runs on its own thread, so none of Crispy's code had to be restructured; the door's `wad=<sha256>` starts it. Also sends Doom's messages to TERMinator's log, since a module has no console. |
| `src/i_video_trace.c` | `i_video.c`, `i_input.c` | Turns each finished 8-bit frame into the BGRA one `trace_present` takes, flagged 4:3. Keys arrive as set-1 scancodes, which is close to what vanilla read from the keyboard controller. |
| `src/i_sound_trace.c` | `i_sdlsound.c` | The WAD's DMX sound effects, resampled to 44.1 kHz and mixed with vanilla's volume and left/right separation, then handed to TERMinator's own mixer. |
| `src/opl_trace.c` | `opl_sdl.c` | The emulated OPL chip. Same code as Crispy's SDL backend, but rendered when we ask rather than from a sound-card callback, and with no locks: one thread does everything. |
| `src/w_file_trace.c` | `w_file_stdc.c` | The WAD, read through TRACE by its hash. There is no other file the module could open. |
| `src/ramfs.c` | the file system | A few files kept in memory. Doom writes a temporary MIDI file per music track, plus savegames and its config; this lets all that code work unchanged. |
| `src/saves.c` | — | Savegames, which live on the BBS. The door sends each save's description at startup so Doom's Load menu is right immediately; opening one fetches it, and saving sends it up a few KB per frame. |
| `src/action_dispatch.c` | — | Calls Doom's action functions with their real signature. Crispy calls them all through a three-argument pointer, which a CPU accepts and WebAssembly does not. |
| `src/stubs_trace.c` | `d_iwad.c`, joystick, music packs, ENDOOM | Answers for the things that don't exist here: there is one WAD, no folders to search, no joystick, no network socket. |
| `include/SDLshim/` | SDL | The handful of SDL calls left in Crispy's shared files: byte swapping, `qsort`, a message box. Anything else won't compile, which is deliberate. |
| `include/tracedoom_compat.h` | — | Forced into every file, so `printf` and `fopen` reach the two files above without editing `../third_party`. |
| `src/crispy_overrides/` | `p_mobj.c`, `p_pspr.c` | Identical to Crispy's, except the two lines that call an action function. Changes are marked `[tracedoom]`. |
| `tools/gen_action_table.py` | — | Regenerates `src/action_table.h` from Crispy's source. Run it if the Crispy version changes. |

## Known gaps

- **Settings don't outlive the call** (resolution, key bindings): Doom's config file is written to the in-memory files
  and disappears with them. Savegames *do* survive — they go to the BBS through the door, per player.
- **Sound has been exercised but not listened to.** The mixer runs and the queue stays full; whether the music and the
  effects sound right is for the first real session to say.
- **No mouse.** TERMinator doesn't capture one for modules yet.
- **Single player only.** Doom's own network play is lockstep, so the way to multiplayer is for the door to relay
  inputs between players, as Chocolate Doom's server does — not the BBS-side simulation in `../ARCHITECTURE.md`.
- **Shareware only.** `doom1.wad` may be passed on unchanged; commercial IWADs never travel.
