#!/usr/bin/env python3
"""Cook the course objects' BRASD animation sound events (include/mkw/object_sound_assets.hpp).

Retail loads brasd/<resource>/<clip>.brasd beside each CHR0 clip (fn_1_455C8, PAL 0x8055597C)
and NW4R's AnimSoundImpl plays its point events as the clip's frame passes them (see
src/race_audio_frame.cpp). Each clip keeps its BRASD frame count (AnimSoundImpl wraps there and
RaceAnimActor::UpdateFrame, 0x80700754, skips frames at or past it), its CHR0 frame count and
loop flag (the clip the object plays), and each event its frame, sound, volume (the byte / 128,
applied by StartEvent 0x80087BE0 only below 128) and pitch (applied when not 1.0).
"""
from pathlib import Path
import hashlib
import json
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
# (BRRES resource, course folder, clip). Every course carrying the same file must carry the same
# bytes (checked below), so one folder stands for all.
SOURCES = [*[(cannon, course, cannon) for cannon, course in (('DonkyCannon_wii', 'boardcross_course'),
                                                            ('DonkyCannonGC', 'old_donkey_gc'),
                                                            ('tree_cannon', 'treehouse_course'))],
           *[('koopaFigure', 'koopa_course', clip) for clip in ('wait', 'vomit')],
           *[('crab', 'old_heyho_gba', clip) for clip in ('walk_l', 'walk_r', 'wait')],
           ('puchi_pakkun', 'old_mario_64', 'puchi_pakkun'),
           *[('hanachan', 'treehouse_course', clip) for clip in ('walk', 'head_wait2')],
           ('sanbo', 'desert_course', 'revive'),
           ('kuribo', 'old_mario_gc', 'walk_l'), ('kuribo', 'old_mario_gc', 'walk_r'),
           *[('cow', 'farm_course', clip) for clip in ('walk', 'run', 'eat', 'eat_ed', 'surprise')],
           *[('penguin_s', 'old_sherbet_64', clip) for clip in ('walk', 'dive', 'stand_up')],
           ('penguin_m', 'old_sherbet_64', 'walk'),
           *[('pakkun_f', 'old_mario_gc', clip) for clip in ('wait', 'attack')],
           *[('poihana', 'old_peach_gc', clip) for clip in ('walk', 'throw')]]


def chr0(path, clip):
    """(frame count, loop) of the CHR0 `clip` in a BRRES (version 5: u16 +0x20, u32 +0x24)."""
    d = path.read_bytes()
    be16 = lambda o: struct.unpack_from('>H', d, o)[0]
    be32 = lambda o: struct.unpack_from('>I', d, o)[0]

    def group(g):
        out = {}
        for i in range(1, be32(g + 4) + 1):
            e = g + 8 + i * 16
            out[d[g + be32(e + 8):].split(b'\0', 1)[0].decode()] = g + be32(e + 12)
        return out
    root = be16(0xc)
    if d[:4] != b'bres' or d[root:root + 4] != b'root':
        raise ValueError('Not a BRRES: ' + str(path))
    at = group(group(root + 8)['AnmChr(NW4R)'])[clip]
    if d[at:at + 4] != b'CHR0' or be32(at + 8) != 5:
        raise ValueError(f'Unsupported CHR0 {clip} in {path}')
    return be16(at + 0x20), be32(at + 0x24) != 0


def main():
    ids = dict((name, int(value)) for name, value in re.findall(r'^\s*(\w+)=(\d+),', (ROOT/'include/mkw/sound_ids.hpp').read_text(), re.M))
    events, clips, receipt = [], [], []
    for resource, course, clip in SOURCES:
        path = ROOT/f'assets/courses/{course}/brasd/{resource}/{clip}.brasd'
        data = path.read_bytes()
        for other in sorted((ROOT/'assets/courses').glob(f'*/brasd/{resource}/{clip}.brasd')):
            if other.read_bytes() != data:
                raise ValueError(f'{other} differs from {path}')
        if data[:8] != b'RASD\xfe\xff\x01\x00':
            raise ValueError('Unsupported RASD: ' + str(path))
        block = struct.unpack_from('>I', data, 16)[0]
        if data[block:block+4] != b'DATA':
            raise ValueError('Missing RASD DATA')
        base = block+8
        frames, _, table_offset = struct.unpack_from('>III', data, base)
        table = base+table_offset
        count = struct.unpack_from('>I', data, table)[0]
        first = len(events)
        for i in range(count):
            at = table+4+i*20
            frame, end = struct.unpack_from('>2i', data, at)
            # Trigger event (frame flags 3), loop offset 0, loop interval 1: it plays on every
            # pass of its frame (UpdateForward 0x80086EA0, trigger 0x80087340).
            if end != -1 or data[at+8:at+16] != b'\x03\x00\x01\x00\x01\x00\x00\x00':
                raise ValueError('Only forward trigger events every loop are cooked')
            info = base+struct.unpack_from('>I', data, at+16)[0]
            text = info+struct.unpack_from('>I', data, info+12)[0]
            name = data[text:].split(b'\0', 1)[0].decode('ascii')
            volume = data[info+16]
            pitch = struct.unpack_from('>f', data, info+20)[0]
            if data[info:info+8] != b'\x00\x00\x00\x00\xff\xff\xff\xff' or data[info+17:info+20] != b'\x00\x00\x00' or not 0 < pitch < 16:
                raise ValueError('Unsupported event options/sound id/pitch')
            events.append((frame, ids[name], min(volume, 128)/128, pitch))
        chr_frames, chr_loop = chr0(ROOT/f'assets/courses/{course}/{resource}.brres', clip)
        clips.append((resource, clip, frames, first, count, chr_frames, chr_loop))
        receipt.append({'path': path.relative_to(ROOT).as_posix(), 'sha256': hashlib.sha256(data).hexdigest(), 'events': count,
                        'chr_frames': chr_frames, 'chr_loop': chr_loop})
    lines = ['#pragma once', '// Generated by tools/cook_object_sounds.py: BRASD animation sound events.', 'namespace mkw {',
             'struct ObjectSoundEvent {float frame;unsigned short sound;float volume,pitch;};',
             'inline constexpr ObjectSoundEvent ObjectSoundEvents[]={']
    lines += ['{%s.f,%s,%.8ff,%.8ff},' % (frame, sound, volume, pitch) for frame, sound, volume, pitch in events]
    lines += ['};', '// frames: the BRASD frame count; chrFrames / chrLoop: the CHR0 clip the object plays.',
              'struct ObjectSoundClip {const char* resource;const char* name;unsigned frames,first,count,chrFrames;bool chrLoop;};',
              'inline constexpr ObjectSoundClip ObjectSoundClips[]={']
    lines += ['{%s,%s,%s,%s,%s,%s,%s},' % (json.dumps(r), json.dumps(c), n, first, count, cf, 'true' if cl else 'false')
              for r, c, n, first, count, cf, cl in clips]
    lines += ['};', '}']
    (ROOT/'include/mkw/object_sound_assets.hpp').write_text('\n'.join(lines)+'\n')
    (ROOT/'build/object-sounds.json').write_text(json.dumps({'sources': receipt, 'clips': len(clips), 'events': len(events)}, indent=2)+'\n')
    print(f'Cooked {len(clips)} object animation sound clips / {len(events)} point events; no new audio files')


if __name__ == '__main__':
    main()
