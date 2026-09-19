# DOOM for TERMinator — a BBS door

DOOM, playable from a BBS: the door sends the game to the caller's terminal and it runs there, at its proper 35 frames
a second with sound, instead of streaming pictures down the line. This is the source for both halves.

**This repository exists so that anyone who plays the door can have the source of the game they were sent**, which is
what the GNU GPL asks for. See [Licensing](#licensing). It lives at https://github.com/omniphil/DOOMDoor, which is the
address the door itself gives out.

## What's here

| Folder | What it is |
|---|---|
| `module/` | The game as TERMinator runs it: Crispy Doom compiled to WebAssembly, with the platform layer that replaces SDL. Built to `doom.wasm`. |
| `door/` | The BBS door, which sends the game and the WAD, starts it, keeps each player's savegames, and waits for them to quit. For terminals without TRACE it also runs the game itself and sends it as ANSI (see below). |
| `third_party/crispy-doom-7.1/` | Crispy Doom 7.1, unmodified, exactly as the module is built against. |
| `module/trace/trace_api.h` | The engine API the module is written against, copied from TERMinator so this source builds on its own. |

`module/README.md` has a file-by-file account of what was replaced and why, including the three things WebAssembly
made awkward (indirect calls, no file system, no console).

## Building

The game module needs [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) (34 was used here):

```
cd module
make            # -> doom.wasm
```

The door is ordinary C for the BBS machine. It also compiles the game itself from `module/` and `third_party/`, for
its ANSI mode:

```
cd door
make            # -> doomdoor
```

Put `doom.wasm` and a shareware `doom1.wad` beside `doomdoor`; `door/INSTALL.md` covers the rest.

## ANSI mode: for terminals without TRACE

A caller whose terminal can't run the game gets it anyway: the door runs the very same module code natively on the
BBS (`door/ansi_host.c` answers its `trace_*` calls the way TERMinator would) and sends each frame as ANSI, in
24-bit colour, xterm's 256 colours, or the 16 colours and CP437 blocks every BBS terminal has. The player picks on
the start page, which marks what their terminal was detected as supporting. The status bar, messages and menus are
drawn as text, since Doom's own are pictures of text that can't be read at 80x24. No sound. Details in
`door/INSTALL.md`.

## Which binary this is the source of

The build published with the door at the time of writing:

| File | Size | SHA-256 |
|---|---|---|
| `doom.wasm` | 954216 | `f49237badb1962aa70eeac5f05b72a39382be2b3c74c56c406300be550199a53` |

A build of this source won't match that hash byte for byte — compilers don't work that way — but it is the same
program. If you want to check what a door actually sent you, TERMinator caches it by hash under
`%APPDATA%\TERMinator\trace\cache\`.

## The WAD is not here

`doom1.wad` (the shareware episode, SHA-256 `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`) is id Software's data, not source, and is not part of this
repository. It may be passed on unchanged and not for profit, which is what the door does. Commercial IWADs
(`doom.wad`, `doom2.wad`) may not be distributed at all and must never be put in the door's folder.

## Licensing

- **`module/` and `third_party/`: GNU GPL version 2** (see `COPYING.md`). Crispy Doom is a fork of Chocolate Doom,
  which descends from id Software's release of the Doom source. The OPL chip emulation inside it is LGPL, which is
  compatible. Anything built from this is GPL-2 as well.
- **`door/`**: written for this project and covered by the same terms, so the whole thing can be passed on together.
  Since the ANSI mode, the door binary has Crispy Doom compiled into it, so it is itself a GPL-2 build of this source.
- **TERMinator itself is a separate program** (also GPL-2, https://github.com/omniphil/TERMinator-Windows). It runs
  the module in a sandbox; the game is not built into it, which is the point: a door brings its own game.

## How it fits together

The terminal provides an engine called TRACE: a door sends a WebAssembly module and any assets it needs, both cached
by hash, and the terminal runs the module on the player's own machine and shows what it draws. The door and the game
then exchange only small messages — which WAD to play, and savegames. After the first call nothing of any size
crosses the wire, which is why it plays at full speed over a modem-era link.
