# Licensing for the DOOM door

Two separate things travel to the player, under two different licences.

## `doom.wasm` — the game: GNU GPL version 2

It's built from **Crispy Doom 7.1**, a fork of Chocolate Doom, which descends from id Software's own release of the
Doom source code. All of it is GPL-2, and so is everything built from it, including this module. The OPL chip
emulation inside it (Nuked OPL3) is LGPL, which is compatible.

That is free to use, change and pass on. The one obligation is **source**: because the door sends the compiled game to
every player, that counts as distributing it, so the matching source has to be available to them.

What satisfies it:

1. The module's own source is `../module/` (the platform layer) plus the unmodified Crispy Doom 7.1 in
   `../third_party/`, and `../module/README.md` says exactly which files change what.
2. **Publish that source somewhere players can get it** — the same GitHub account as TERMinator is the obvious home —
   and keep the version that's published matching the `doom.wasm` the door is handing out.
3. Keep this file, Crispy's `COPYING.md` and its `AUTHORS` with the door, so the licence travels with the game.

TERMinator is GPL-2 as well, so nothing here conflicts with it. The engine and its sandbox stay separate programs that
happen to run the module, which is exactly how a door's own code is meant to reach a player.

## `doom1.wad` — the game data: id Software's shareware terms

The shareware episode ("Knee-Deep in the Dead") was given away by id and may be passed on **unchanged and not for
profit**, which is what this door does. It is not GPL and never became free software: only the engine did.

**Never put `doom.wad`, `doom2.wad` or any other commercial IWAD in this folder.** Those may not be distributed, and
the door would be sending a copy to every player who opened it. If support for a player's own commercial WAD is ever
wanted, it has to be read from their own machine and never travel over the wire.
