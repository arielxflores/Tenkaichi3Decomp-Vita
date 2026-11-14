"""Builds the art changes for the Vita (port/vita/vita_optart.h) from the USA disc's files:

    python3 gen_patches.py <folder> <out.h> [bt3|bt4] [es]

<folder> holds tools/ (this folder) and what the changes apply to: opt_s1.bin (file 454, section 1 unpacked: the
options screen), vs_s1.bin (file 449, section 1 unpacked: the versus menu) and boot.bin (file 5, the boot file:
its member 8 holds the button icons of the game's text). With bt4: the same files of the BT4 B14 Rev2 disc (the
options screen is its pzs4us1.afs member 453, the versus menu member 448's first section), whose lettering is the
mod's own: vita_optart_bt4.h, for the BT4 program; its sound page keeps Change Voice. With es: the same of a Spanish disc
(BT4: Team BT4's Spanish release), the new lettering in Spanish and the tables' names ending in Es (kOptArtRunsEs ...):
vita_optart_bt4es.h, which vita_optart.c takes for a disc whose art is that one."""
import sys, struct, numpy as np
sys.path.insert(0, sys.argv[1] + '/tools')
from bt3tex import *
from labelart import *
from PIL import Image
import vita_pic
import vita_icons

base = sys.argv[1]
BT4 = len(sys.argv) > 3 and sys.argv[3] == 'bt4'
ES = len(sys.argv) > 4 and sys.argv[4] == 'es'
# the new lettering in Spanish (es): the words the Vita adds; the rest as in English
TR_ES = {'Vita Settings': 'Ajustes Vita', 'Controller Settings': 'Ajustes del control', 'Language': 'Idioma',
         'Rear': 'Trasero', 'Front': 'Frontal', 'Touch': 'T\u00e1ctil', 'Clashes': 'Choques', '2nd Pad': 'Control 2',
         'Square': 'Cuadrado', 'Stick': 'Palanca', 'Unlock All': 'Desbloquear', 'Quick Start': 'Inicio R\u00e1pido',
         'Quick Save': 'Guardado R\u00e1pido', 'Tech Fixes': 'Arreglos', 'Z Counter': 'Contador Z',
         'Bombardment': 'Bombardeo', 'Game': 'Juego', 'Screen': 'Pantalla', 'Multiplayer': 'Multijugador',
         'Two Controllers': 'Dos Controles', 'Ad Hoc: Host': 'Ad Hoc: Crear', 'Ad Hoc: Join': 'Ad Hoc: Unirse'}
def T(text):
    return TR_ES.get(text, text) if ES else text
u = open(base + '/opt_s1.bin', 'rb').read()
packs = {'opt': u, 'vs': open(base + '/vs_s1.bin', 'rb').read(), 'boot': open(base + '/boot.bin', 'rb').read()}

def sec_bytes(n, pack='opt'):
    u = packs[pack]
    a = u32(u, n * 4) & ~3
    b = (u32(u, (n + 1) * 4) & ~3) if n < u32(u, 0) else len(u)
    return a, bytearray(u[a:b])

def shadow_rgb(rgba, y0, y1):
    a = rgba[y0:y1].astype(float)
    m = (a[..., 3] > 0) & (a[..., 3] <= 60)
    return tuple(a[m][:, :3].mean(0)) if m.any() else (0, 0, 0)

def smooth(rows):
    ks = sorted(rows)
    out = {}
    for k in ks:
        near = [rows[j] for j in (k - 1, k, k + 1) if j in rows]
        out[k] = np.mean(near, axis=0)
    return out

patches = []   # (section, original bytes, new bytes) of the screen being made
previews = []

def label_job(section, entry_i, jobs, style_cell, pack='opt', base_sec=None, keep=True, maxw=None):
    """jobs: (cell_y, text, x, align); style from the label in style_cell (y of its 32-row cell). base_sec: draw on
    these bytes of the section instead of the disc's; keep=False: only return the new bytes. maxw: no label wider
    than that many pixels (a longer one is narrowed: the plates' captions, which the plates show only so wide)"""
    a, sec = sec_bytes(section, pack)
    if base_sec is not None:
        sec = bytearray(base_sec)
    t = texture(bytes(sec), entry_i)
    rgba, idx, colors, srcmap = t['rgba'], t['idx'].copy(), t['colors'], t['srcmap']
    fill, outl = row_stats(rgba, style_cell, style_cell + 32)
    fill = smooth({k - style_cell: v for k, v in fill.items()})
    outl = smooth({k - style_cell: v for k, v in outl.items()})
    sh = shadow_rgb(rgba, style_cell, style_cell + 32)
    clear = int(np.argmin(colors[:, 3].astype(int) * 1000 + colors[:, :3].astype(int).sum(1)))
    w = t['w']
    for cell, text, x, align in jobs:
        text = T(text)
        g = render(text, 18, x, 22, w, 32, fill, outl, sh, tracking=0.5, align=align)
        if maxw is not None:
            cols = np.where(g[..., 3].max(0) > 40)[0]
            width = cols.max() - cols.min() + 1 if len(cols) else 0
            if width > maxw:
                # narrowed first, down to three quarters of its width; a longer one also a little smaller (its
                # middle kept on the row's middle), a size at a time
                for cap in (18, 17, 16, 15, 14, 13):
                    xs = 1.0
                    while width > maxw and xs > 0.74:  # (the outline's few pixels do not narrow: a step at a time)
                        xs -= 0.02
                        g = render(text, cap, x, 22 - (18 - cap) / 2, w, 32, fill, outl, sh, tracking=0.5, align=align, xscale=xs)
                        cols = np.where(g[..., 3].max(0) > 40)[0]
                        width = cols.max() - cols.min() + 1
                    if width <= maxw:
                        break
                    xs = 1.0
                    g = render(text, cap - 1, x, 22 - (18 - cap + 1) / 2, w, 32, fill, outl, sh, tracking=0.5, align=align)
                    cols = np.where(g[..., 3].max(0) > 40)[0]
                    width = cols.max() - cols.min() + 1
                print(f'  {text!r}: {cap} high, narrowed to {xs:.2f} of its width ({width} pixels)')
        q = quantize(g, colors)
        q[g[..., 3] < 1.0] = clear
        idx[cell:cell + 32, :] = q
    new = bytearray(sec)
    for y in range(t['h']):
        for x in range(w):
            off, shift = srcmap[y, x]
            new[off] = int(idx[y, x])  # (8 bits: shift 0)
    if not keep:
        return new
    previews.append(colors[idx])
    patches.append((section, sec, new))

def picture_job(section, entry_i, img):
    a, sec = sec_bytes(section)
    t = texture(bytes(sec), entry_i)
    srcmap = t['srcmap']
    # 256 colours with alpha, PS2 alpha scale (0..128)
    q = img.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette(rawmode='RGBA')[:256 * 4], dtype=np.uint8).reshape(-1, 4) if q.getpalette(rawmode='RGBA') else None
    idx = np.array(q)
    pal = pal.astype(int)
    pal[:, 3] = (pal[:, 3] * 128 + 127) // 255
    new = bytearray(sec)
    for y in range(t['h']):
        for x in range(t['w']):
            off, shift = srcmap[y, x]
            new[off] = int(idx[y, x])
    pbase = t['palette']
    for i in range(256):
        j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)
        new[pbase + j * 4:pbase + j * 4 + 4] = bytes(int(v) for v in pal[i])
    previews.append(pal.astype(np.uint8)[idx])
    patches.append((section, sec, new))

def icon_job(entry_i, idx, pal, section=8, pack='boot'):
    """a 16-colour texture of a texture file redrawn: its pixels (4 bits each) and its palette"""
    a, sec = sec_bytes(section, pack)
    t = texture(bytes(sec), entry_i)
    assert t['psm'] == 20 and idx.shape == (t['h'], t['w'])
    new = bytearray(sec)
    for y in range(t['h']):
        for x in range(t['w']):
            off, shift = t['srcmap'][y, x]
            new[off] = (new[off] & ~(15 << shift) & 0xFF) | (int(idx[y, x]) << shift)
    for i in range(16):
        new[t['palette'] + i * 4:t['palette'] + i * 4 + 4] = bytes(int(v) for v in pal[i])
    previews.append(np.array(pal, dtype=np.uint8)[idx])
    patches.append((section, sec, new))

def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h

def runs_of(patches):
    """per section, the runs of changed bytes"""
    out = []
    for section in sorted(set(p[0] for p in patches)):
        ps = [p for p in patches if p[0] == section]
        orig = ps[0][1]
        new = bytearray(orig)
        for _, _, n in ps:
            for i in range(len(n)):
                if n[i] != orig[i]:
                    new[i] = n[i]
        runs = []
        i = 0
        while i < len(orig):
            if new[i] != orig[i]:
                j = i
                while j < len(orig) and (new[j] != orig[j] or (j + 1 < len(orig) and new[j + 1] != orig[j + 1]) or (j + 2 < len(orig) and new[j + 2] != orig[j + 2])):
                    j += 1
                runs.append((i, bytes(orig[i:j]), bytes(new[i:j])))
                i = j
            else:
                i += 1
        out.append((section, runs))
    return out

def emit(lines, out, macro, name):
    """the C tables of one screen: kXxxRuns, kXxxBytes, kXxxSections (nothing to change: one section of no runs)"""
    if not out:
        lines.append(f'static const uint32_t k{name}Runs[][3] = {{{{0, 0, 0}}}}; /* (nothing changed) */')
        lines.append(f'static const uint8_t k{name}Bytes[1] = {{0}};')
        lines.append(f'static const struct {{ int section, first, count; uint32_t hash; }} k{name}Sections[] = {{{{0, 0, 0, 0x811C9DC5u}}}};')
        print(name, 'nothing changed')
        return
    blob = bytearray()
    tab = []
    for section, runs in out:
        origcat = b''.join(r[1] for r in runs)
        first = len(tab)
        for off, o, n in runs:
            tab.append((off, len(n), len(blob)))
            blob += n
        lines.append(f'/* section {section}: {len(runs)} runs, {len(origcat)} bytes */')
        lines.append(f'#define {macro}{section} {section}, {first}, {len(runs)}, 0x{fnv(origcat):08X}u')
    lines.append(f'static const uint32_t k{name}Runs[][3] = {{ /* offset in the section, length, offset in k{name}Bytes */')
    for off, ln, at in tab:
        lines.append(f'    {{0x{off:X}, {ln}, {at}}},')
    lines.append('};')
    lines.append(f'static const uint8_t k{name}Bytes[{len(blob)}] = {{')
    for i in range(0, len(blob), 32):
        lines.append('    ' + ','.join(str(b) for b in blob[i:i + 32]) + ',')
    lines.append('};')
    lines.append(f'static const struct {{ int section, first, count; uint32_t hash; }} k{name}Sections[] = {{' + ', '.join('{' + f'{macro}{s}' + '}' for s, _ in out) + '};')
    print(name, 'runs', len(tab), 'bytes', len(blob), [(s, len(r)) for s, r in out])

# the options screen. The controller page's second row: Vibration -> Vita Settings; the sound page's third row:
# Change Voice -> Language (the text's language and the voices: Option_VitaLang) (normal and highlighted strips)
for e in (1, 3):  # (BT3's Spanish: Vita Settings made of the disc's own letters, below; Language rendered: no I there.
                  #  BT4's Spanish has no A or j: its Ajustes rendered, in its rows' capitals)
    label_job(16, e, ([] if ES and not BT4 else [(128, 'Vita Settings', 4, 'left')]) + ([] if BT4 else [(64, 'Language', 4, 'left')]) +
              ([(96, 'Ajustes del Control', 4, 'left')] if ES and BT4 else []), 128)
# the top page's second row: Screen -> Vita Settings (the Screen page is opened from Vita Settings' third page)
for e in (0, 2) if not (ES and not BT4) else ():
    label_job(16, e, [(32, 'Vita Settings', 4, 'left')], 32)
# the pickers' second caption strip (1P / 2P / Normal / Custom / Simple, three rows free): the captions of "Touch
# Buttons" in the rows the Vita does not show and the free ones; Normal and Custom stay
for e in (1, 3):
    label_job(14, e, [(0, 'L2', 128.5, 'center'), (32, 'R2', 128.5, 'center'), (128, 'L3', 128.5, 'center'),
                      (160, 'Rear', 128.5, 'center'), (192, 'Front', 128.5, 'center'), (224, 'R3', 128.5, 'center')], 64)
def art_job(section, entry_i, y0, img):
    """rows y0.. of a texture redrawn from an image (h x w x 4, PS2 alpha), in the texture's own colours"""
    a, sec = sec_bytes(section)
    t = texture(bytes(sec), entry_i)
    idx, colors, srcmap = t['idx'].copy(), t['colors'], t['srcmap']
    clear = int(np.argmin(colors[:, 3].astype(int) * 1000 + colors[:, :3].astype(int).sum(1)))
    h = img.shape[0]
    q = quantize(img, colors)
    q[img[..., 3] < 1.0] = clear
    idx[y0:y0 + h, :] = q
    new = bytearray(sec)
    for y in range(t['h']):
        for x in range(t['w']):
            off, shift = srcmap[y, x]
            new[off] = int(idx[y, x])
    previews.append(colors[idx])
    patches.append((section, sec, new))

if ES:
    # Spanish ("control", "controles" for "mando", "mandos"): the top page's fourth row, Mando -> Control; its second,
    # Pantalla -> Ajustes Vita; the controller page's first row, Tipo de control -> Ajustes del control, and its second,
    # Vibraci\u00f3n -> Ajustes Vita: made of the disc's own letters (glyphart.py) from Guardar/Cargar (C), Pantalla, the
    # screen row's Ajuste (A j u s t), Tipo de control (d e c o n t r o l), Vibraci\u00f3n (V i); BT4's Spanish rows have no
    # A or j: there only Control so, the two Ajustes rendered (above). The title,
    # Mando -> Control: the English disc's title Controller to its l (its letters as the Spanish one's), from
    # opt_s1_en.bin beside the Spanish one.
    import glyphart as G
    tex16 = bytes(sec_bytes(16)[1])
    pt, ps = texture(tex16, 0)['rgba'], texture(tex16, 1)['rgba']
    for top, sub in ((0, 1), (2, 3)):
        rt, rs = texture(tex16, top)['rgba'], texture(tex16, sub)['rgba']
        cg = lambda r, shape, cell: G.pieces(r, cell, cell + 32, shape=shape)
        save, pant, mando = cg(rt, pt, 0), cg(rt, pt, 32), cg(rt, pt, 96)
        tipo, vib = cg(rs, ps, 96), cg(rs, ps, 128)
        assert (len(save), len(pant), len(mando), len(tipo), len(vib)) == (14, 8, 5, 13, 9), \
            (len(save), len(pant), len(mando), len(tipo), len(vib))
        C_, ontrol = save[8], tipo[7:13]
        gap = int(np.median(G.gaps([(x0, x1, mk) for x0, x1, mk, _c, _w in tipo])))
        tab_t = G.style_table(rt, range(0, 256, 32), 32, shape=pt)
        tab_s = G.style_table(rs, range(0, 160, 32), 32, shape=ps)
        art_job(16, top, 96, G.compose_copy([C_] + ontrol, gap, 8, mando[0][0], 32, rt.shape[1], tab_t))
        if BT4:  # (no A or j in its rows: its Ajustes rendered, above)
            continue
        adj = cg(rt, pt, 192)
        assert len(adj) == 14, len(adj)
        d_, e_, c_, l_ = tipo[4], tipo[5], tipo[6], tipo[12]
        A_, j_, u_, s_, t_ = adj[0:5]
        V_, i_, a_ = vib[0], vib[1], pant[1]
        ajustes = [A_, j_, u_, s_, t_, e_, s_]
        art_job(16, top, 32, G.compose_copy(ajustes + [None, V_, i_, t_, a_], gap, 8, pant[0][0], 32, rt.shape[1], tab_t))
        art_job(16, sub, 96, G.compose_copy(ajustes + [None, d_, e_, l_, None, c_] + ontrol, gap, 8, tipo[0][0], 32,
                                            rs.shape[1], tab_s))
        art_job(16, sub, 128, G.compose_copy(ajustes + [None, V_, i_, t_, a_], gap, 8, vib[0][0], 32, rs.shape[1], tab_s))
    if not BT4:  # the top page's third row, Sonido -> Idioma/Sonido (its page opens on Idioma): rendered in the row's
                 # style, as the sound page's Idioma (the disc has no I)
        for e in (0, 2):
            label_job(16, e, [(64, 'Idioma/Sonido', 4, 'left')], 64)
    G.TITLE = True
    import os
    en = open(os.path.join(sys.argv[1], 'opt_s1_en.bin'), 'rb').read()
    def en_sec(n):
        a = u32(en, n * 4) & ~3
        b = (u32(en, (n + 1) * 4) & ~3) if n < u32(en, 0) else len(en)
        return en[a:b]
    tc = texture(en_sec(18), 5)['rgba'].astype(float)  # (the English disc's Controller)
    gc = G.glyphs(tc, 0, 64)
    assert len(gc) == 10, len(gc)
    m = np.zeros(tc.shape[:2], bool)
    for _, _, mk in gc[:7]:
        m |= mk
    title = G.paint(m, G.style_table(tc, [0], 64))
    cut = (gc[6][0] + gc[6][1]) // 2  # (Control as it is, to the middle of its l; the l's right and its glow drawn again)
    title[:, :cut] = tc[:, :cut]
    art_job(18, 5, 0, title)
    G.TITLE = False

if not ES:
    # Controls: the top page's fourth row, Controller -> Controls; the controller page's first row, Control Type ->
    # Controller Settings; the controller page's title, Controller -> Controls. Made of the disc's own letters
    # (glyphart.py): the rows' from Controller, Screen, BGM Test (BT4: Screen Adjust), Vibration and Change Voice,
    # the title's "Control" as it is and the s of the title Options
    import glyphart as G
    tex16 = bytes(sec_bytes(16)[1])
    pt, ps = texture(tex16, 0)['rgba'], texture(tex16, 1)['rgba']  # (the letters' shapes: the plain strips')
    for top, sub in ((0, 1), (2, 3)):  # (plain, highlighted: the letters' pixels each from its own strip)
        rt, rs = texture(tex16, top)['rgba'], texture(tex16, sub)['rgba']
        cg = lambda r, shape, cell: G.pieces(r, cell, cell + 32, shape=shape)
        ctrl, screen = cg(rt, pt, 96), cg(rt, pt, 32)
        change, vib = cg(rs, ps, 64), cg(rs, ps, 128)
        if BT4:  # (its BGM Select has no s: Screen Adjust's s and t)
            adj = cg(rt, pt, 192)
            assert len(adj) == 12
            s_, t_ = adj[10], adj[11]
        else:
            test = cg(rs, ps, 32)
            assert len(test) == 7
            s_, t_ = test[5], test[6]
        assert (len(ctrl), len(screen), len(change), len(vib)) == (10, 6, 11, 9)
        tab_t = G.style_table(rt, range(0, 256, 32), 32, shape=pt)
        tab_s = G.style_table(rs, range(0, 160, 32), 32, shape=ps)
        art_job(16, top, 96, G.compose_copy(ctrl[:7] + [s_], 3, 8, ctrl[0][0], 32, rt.shape[1], tab_t))
        if not BT4:  # the top page's third row, Sound -> Language/Sound (its page opens on Language): L, a, e and the
                     # slash of Save/Load, n and u of Sound, g of Change Voice, then Sound as it is
            save, sound = cg(rt, pt, 0), cg(rt, pt, 64)
            assert (len(save), len(sound)) == (9, 5)
            L_, a_, e_, sl_, g_ = save[5], save[1], save[3], save[4], change[4]
            art_job(16, top, 64, G.compose_copy([L_, a_, sound[3], g_, sound[2], a_, g_, e_, sl_] + sound, 3, 8,
                                                sound[0][0], 32, rt.shape[1], tab_t))
        art_job(16, sub, 96, G.compose_copy(ctrl + [None, screen[0], screen[3], t_, t_, vib[1], screen[5], change[4], s_],
                                            3, 8, ctrl[0][0], 32, rs.shape[1], tab_s))
    G.TITLE = True
    tc = texture(bytes(sec_bytes(18)[1]), 5)['rgba'].astype(float)
    to = texture(bytes(sec_bytes(18)[1]), 1)['rgba'].astype(float)
    gc, go = G.glyphs(tc, 0, 64), G.glyphs(to, 0, 64)
    assert len(gc) == 10 and len(go) == 7
    m = np.zeros(tc.shape[:2], bool)
    for _, _, mk in gc[:7]:
        m |= mk
    sx0, sx1, smk = go[-1]
    at = gc[6][1] + 5  # (the s four columns after the l, as the l's before it)
    m[:, at:at + sx1 - sx0 + 1] |= smk[:, sx0:sx1 + 1]
    title = G.paint(m, G.style_table(tc, [0], 64))
    cut = (gc[6][0] + gc[6][1]) // 2  # (Control as it is, to the middle of the l; the l's right and the s's left
    title[:, :cut] = tc[:, :cut]      # drawn again; the rest of the s as Options has it)
    for x in range(at + 2, tc.shape[1]):
        title[:, x] = to[:, x - (at - sx0)] if x - (at - sx0) < tc.shape[1] else 0
    art_job(18, 5, 0, title)
    G.TITLE = False

# the controller picture
picture_job(21, 0, vita_pic.make())
out = runs_of(patches)
opt_previews = previews

# Vita Settings needs fourteen captions of that strip, which has eight rows: the rows of Touch Buttons (L2 R2 L3 Rear
# Front R3) and those of the page above them (Touch, Clashes, 2nd Pad, Mods, Square, Stick) take turns, written into
# the unpacked screen as the page changes (vita_optart.c Vita_OptionCaptions). Normal and Custom stay. A third set
# holds the European languages of the Language row (English and Japanese are the first strip's own).
_, sec14 = sec_bytes(14)
CAP_W = 115  # (the widest caption of the disc's own in that strip: the plates show about so much)
cap_t = bytearray(sec14)
for section, runs in out:
    if section == 14:
        for off, _o, n in runs:
            cap_t[off:off + len(n)] = n
cap_v = bytearray(cap_t)
for e in (1, 3):
    cap_v = label_job(14, e, [(0, 'Touch', 128.5, 'center'), (32, 'Clashes', 128.5, 'center'), (128, '2nd Pad', 128.5, 'center'),
                              (160, 'Mods', 128.5, 'center'), (192, 'Square', 128.5, 'center'), (224, 'Stick', 128.5, 'center')],
                      64, base_sec=cap_v, keep=False, maxw=CAP_W)
cap_l = bytearray(cap_t)
for e in (1, 3):  # (BT4: its Language row's two, the English and the Spanish release)
    cap_l = label_job(14, e, [(0, 'English', 128.5, 'center'), (32, 'Espa\u00f1ol', 128.5, 'center')] if BT4 else
                      [(0, 'Espa\u00f1ol', 128.5, 'center'), (32, 'Deutsch', 128.5, 'center'),
                       (128, 'Fran\u00e7ais', 128.5, 'center'), (160, 'Italiano', 128.5, 'center')],
                      64, base_sec=cap_l, keep=False, maxw=CAP_W)
# a fourth: Vita Settings' second page (Unlock All, Quick Start, Quick Save, Tech Fixes); a fifth: the Tech Fixes' two (Z Counter, Bombardment). Normal and Custom stay in every set (the control
# type's picker, whichever set was put in last).
cap_x = bytearray(cap_t)
for e in (1, 3):
    cap_x = label_job(14, e, [(0, 'Unlock All', 128.5, 'center'), (32, 'Quick Start', 128.5, 'center'),
                              (128, 'Quick Save', 128.5, 'center'), (160, 'Tech Fixes', 128.5, 'center')],
                      64, base_sec=cap_x, keep=False, maxw=CAP_W)
cap_y = bytearray(cap_t)
for e in (1, 3):
    cap_y = label_job(14, e, [(0, 'Z Counter', 128.5, 'center'), (32, 'Bombardment', 128.5, 'center')],
                      64, base_sec=cap_y, keep=False, maxw=CAP_W)
# a sixth: Vita Settings' third page (Game, Screen: the console's Screen page) and Game's two plates (BT3, BT4: the
# package's two games); a seventh: BT4's second page (Game, Unlock All, Screen) and the same two plates
cap_g = bytearray(cap_t)
for e in (1, 3):
    cap_g = label_job(14, e, [(0, 'Game', 128.5, 'center'), (32, 'Screen', 128.5, 'center'),
                              (192, 'BT3', 128.5, 'center'), (224, 'BT4', 128.5, 'center')],
                      64, base_sec=cap_g, keep=False, maxw=CAP_W)
cap_h = bytearray(cap_t)
for e in (1, 3):
    cap_h = label_job(14, e, [(0, 'Game', 128.5, 'center'), (32, 'Unlock All', 128.5, 'center'), (128, 'Screen', 128.5, 'center'),
                              (160, 'Language', 128.5, 'center'), (192, 'BT3', 128.5, 'center'), (224, 'BT4', 128.5, 'center')],
                      64, base_sec=cap_h, keep=False, maxw=CAP_W)
def differs(k):
    return k < len(cap_t) and any(cap_t[k] != c[k] for c in (cap_v, cap_l, cap_x, cap_y, cap_g, cap_h))
cap_runs = []
i = 0
while i < len(cap_t):
    if differs(i):
        j = i
        while j < len(cap_t) and (differs(j) or differs(j + 1) or differs(j + 2)):
            j += 1
        cap_runs.append((i, j - i))
        i = j
    else:
        i += 1

# the versus menu: the second plate, 1P VS 2P -> Multiplayer (the plates' strip, plain and highlighted)
patches, previews = [], []
for e in (0, 1) if not (ES and BT4) else ():  # (BT4's Spanish disc's own reads Multijugador)
    label_job(8, e, [(32, 'Multiplayer', 4, 'left')], 32, pack='vs')
out_vs = runs_of(patches)

# The versus menu's Multiplayer, its choice of how (vita_optart.c Vita_VersusNetPlates): the battle type's three plates
# (Single Battle, Team Battle, DP Battle: textures 2 and 3, plain and highlighted) read Two Controllers, Ad Hoc: Host and
# Ad Hoc: Join while it is open, written into the unpacked screen and back again.
_, sec8 = sec_bytes(8, 'vs')
vs_t = bytearray(sec8)
for section, runs in out_vs:
    if section == 8:
        for off, _o, n in runs:
            vs_t[off:off + len(n)] = n
vs_n = bytearray(vs_t)
for e in (2, 3):
    vs_n = label_job(8, e, [(0, 'Two Controllers', 4, 'left'), (32, 'Ad Hoc: Host', 4, 'left'), (64, 'Ad Hoc: Join', 4, 'left')],
                     0, pack='vs', base_sec=vs_n, keep=False)
vsnet_runs = []
i = 0
while i < len(vs_t):
    if vs_t[i] != vs_n[i]:
        j = i
        while j < len(vs_t) and (vs_t[j] != vs_n[j] or (j + 1 < len(vs_t) and vs_t[j + 1] != vs_n[j + 1]) or
                                 (j + 2 < len(vs_t) and vs_t[j + 2] != vs_n[j + 2])):
            j += 1
        vsnet_runs.append((i, j - i))
        i = j
    else:
        i += 1

# the button icons of the game's text (file 5, member 8): L3 / R3 as plates like L1, not the PS2's sticks
patches, previews = [], []
for e, (idx, pal) in sorted(vita_icons.make(sec_bytes(8, 'boot')[1]).items()):
    icon_job(e, idx, pal)
out_icons = runs_of(patches)

lines = ['/* Generated by tools/gen_patches.py from the USA disc\'s files: the art changed for the Vita. Each section: runs',
         '   of bytes to change, and the FNV-1a of the bytes they replace (a disc whose art differs is left alone). */',
         '#include <stdint.h>', '', '/* The options screen (file 454, section 1): its controller page. */']
emit(lines, out, 'OPTART_S', 'OptArt')
lines += ['', '/* Vita Settings: the runs of the second caption strip (section 14) that differ between the captions of Touch Buttons',
          '   (as patched above) and those of the page above it, and both versions of their bytes. */',
          'static const uint32_t kOptCapRuns[][2] = { /* offset in section 14, length */']
for off, ln in cap_runs:
    lines.append(f'    {{0x{off:X}, {ln}}},')
lines.append('};')
for name, src in (('kOptCapT', cap_t), ('kOptCapV', cap_v), ('kOptCapL', cap_l), ('kOptCapX', cap_x), ('kOptCapY', cap_y),
                  ('kOptCapG', cap_g), ('kOptCapH', cap_h)):
    blob = b''.join(bytes(src[off:off + ln]) for off, ln in cap_runs)
    lines.append(f'static const uint8_t {name}[{len(blob)}] = {{')
    for i in range(0, len(blob), 32):
        lines.append('    ' + ','.join(str(b) for b in blob[i:i + 32]) + ',')
    lines.append('};')
print('captions: runs', len(cap_runs), 'bytes', sum(l for _, l in cap_runs))
# previews of the second and third sets
for e in (1, 3):
    for tag, src in (('v', cap_v), ('l', cap_l), ('x', cap_x), ('y', cap_y), ('g', cap_g), ('h', cap_h)):
        t = texture(bytes(src), e)
        Image.fromarray(t['rgba'].copy(), 'RGBA').save(f'{base}/patched_opt_s14_t{e}_{tag}.png')
lines += ['', '/* The versus menu (file 449, section 1): the plate "1P VS 2P" reads "Multiplayer". */']
emit(lines, out_vs, 'VSART_S', 'VsArt')
lines += ['', '/* The versus menu\'s Multiplayer choice: the runs of section 8 that differ between the battle types\' lettering (as the',
          '   disc has it) and Two Controllers / Ad Hoc: Host / Ad Hoc: Join, and both versions of their bytes. */',
          'static const uint32_t kVsNetRuns[][2] = { /* offset in section 8, length */']
for off, ln in vsnet_runs:
    lines.append(f'    {{0x{off:X}, {ln}}},')
lines.append('};')
for name, src in (('kVsNetT', vs_t), ('kVsNetN', vs_n)):
    blob = b''.join(bytes(src[off:off + ln]) for off, ln in vsnet_runs)
    lines.append(f'static const uint8_t {name}[{len(blob)}] = {{')
    for i in range(0, len(blob), 32):
        lines.append('    ' + ','.join(str(b) for b in blob[i:i + 32]) + ',')
    lines.append('};')
print('versus net plates: runs', len(vsnet_runs), 'bytes', sum(l for _, l in vsnet_runs))
for e in (2, 3):
    Image.fromarray(texture(bytes(vs_n), e)['rgba'].copy(), 'RGBA').save(f'{base}/patched_vs_s8_t{e}_net.png')
lines += ['', '/* The button icons of the game\'s text (file 5, the boot file, member 8): L3 and R3 drawn as plates like L1, not as',
          '   the PS2\'s sticks, and held down the same. */']
emit(lines, out_icons, 'ICONART_S', 'IconArt')
text = '\n'.join(lines) + '\n'
if ES:
    import re
    text = re.sub(r'\b(k(?:OptArt|OptCap|VsArt|VsNet|IconArt)\w*?)(\[|\b)', r'\1Es\2', text)
    text = re.sub(r'\b(OPTART|VSART|ICONART)_S(\d+)', r'\1_ES_S\2', text)
    text = text.replace("from the USA disc's files", "from the Spanish disc's files")
open(sys.argv[2], 'w').write(text)

# previews: decode the patched sections again
vis = []
for pack, o, shows in (('opt', out, {16: (0, 1, 2, 3), 14: (1, 3), 18: (5,), 21: (0,)}), ('vs', out_vs, {8: (0, 1)}),
                       ('boot', out_icons, {8: (16, 19, 26, 27)})):
    for section, runs in o:
        a, sec = sec_bytes(section, pack)
        new = bytearray(sec)
        for off, _, n in runs:
            new[off:off + len(n)] = n
        for e in shows[section]:
            t = texture(bytes(new), e)
            im = Image.fromarray(t['rgba'].copy(), 'RGBA')
            im.save(f'{base}/patched_{pack}_s{section}_t{e}.png')
