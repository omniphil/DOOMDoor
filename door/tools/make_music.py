#!/usr/bin/env python3
"""
make_music.py -- turns musrender's WAV files into the pieces the JPEG XL graphics mode plays (door/music/).

Each track becomes 5-second Ogg Vorbis files, mono at 22050 Hz (about 4.4 KB a second). The door uploads a piece to
the caller's cache only when it's about to be played, so a track starts within a moment and a slow connection is
never tied up sending a whole one; the caller's terminal plays the pieces back to back on one channel, seamlessly
(Vorbis keeps each piece's exact length).

    ./musrender /tmp/tracks && python3 tools/make_music.py /tmp/tracks music

Writes music/<TRACK>_<nn>.ogg and music/index.txt, one line per track:
    <TRACK> <sha256 of the MUS lump> <pieces> <length of the last piece in ms>
Needs ffmpeg with libvorbis. Run on the development machine; the results are part of the door.
"""

import os
import subprocess
import sys

RATE = 22050
PIECE = 5 * RATE          # samples a piece


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(dst):
        if name.endswith('.ogg') or name == 'index.txt':
            os.remove(os.path.join(dst, name))

    tracks = [line.split() for line in open(os.path.join(src, 'tracks.txt')) if line.strip()]
    index = []
    for track, lump_hash in tracks:
        # the whole track, mono at 22050 Hz, as raw samples: cut from this so the pieces join exactly
        pcm = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', os.path.join(src, track + '.wav'),
                              '-ac', '1', '-ar', str(RATE), '-f', 's16le', '-'],
                             check=True, capture_output=True).stdout
        samples = len(pcm) // 2
        pieces = (samples + PIECE - 1) // PIECE
        for k in range(pieces):
            chunk = pcm[k * PIECE * 2:(k + 1) * PIECE * 2]
            subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 's16le', '-ar', str(RATE), '-ac', '1',
                            '-i', '-', '-c:a', 'libvorbis', '-q:a', '0',
                            os.path.join(dst, f'{track}_{k:02d}.ogg')], input=chunk, check=True)
        last_ms = (samples - (pieces - 1) * PIECE) * 1000 // RATE
        index.append(f'{track} {lump_hash} {pieces} {last_ms}\n')
        print(f'{track}: {pieces} pieces')
    with open(os.path.join(dst, 'index.txt'), 'w') as f:
        f.writelines(index)


if __name__ == '__main__':
    main()
