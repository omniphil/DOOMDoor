# The DOOM door. For TERMinator it sends doom.wasm and doom1.wad, which run on the player's own machine (see
# ../module). For every other terminal it runs the same game itself and sends it as JPEG XL pictures with sound
# (pix_*.c) or as ANSI (ansi_*.c), so the game's sources are built in here too. Built for the BBS box; the same C
# builds anywhere POSIX. libjxl is loaded at run time (jxl_enc.c), so only its headers are needed, and they're here.
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
LDFLAGS = -pthread -lm -ldl

# Doom's sound calls go through pix_hooks.c first, for the JPEG XL mode (which plays sound on the caller's terminal),
# and so does its end-of-frame call, so IDRATE can show the frames that reached the caller
WRAP = I_StartSound I_UpdateSoundParams I_StopSound I_SoundIsPlaying \
       I_RegisterSong I_UnRegisterSong I_PlaySong I_StopSong I_PauseSong I_ResumeSong I_SetMusicVolume \
       I_FinishUpdate
WRAP_FLAGS = $(foreach f, $(WRAP), -Wl,--wrap=$(f))

# The game's sources: the development tree when there is one (so edits there are always what gets built), otherwise
# the copy `make bundle` puts in native/ for the BBS
MODULE = $(firstword $(wildcard ../module native/module))
CRISPY = $(firstword $(wildcard ../third_party/crispy-doom-7.1 native/crispy))
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include native/trace ../module/trace))

DOOR_OBJ = main.o door.o trace_doom.o trace_door.o saves.o sha256.o ansi_host.o ansi_screen.o ansi_input.o ansi_play.o \
           apc.o jxl_enc.o pix_play.o pix_sound.o

# ---- the game, compiled for this machine ----
# The same files and flags ../module/Makefile builds into doom.wasm, only for a normal CPU rather than wasm32.
GAME_CFLAGS = -O2 -pthread -std=gnu11 -w \
              -DDISABLE_SDL2MIXER=1 -DDISABLE_SDL2NET=1 -DHAVE_CONFIG_H=1 \
              -include tracedoom_compat.h -include ansi_compat.h -I $(MODULE)/include -I $(MODULE)/include/SDLshim -I $(MODULE)/src \
              -I $(TRACE_INCLUDE) -I $(CRISPY)/src -I $(CRISPY)/src/doom -I $(CRISPY)/opl -I $(CRISPY)/pcsound \
              -I $(CRISPY)/textscreen -I .

COMMON = crispy.c i_system.c m_argv.c m_misc.c a11y.c aes_prng.c d_event.c d_loop.c d_mode.c deh_str.c \
         i_oplmusic.c i_sound.c i_truecolor.c midifallback.c midifile.c mus2mid.c \
         m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c memio.c tables.c \
         v_diskicon.c v_video.c v_trans.c w_checksum.c w_main.c w_wad.c w_file.c w_merge.c z_zone.c sha1.c \
         net_client.c net_common.c net_io.c net_loop.c net_packet.c net_petname.c net_query.c \
         net_server.c net_structrw.c net_dedicated.c p_rejectpad.c \
         deh_io.c deh_main.c deh_mapping.c deh_text.c
OVERRIDDEN = p_mobj.c p_pspr.c
GAME_SRC = $(addprefix $(CRISPY)/src/, $(COMMON)) \
           $(filter-out $(addprefix $(CRISPY)/src/doom/, $(OVERRIDDEN)), $(wildcard $(CRISPY)/src/doom/*.c)) \
           $(addprefix $(MODULE)/src/crispy_overrides/, $(OVERRIDDEN)) \
           $(CRISPY)/opl/opl.c $(CRISPY)/opl/opl3.c $(CRISPY)/opl/opl_queue.c \
           $(wildcard $(MODULE)/src/*.c) \
           ansi_hud.c ansi_compat.c pix_hooks.c

# In a folder of their own, so the module's saves.c can't be confused with the door's
GAME_OBJ = $(patsubst %.c, game/%.o, $(notdir $(GAME_SRC)))
vpath %.c $(MODULE)/src $(MODULE)/src/crispy_overrides $(CRISPY)/src $(CRISPY)/src/doom $(CRISPY)/opl

doomdoor: $(DOOR_OBJ) $(GAME_OBJ)
	$(CC) $(DOOR_OBJ) $(GAME_OBJ) -o $@ $(LDFLAGS) $(WRAP_FLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# trace_api.h marks its functions for WebAssembly, which gcc doesn't know and would warn about
ansi_host.o: ansi_host.c
	$(CC) $(CFLAGS) -Wno-attributes -I $(TRACE_INCLUDE) -c $< -o $@

jxl_enc.o: jxl_enc.c
	$(CC) $(CFLAGS) -I jxl_include -c $< -o $@

# Renders the WAD's music for the JPEG XL mode (tools/musrender.c, then tools/make_music.py). Development only: the
# results are in music/, so the BBS never builds this.
musrender: tools/musrender.o saves.o sha256.o ansi_host.o trace_doom.o trace_door.o door.o $(GAME_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS) $(WRAP_FLAGS) -Wl,--wrap=trace_present,--wrap=trace_audio_room,--wrap=trace_audio_write

# The module's two overridden files share names with Crispy's, so they are built from their own folder
game/p_mobj.o: $(MODULE)/src/crispy_overrides/p_mobj.c | game
	$(CC) $(GAME_CFLAGS) -c $< -o $@
game/p_pspr.o: $(MODULE)/src/crispy_overrides/p_pspr.c | game
	$(CC) $(GAME_CFLAGS) -c $< -o $@

# And the module's saves.c shares a name with the door's own
game/saves.o: $(MODULE)/src/saves.c | game
	$(CC) $(GAME_CFLAGS) -c $< -o $@

# opl.c picks its chip driver from a list that is empty without this (see ../module/Makefile)
game/opl.o: $(CRISPY)/opl/opl.c | game
	$(CC) $(GAME_CFLAGS) -UDISABLE_SDL2MIXER -c $< -o $@

game/%.o: %.c | game
	$(CC) $(GAME_CFLAGS) -c $< -o $@

game:
	mkdir -p game

# Everything the sysop needs in one folder: the door, the game and the WAD. On the BBS these two files have usually
# come along with the folder already, and there is no ../module to copy from, so this only fills in what's missing.
install: doomdoor
	@if [ -f doom.wasm ]; then echo "doom.wasm: already here"; \
	elif [ -f ../module/doom.wasm ]; then cp ../module/doom.wasm . && echo "doom.wasm: copied"; \
	else echo "doom.wasm: MISSING -- copy it in from the development machine"; fi
	@if [ -f doom1.wad ]; then echo "doom1.wad: already here"; \
	elif [ -f ../wads/doom1.wad ]; then cp ../wads/doom1.wad . && echo "doom1.wad: copied"; \
	else echo "doom1.wad: MISSING -- copy it in from the development machine"; fi

# Copies the game's sources into native/, so this folder builds on the BBS box on its own (run on the development
# machine before copying the folder over). Only what the build uses: no SDL, no other games, no docs.
bundle:
	rm -rf native
	mkdir -p native/module native/crispy/src/doom native/crispy/opl native/crispy/pcsound native/crispy/textscreen native/trace
	cp -r ../module/src ../module/include native/module/
	cp ../third_party/crispy-doom-7.1/src/*.c ../third_party/crispy-doom-7.1/src/*.h native/crispy/src/
	cp ../third_party/crispy-doom-7.1/src/doom/*.c ../third_party/crispy-doom-7.1/src/doom/*.h native/crispy/src/doom/
	cp ../third_party/crispy-doom-7.1/opl/*.c ../third_party/crispy-doom-7.1/opl/*.h native/crispy/opl/
	cp ../third_party/crispy-doom-7.1/pcsound/*.h native/crispy/pcsound/
	cp ../third_party/crispy-doom-7.1/textscreen/*.h native/crispy/textscreen/
	cp ../third_party/crispy-doom-7.1/COPYING.md ../third_party/crispy-doom-7.1/AUTHORS native/crispy/
	cp ../../../TERMinator-Windows/trace/include/trace_api.h native/trace/
	@du -sh native

clean:
	rm -rf $(DOOR_OBJ) game doomdoor musrender tools/*.o

.PHONY: clean install bundle
