"""Builds the versus menu's Multiplayer plates for the Vita (port/vita/vita_vsnet*.h): the battle type's three plates
(Single Battle, Team Battle, DP Battle: section 8's textures 2 and 3, plain and highlighted) lettered anew for the
Multiplayer choice's three levels (1.2.6):

    A  Online          | Ad Hoc       | Two Controllers
    B  Ad Hoc: Host    | Ad Hoc: Join | Back
    C  Online: Host    | Online: Join | Back

    python3 gen_vsnet.py <vs_s1.bin> <optart header> <out.h> [es] [bt4]

<vs_s1.bin>: the versus menu's first section unpacked (file 449; BT4: member 448), of the disc the header's art is
for; <optart header>: that disc's vita_optart*.h (its kVsArt runs: the plate "1P VS 2P" already reads Multiplayer, as
the running game has it). The header holds the runs of section 8 any of the three sets changes, the FNV-1a of the bytes
they replace (the disc's: checked before anything is written, and kept in memory by vita_optart.c to put back) and the
three sets' bytes. Not the disc's own bytes: those are read from the running game. Previews: <out.h>-<set>-t<n>.png."""
import re, sys, os
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bt3tex import u32, texture
from labelart import render, row_stats, quantize

vs_path, opt_h, out_h = sys.argv[1], sys.argv[2], sys.argv[3]
ES = 'es' in sys.argv[4:]
BT4 = 'bt4' in sys.argv[4:]
SUF = 'Es' if ES else ''
SETS = {
    'A': ['Online', 'Ad Hoc', 'Two Controllers'],
    'B': ['Ad Hoc: Host', 'Ad Hoc: Join', 'Back'],
    'C': ['Online: Host', 'Online: Join', 'Back'],
}
SETS_ES = {
    'A': ['En Línea', 'Ad Hoc', 'Dos Controles'],
    'B': ['Ad Hoc: Crear', 'Ad Hoc: Unirse', 'Volver'],
    'C': ['En Línea: Crear', 'En Línea: Unirse', 'Volver'],
}
if ES:
    SETS = SETS_ES


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def sec_bytes(u, n):
    a = u32(u, n * 4) & ~3
    b = (u32(u, (n + 1) * 4) & ~3) if n < u32(u, 0) else len(u)
    return bytearray(u[a:b])


def shadow_rgb(rgba, y0, y1):
    a = rgba[y0:y1].astype(float)
    m = (a[..., 3] > 0) & (a[..., 3] <= 60)
    return tuple(a[m][:, :3].mean(0)) if m.any() else (0, 0, 0)


def smooth(rows):
    out = {}
    for k in sorted(rows):
        near = [rows[j] for j in (k - 1, k, k + 1) if j in rows]
        out[k] = np.mean(near, axis=0)
    return out


def width_of(g):
    cols = np.where(g[..., 3].max(0) > 40)[0]
    return cols.max() - cols.min() + 1 if len(cols) else 0


def letter(sec, entry_i, texts, maxw):
    """the three plates of texture entry_i lettered with texts (as gen_patches.py's label_job, style from the first
    plate); a label wider than maxw narrowed (down to three quarters of its width), then made smaller"""
    t = texture(bytes(sec), entry_i)
    rgba, idx, colors, srcmap = t['rgba'], t['idx'].copy(), t['colors'], t['srcmap']
    fill, outl = row_stats(rgba, 0, 32)
    fill, outl = smooth(fill), smooth(outl)
    sh = shadow_rgb(rgba, 0, 32)
    clear = int(np.argmin(colors[:, 3].astype(int) * 1000 + colors[:, :3].astype(int).sum(1)))
    w = t['w']
    for cell, text in zip((0, 32, 64), texts):
        g = render(text, 18, 4, 22, w, 32, fill, outl, sh, tracking=0.5, align='left')
        width = width_of(g)
        left = int(np.where(g[..., 3].max(0) > 40)[0].min())
        if width > maxw:
            done = False
            for cap in (18, 17, 16, 15, 14):
                xs = 1.0
                while width > maxw and xs > 0.74:
                    xs -= 0.02
                    g = render(text, cap, 4, 22 - (18 - cap) / 2, w, 32, fill, outl, sh, tracking=0.5, align='left', xscale=xs)
                    width = width_of(g)
                if width <= maxw:
                    done = True
                    break
            print(f'  {text!r}: narrowed to {width} pixels{"" if done else " (still wider)"}')
            now = int(np.where(g[..., 3].max(0) > 40)[0].min())
            g = np.roll(g, left - now, axis=1)  # (its left edge where the others' is: narrowing keeps the middle)
        q = quantize(g, colors)
        q[g[..., 3] < 1.0] = clear
        idx[cell:cell + 32, :] = q
    new = bytearray(sec)
    for y in range(t['h']):
        for x in range(w):
            off, _shift = srcmap[y, x]
            new[off] = int(idx[y, x])
    return new, colors[idx]


# the section as the running game has it: the disc's, with the header's kVsArt runs (section 8) written
u = open(vs_path, 'rb').read()
live = sec_bytes(u, 8)
h = open(opt_h).read()
m = re.search(r'kVsArtRuns' + SUF + r'\[\]\[3\] = \{[^\n]*\n(.*?)\n\};', h, re.S)
runs3 = [(int(a, 16), int(b), int(c)) for a, b, c in re.findall(r'\{0x([0-9A-F]+), (\d+), (\d+)\}', m.group(1))] if m else []
m = re.search(r'kVsArtBytes' + SUF + r'\[\d+\] = \{\n(.*?)\n\};', h, re.S)
vbytes = bytes(int(x) for x in re.findall(r'\d+', m.group(1))) if m else b''
for off, ln, at in runs3:
    live[off:off + ln] = vbytes[at:at + ln]
print(f'{vs_path}: section 8, {len(live)} bytes; {len(runs3)} kVsArt runs written first')

# the widest a plate's label may be: what "Two Controllers" takes, which the plates show whole (in either language)
t2 = texture(bytes(live), 2)
fill, outl = row_stats(t2['rgba'], 0, 32)
probe = render('Two Controllers', 18, 4, 22, t2['w'], 32, smooth(fill), smooth(outl), (0, 0, 0), tracking=0.5)
maxw = max(width_of(probe), 150)
print(f'  the widest label: {maxw} pixels')

sets = {}
for name, texts in SETS.items():
    new = bytearray(live)
    for e in (2, 3):
        new, pic = letter(new, e, texts, maxw)
        Image.fromarray(pic.astype(np.uint8), 'RGBA').save(f'{out_h}-{name}-t{e}.png')
    sets[name] = new

# the runs any set changes (gaps of up to two bytes joined)
diff = np.zeros(len(live), bool)
L = np.frombuffer(bytes(live), np.uint8)
for new in sets.values():
    diff |= np.frombuffer(bytes(new), np.uint8) != L
runs = []
i, n = 0, len(live)
while i < n:
    if diff[i]:
        j = i
        while j < n and (diff[j] or (j + 1 < n and diff[j + 1]) or (j + 2 < n and diff[j + 2])):
            j += 1
        runs.append((i, j - i))
        i = j
    else:
        i += 1
orig = b''.join(bytes(live[o:o + k]) for o, k in runs)
total = len(orig)
print(f'  runs {len(runs)}, {total} bytes a set')

what = ('BT4 B14 Rev2' + (" (its Spanish release)" if ES else '')) if BT4 else ("the European disc's Spanish menus" if ES else "the USA disc")
lines = [f'/* Generated by tools/gen_vsnet.py from {what}: the versus menu\'s Multiplayer plates (section 8, textures 2 and 3)',
         '   for the choice\'s three levels: A Online / Ad Hoc / Two Controllers, B Ad Hoc: Host / Ad Hoc: Join / Back, C Online:',
         '   Host / Online: Join / Back. The runs any of them changes, the FNV-1a of the bytes they replace (the disc\'s, read from',
         '   the running game and kept to put back: vita_optart.c), and each set\'s bytes. */',
         f'static const uint32_t kVsNetRuns{SUF}[][2] = {{ /* offset in section 8, length */']
for o, k in runs:
    lines.append(f'    {{0x{o:X}, {k}}},')
lines.append('};')
lines.append(f'#define VSNET_HASH{SUF} 0x{fnv(orig):08X}u')
lines.append(f'#define VSNET_BYTES{SUF} {total}')
for name, new in sets.items():
    blob = b''.join(bytes(new[o:o + k]) for o, k in runs)
    assert len(blob) == total
    lines.append(f'static const uint8_t kVsNet{name}{SUF}[{total}] = {{')
    for i in range(0, total, 32):
        lines.append('    ' + ','.join(str(b) for b in blob[i:i + 32]) + ',')
    lines.append('};')
open(out_h, 'w').write('\n'.join(lines) + '\n')
print(f'  wrote {out_h}')
