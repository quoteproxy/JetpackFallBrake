"""Remove the end-of-clip "jumpLand" trigger from first-person JumpFall clips (Fallout 4 behavior .hkx).

usage: python patch_jumpfall.py <in.hkx> <out.hkx>
Input: Meshes\Actors\Character\_1stPerson\Behaviors\MTBehavior.hkx, GunBehavior.hkx or MeleeBehavior.hkx,
extracted from "Fallout4 - Animations.ba2".
The JumpFall clip plays once; its trigger raises "jumpLand" when it ends (~2.3 s), which lands the
first-person animation in mid-air during long jetpack flights. The trigger array (one trigger, one user)
is emptied by setting its size to 0 (one byte). Real landings still come from the game's own landing events.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hkpack import Pack
if len(sys.argv) != 3:
    sys.exit(__doc__)
p = Pack(sys.argv[1])
b = bytearray(p.b)
def arr(off):
    pt = p.ptr(off); n = struct.unpack_from('<i', p.raw(off + 8, 4))[0]
    return (pt[1] if pt else None), n
sd = next(o for o, c in p.objs if c == 'hkbBehaviorGraphStringData')
ea, en = arr(sd + 0x10); events = [p.cstr(ea + 8 * i) for i in range(en)]
users = {}
for off, c in p.objs:
    if c == 'hkbClipGenerator':
        tr = p.ptr(off + 0x98)
        if tr: users.setdefault(tr[1], []).append(off)
done = 0
for ta, clips in users.items():
    a, n = arr(ta + 0x10)
    trig = [struct.unpack_from('<ifi?', p.raw(a + 0x20 * i, 0x20), 0) for i in range(n)]
    ids = [struct.unpack_from('<i', p.raw(a + 0x20 * i + 8, 4))[0] for i in range(n)]
    rel = [p.raw(a + 0x20 * i + 0x18, 1)[0] for i in range(n)]
    if n == 1 and len(clips) == 1 and events[ids[0]] == 'jumpLand' and rel[0] == 1:
        struct.pack_into('<i', b, p.d0 + ta + 0x18, 0)
        print('emptied trigger array of clip %s (%s)' % (p.cstr(clips[0] + 0x38), p.cstr(clips[0] + 0x90)))
        done += 1
if done != 1:
    sys.exit('expected exactly one JumpFall jumpLand trigger, found %d; nothing written' % done)
open(sys.argv[2], "wb").write(b)
