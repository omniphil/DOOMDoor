# The DOOM door. It doesn't run the game: it sends doom.wasm and doom1.wad to TERMinator, which plays them on the
# player's own machine (see ../module). Built for the BBS box; the same C builds anywhere POSIX.
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
LDFLAGS =

OBJ = main.o door.o trace_doom.o saves.o sha256.o

doomdoor: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Everything the sysop needs in one folder: the door, the game and the WAD. On the BBS these two files have usually
# come along with the folder already, and there is no ../module to copy from, so this only fills in what's missing.
install: doomdoor
	@if [ -f doom.wasm ]; then echo "doom.wasm: already here"; \
	elif [ -f ../module/doom.wasm ]; then cp ../module/doom.wasm . && echo "doom.wasm: copied"; \
	else echo "doom.wasm: MISSING -- copy it in from the development machine"; fi
	@if [ -f doom1.wad ]; then echo "doom1.wad: already here"; \
	elif [ -f ../wads/doom1.wad ]; then cp ../wads/doom1.wad . && echo "doom1.wad: copied"; \
	else echo "doom1.wad: MISSING -- copy it in from the development machine"; fi

clean:
	rm -f $(OBJ) doomdoor

.PHONY: clean install
