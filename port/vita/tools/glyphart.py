"""New words made of the disc's own letters: each letter's body (its fill) cut out of a word the texture already
has, set side by side at the disc's spacing, and the outline, the glow and the shadow drawn around them again from
the texture's own: every pixel takes the colour that pixels at the same height and the same distance from the
letters have in that texture's labels (so the gradient down the letters and the rings around them are the disc's)."""
import numpy as np
from scipy import ndimage


TITLE = False  # the pages' titles: the letters' bodies are the red ones (the pale ring around all is red too, but one
               # piece the whole word wide, and left out)


def fill_mask(rgba, y0, y1):
    """the letters' bodies in rows y0..y1 of a texture (PS2 alpha 0..128): opaque and bright"""
    a = rgba[y0:y1].astype(int)
    if TITLE:
        m = (a[..., 3] > 100) & (a[..., 0] > 200)
        lab, n = ndimage.label(m)
        keep = np.zeros(n + 1, bool)
        for k, o in enumerate(ndimage.find_objects(lab), 1):
            keep[k] = o is not None and o[1].stop - o[1].start < 48
        return keep[lab] & m
    return (a[..., 3] > 100) & (a[..., :3].sum(2) > 400)


def glyphs(rgba, y0, y1):
    """the letters of the word in rows y0..y1, left to right: (x0, x1, mask) with mask the full cell's height
    (parts whose columns overlap are one letter: the i's dot)"""
    m = fill_mask(rgba, y0, y1)
    lab, n = ndimage.label(m)
    parts = []
    for k in range(1, n + 1):
        ys, xs = np.where(lab == k)
        if len(xs) < 3:
            continue
        parts.append([xs.min(), xs.max(), lab == k])
    parts.sort(key=lambda p: p[0])
    out = []
    for p in parts:
        if out and p[0] <= out[-1][1]:
            out[-1][1] = max(out[-1][1], p[1])
            out[-1][2] = out[-1][2] | p[2]
        else:
            out.append(p)
    return [(x0, x1, mk) for x0, x1, mk in out]


def gaps(gl):
    return [gl[i + 1][0] - gl[i][1] - 1 for i in range(len(gl) - 1)]


def style_table(rgba, cells, h, dmax=12, shape=None):
    """per row of a cell (0..h-1) and signed distance from the letters' bodies (inside negative, -3..dmax): the
    texture's mean colour and alpha there, from its labels in the given cells (shape: the texture whose bodies
    are taken, the plain one of a highlighted strip, whose letters lie in the same pixels and are darker at the
    foot)"""
    acc = {}
    for y0 in cells:
        a = rgba[y0:y0 + h].astype(float)
        m = fill_mask(rgba if shape is None else shape, y0, y0 + h)
        if not m.any():
            continue
        dout = ndimage.distance_transform_edt(~m)
        din = ndimage.distance_transform_edt(m)
        d = np.where(m, -np.round(din - 1), np.round(dout)).astype(int)
        for y in range(h):
            for x in range(a.shape[1]):
                dd = d[y, x]
                if dd > dmax or dd < -3:
                    continue
                acc.setdefault((y, dd), []).append(a[y, x])
    return {k: np.mean(v, 0) for k, v in acc.items()}


def paint(mask, table, dmax=12):
    """the colours (h x w x 4, PS2 alpha) around a mask of letter bodies, from a style_table"""
    h, w = mask.shape
    dout = ndimage.distance_transform_edt(~mask)
    din = ndimage.distance_transform_edt(mask)
    d = np.where(mask, -np.round(din - 1), np.round(dout)).astype(int)
    out = np.zeros((h, w, 4))
    for y in range(h):
        for x in range(w):
            dd = max(-3, d[y, x])
            if dd > dmax:
                continue
            key = (y, dd)
            if key not in table:  # (the nearest row that has the distance)
                ks = [k for k in table if k[1] == dd]
                if not ks:
                    continue
                key = min(ks, key=lambda k: abs(k[0] - y))
            out[y, x] = table[key]
    return out


def compose(pieces, gap, space, x, h, w):
    """pieces: glyph masks (full cell height) or None for a space; set from x with `gap` columns between letters"""
    out = np.zeros((h, w), bool)
    cx = x
    for p in pieces:
        if p is None:
            cx += space
            continue
        x0, x1, mk = p
        cols = mk[:, x0:x1 + 1]
        if cx + cols.shape[1] > w:
            raise ValueError('the word does not fit')
        out[:, cx:cx + cols.shape[1]] |= cols
        cx += cols.shape[1] + gap
    return out
