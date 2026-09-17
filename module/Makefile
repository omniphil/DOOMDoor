# Builds doom.wasm: the whole of Doom (game, renderer and sound) as a TRACE module the door sends to TERMinator.
#
# Crispy Doom 7.1 comes from ../third_party unmodified. Everything platform-specific is replaced by src/*_trace.c,
# which talk to the TRACE API instead of SDL; the handful of SDL calls left in shared files are satisfied by
# include/SDLshim. Run in WSL/Linux with wasi-sdk.
WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CC = $(WASI_SDK)/bin/clang

CRISPY = ../third_party/crispy-doom-7.1
# trace_api.h: the client's own tree when building here, or the copy bundled with the published source, so the
# released source builds on its own without a checkout of TERMinator beside it.
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) trace)

CFLAGS = --target=wasm32-wasip1-threads -O2 -pthread -std=gnu11 \
         -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
         -DDISABLE_SDL2MIXER=1 -DDISABLE_SDL2NET=1 -DHAVE_CONFIG_H=1 \
         -include tracedoom_compat.h -I include -I include/SDLshim -I src -I $(TRACE_INCLUDE) \
         -I $(CRISPY)/src -I $(CRISPY)/src/doom -I $(CRISPY)/opl -I $(CRISPY)/pcsound -I $(CRISPY)/textscreen
LDFLAGS = -mexec-model=reactor -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=268435456 \
          -Wl,--export=wasi_thread_start -Wl,--strip-all -Wl,-z,stack-size=8388608

# Crispy's portable code: the game, the renderer, sound logic, WAD handling, Dehacked
COMMON = crispy.c i_system.c m_argv.c m_misc.c a11y.c aes_prng.c d_event.c d_loop.c d_mode.c deh_str.c \
         i_oplmusic.c i_sound.c i_truecolor.c midifallback.c midifile.c mus2mid.c \
         m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c memio.c tables.c \
         v_diskicon.c v_video.c v_trans.c w_checksum.c w_main.c w_wad.c w_file.c w_merge.c z_zone.c sha1.c \
         net_client.c net_common.c net_io.c net_loop.c net_packet.c net_petname.c net_query.c \
         net_server.c net_structrw.c net_dedicated.c p_rejectpad.c \
         deh_io.c deh_main.c deh_mapping.c deh_text.c
COMMON_SRC = $(addprefix $(CRISPY)/src/, $(COMMON))

# The game itself. Two files come from src/crispy_overrides instead: they call Doom's action functions through
# action_dispatch.c, because WebAssembly won't allow the signature mismatch Crispy relies on (see that file).
OVERRIDDEN = p_mobj.c p_pspr.c
GAME_SRC = $(filter-out $(addprefix $(CRISPY)/src/doom/, $(OVERRIDDEN)), $(wildcard $(CRISPY)/src/doom/*.c)) \
           $(addprefix src/crispy_overrides/, $(OVERRIDDEN))

# The OPL chip emulation; its SDL backend is replaced by src/opl_trace.c
# opl_timer.c drives a real OPL card from a thread; only the emulated chip is used here
OPL_SRC = $(CRISPY)/opl/opl.c $(CRISPY)/opl/opl3.c $(CRISPY)/opl/opl_queue.c

# Our platform layer: everything Doom would normally get from SDL and the operating system
TRACE_SRC = $(wildcard src/*.c)

SRC = $(COMMON_SRC) $(GAME_SRC) $(OPL_SRC) $(TRACE_SRC)

OBJ = $(patsubst %.c, obj/%.o, $(notdir $(SRC)))
VPATH = src:src/crispy_overrides:$(CRISPY)/src:$(CRISPY)/src/doom:$(CRISPY)/opl

doom.wasm: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@

# -MMD writes each object's header dependencies, so editing a header rebuilds what uses it
obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

# opl.c picks its chip driver from a list that is empty without this; ours is src/opl_trace.c, registered in the
# slot the list already has (see the comment there).
obj/opl.o: $(CRISPY)/opl/opl.c | obj
	$(CC) $(CFLAGS) -UDISABLE_SDL2MIXER -c $< -o $@

obj:
	mkdir -p obj

clean:
	rm -rf obj doom.wasm

.PHONY: clean
