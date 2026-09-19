#!/usr/bin/env python3
"""
Pretends to be TERMinator, so the door can be tested without a BBS or a terminal.

It runs the door on a pseudo-terminal, answers its TRACE commands the way TERMinator would, and checks that what the
door uploads really is doom.wasm and doom1.wad, byte for byte. Run it after changing anything in the door:

    python3 test_door.py            # a terminal with TRACE: the full exchange
    python3 test_door.py --plain    # a terminal without it: TRACE is marked NOT FOUND on the menu
"""

import base64
import hashlib
import os
import pty
import re
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DOOR = os.path.join(HERE, 'doomdoor')
APC = '\x1b_'
ST = '\x1b\\'

INFO = 'TERMinator:TRACE;Info;v=1;wasm=1;audio=1;assets=1;send=1;store=1;tick=1'

# The size of savegame piece the real module sends (module/src/saves.c CHUNK, door/saves.h SAVES_CHUNK).
# They have to agree: a piece bigger than the door can decode is dropped, which corrupts the save.
SAVE_CHUNK = 3000


def sha256_file(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def fields(text):
    out = {}
    for part in text.split(';'):
        if '=' in part:
            key, value = part.split('=', 1)
            out[key] = value
    return out


class FakeTerminal:
    def __init__(self, plain=False):
        self.plain = plain
        self.left_menu = False
        self.uploads = {}       # name -> bytes being collected ('module' or an asset hash)
        self.stored = {}        # what finished uploading, by hash
        self.started = None     # the WAD hash the module was told to play
        self.buffer = ''
        self.screen = ''
        self.listed = {}        # saves the door says this player has
        self.fetched = {}       # a save the door sent back

    def reply(self, fd, text):
        os.write(fd, (APC + text + ST).encode())

    def handle(self, fd, command):
        verb = command.split(';')[0]
        args = fields(command)

        if verb == 'Query':
            if not self.plain:
                self.reply(fd, INFO)
        elif verb == 'Asset':
            key = args['sha256']
            if key in self.stored:
                self.reply(fd, f'TERMinator:TRACE;Have;module=doom;sha256={key}')
            else:
                self.uploads[key] = bytearray()
                self.reply(fd, f'TERMinator:TRACE;NeedAsset;module=doom;sha256={key}')
        elif verb == 'Open':
            key = args['wasm']
            if key in self.stored:
                self.reply(fd, 'TERMinator:TRACE;Ready;module=doom')
            else:
                self.uploads['module:' + key] = bytearray()
                self.reply(fd, f'TERMinator:TRACE;Need;module=doom;wasm={key}')
        elif verb == 'Put':
            key = args.get('asset') or 'module:' + next(k[7:] for k in self.uploads if k.startswith('module:'))
            chunk = base64.b64decode(args['data'])
            assert int(args['offset']) == len(self.uploads[key]), 'chunks arrived out of order'
            self.uploads[key] += chunk
        elif verb == 'PutDone':
            key = args.get('asset') or next(k for k in self.uploads if k.startswith('module:'))
            data = bytes(self.uploads.pop(key))
            digest = hashlib.sha256(data).hexdigest()
            expect = key.split(':')[-1]
            assert digest == expect, f'what arrived does not match its hash ({digest} vs {expect})'
            self.stored[expect] = data
            if key.startswith('module:'):
                self.reply(fd, 'TERMinator:TRACE;Ready;module=doom')
            else:
                self.reply(fd, f'TERMinator:TRACE;Have;module=doom;sha256={expect}')
        elif verb == 'Data':
            if 'wad' in args:
                self.started = args['wad']
                # The player plays for a moment, saves a game, loads it back, and quits
                time.sleep(0.2)
                self.play(fd)
                self.reply(fd, 'TERMinator:TRACE;Closed;module=doom;code=0')
            elif 'b64' in args:
                # Something the door sent the game: the list of saves it holds for this player
                message = base64.b64decode(args['b64'])
                head = message.split(b'\n', 1)[0].decode('latin-1')
                body = message.split(b'\n', 1)[1] if b'\n' in message else b''
                if head.startswith('list'):
                    self.listed[int(head.split('slot=')[1])] = body
                elif head.startswith('data'):
                    fields_ = dict(p.split('=') for p in head.split()[1:])
                    self.fetched.setdefault(int(fields_['slot']), bytearray())[int(fields_['off']):] = body
        elif verb == 'Close':
            pass

    def module_says(self, fd, head, payload=b''):
        """Pretends to be the game talking to the door, which TERMinator relays as base64."""
        message = head.encode() + (b'\n' + payload if payload else b'')
        self.reply(fd, 'TERMinator:TRACE;Data;module=doom;b64=' + base64.b64encode(message).decode())

    def play(self, fd):
        """The player saves a game and loads it back, which is all the door has to handle while they play."""
        save = bytes(range(256)) * 40          # stands in for a real savegame
        save = b'E1M1 test save         ' + b'\x00' + save[24:]

        # First, a save that loses a piece on the way: the door must refuse it rather than keep a holed savegame.
        # (This is what actually went wrong on the first live call.)
        for off in range(0, len(save), SAVE_CHUNK):
            if off == SAVE_CHUNK:
                continue                       # this piece never arrives
            self.module_says(fd, f'put slot=5 off={off} total={len(save)}', save[off:off + SAVE_CHUNK])
        time.sleep(0.3)

        # Then a good one, in the pieces the real game sends
        for off in range(0, len(save), SAVE_CHUNK):
            self.module_says(fd, f'put slot=3 off={off} total={len(save)}', save[off:off + SAVE_CHUNK])
        self.saved = save
        time.sleep(0.3)
        self.module_says(fd, 'get slot=3')
        time.sleep(0.5)

    def feed(self, fd, text):
        self.buffer += text
        while True:
            start = self.buffer.find(APC)
            if start < 0:
                self.screen += self.buffer
                self.buffer = ''
                return
            end = self.buffer.find(ST, start)
            if end < 0:
                return
            self.screen += self.buffer[:start]
            command = self.buffer[start + 2:end]
            self.buffer = self.buffer[end + 2:]
            if command.startswith('TERMinator:TRACE;'):
                self.handle(fd, command[len('TERMinator:TRACE;'):])


def run(plain=False, timeout=120):
    terminal = FakeTerminal(plain)
    primary, secondary = pty.openpty()
    door = subprocess.Popen([DOOR], stdin=secondary, stdout=secondary, stderr=subprocess.STDOUT, cwd=HERE)
    os.close(secondary)

    deadline = time.time() + timeout
    last_output = time.time()
    while time.time() < deadline:
        ready, _, _ = select.select([primary], [], [], 0.2)
        if ready:
            last_output = time.time()
            try:
                data = os.read(primary, 65536)
            except OSError:
                break
            if not data:
                break
            terminal.feed(primary, data.decode('latin-1'))
        if door.poll() is not None:
            # drain whatever is left
            while select.select([primary], [], [], 0.1)[0]:
                try:
                    data = os.read(primary, 65536)
                except OSError:
                    break
                if not data:
                    break
                terminal.feed(primary, data.decode('latin-1'))
            break
        # A terminal without TRACE is offered the ANSI versions; this test only checks it's told about TRACE, so it
        # leaves from the menu rather than start a game
        if plain and 'Q = back' in terminal.screen and not terminal.left_menu:
            os.write(primary, b'q')
            terminal.left_menu = True
            last_output = time.time()
        # The door stops at "press any key" and at its menu (Enter takes the recommended choice, TRACE here); anything
        # counts, so give it Enter when it goes quiet
        if door.poll() is None and time.time() - last_output > 1.0:
            os.write(primary, b'\r')
            last_output = time.time()
    else:
        door.kill()
        raise SystemExit('the door never finished')

    return door.wait(), terminal


def main():
    plain = '--plain' in sys.argv
    code, terminal = run(plain)
    screen = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', terminal.screen)

    print(f'door exit code: {code}')
    print('--- what the player saw ---')
    print('\n'.join(line for line in screen.splitlines() if line.strip()))
    print('---')

    if plain:
        assert 'needs TERMinator' in screen, 'a terminal without TRACE should be told so'
        print('PASS: a terminal without TRACE is told TRACE needs TERMinator, and offered the ANSI versions')
        return

    wasm = sha256_file(os.path.join(HERE, 'doom.wasm'))
    wad = sha256_file(os.path.join(HERE, 'doom1.wad'))
    assert wasm in terminal.stored, 'the game was never uploaded'
    assert wad in terminal.stored, 'the WAD was never uploaded'
    assert terminal.stored[wasm] == open(os.path.join(HERE, 'doom.wasm'), 'rb').read(), 'the game arrived damaged'
    assert terminal.stored[wad] == open(os.path.join(HERE, 'doom1.wad'), 'rb').read(), 'the WAD arrived damaged'
    assert terminal.started == wad, 'the module was not told which WAD to play'
    assert code == 0, 'the door should exit cleanly'
    print(f'PASS: game ({len(terminal.stored[wasm]):,} bytes) and WAD ({len(terminal.stored[wad]):,} bytes) '
          'arrived intact, and the game was started')

    # The savegame the "game" sent up should be on disk, and the copy the door sent back should match it
    saved_path = os.path.join(HERE, 'saves', 'player', 'doomsav3.dsg')
    assert os.path.exists(saved_path), 'the door did not keep the savegame'
    on_disk = open(saved_path, 'rb').read()
    assert on_disk == terminal.saved, 'the savegame on disk does not match what was sent'
    assert bytes(terminal.fetched.get(3, b'')) == terminal.saved, 'the door sent back a different savegame'
    print(f'PASS: savegame ({len(on_disk):,} bytes) kept for the player and sent back intact')

    assert not os.path.exists(os.path.join(HERE, 'saves', 'player', 'doomsav5.dsg')), \
        'a savegame that lost a piece on the way was kept anyway'
    print('PASS: a savegame with a missing piece was refused instead of kept')


if __name__ == '__main__':
    main()
