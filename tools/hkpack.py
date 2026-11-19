"""Minimal Havok packfile (hk_2014, 64-bit) reader: objects, classes, fixups."""
import struct, sys

class Pack:
    def __init__(self, path):
        self.b = b = open(path, 'rb').read()
        m0, m1, utag, fver = struct.unpack_from('<IIii', b, 0)
        assert (m0, m1) == (0x57e0e057, 0x10c0c010), 'not a packfile'
        self.nsec = struct.unpack_from('<i', b, 0x14)[0]
        self.version = b[0x28:0x38].split(b'\0')[0].decode()
        maxpred, predsize = struct.unpack_from('<hh', b, 0x3C)
        off = 0x40 + (predsize if maxpred != -1 else 0)
        self.secs = []
        for i in range(self.nsec):
            tag = b[off:off + 20].split(b'\0')[0].decode()
            start, lf, gf, vf, ex, im, end = struct.unpack_from('<7i', b, off + 20)
            self.secs.append(dict(tag=tag, start=start, lf=lf, gf=gf, vf=vf, ex=ex, im=im, end=end))
            off += 0x40 if fver >= 11 else 0x30
        self.data = next(i for i, s in enumerate(self.secs) if s['tag'] == '__data__')
        self.cls = next(i for i, s in enumerate(self.secs) if s['tag'] == '__classnames__')
        d = self.secs[self.data]; s0 = d['start']
        self.local = {}
        o = s0 + d['lf']
        while o + 8 <= min(s0 + d['gf'], len(b)):
            src, dst = struct.unpack_from('<ii', b, o); o += 8
            if src != -1: self.local[src] = dst
        self.glob = {}
        o = s0 + d['gf']
        while o + 12 <= min(s0 + d['vf'], len(b)):
            src, sec, dst = struct.unpack_from('<iii', b, o); o += 12
            if src != -1: self.glob[src] = (sec, dst)
        self.objs = []  # (offset, classname)
        o = s0 + d['vf']
        cs = self.secs[self.cls]['start']
        while o + 12 <= min(s0 + d['ex'], len(b)):
            src, sec, nameoff = struct.unpack_from('<iii', b, o); o += 12
            if src != -1:
                name = b[cs + nameoff:cs + nameoff + 80].split(b'\0')[0].decode()
                self.objs.append((src, name))
        self.objs.sort()
        self.d0 = s0
        self.dataend = s0 + d['lf']
    def obj_end(self, off):
        nxt = [o for o, _ in self.objs if o > off]
        return min(nxt) if nxt else self.dataend - self.d0
    def raw(self, off, n): return self.b[self.d0 + off:self.d0 + off + n]
    def ptr(self, off):
        if off in self.local: return ('L', self.local[off])
        if off in self.glob: return ('G', self.glob[off][1])
        return None
    def cstr(self, off):
        p = self.ptr(off)
        if not p: return None
        return self.b[self.d0 + p[1]:self.d0 + p[1] + 200].split(b'\0')[0].decode('latin1')
    def classof(self, off):
        return dict(self.objs).get(off)
