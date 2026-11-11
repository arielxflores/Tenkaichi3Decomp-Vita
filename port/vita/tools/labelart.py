"""Draws new menu labels in the style of the options screen's lettering (outlined bold letters with a soft shadow)."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
SS = 4

def font_for_cap(cap):
    lo, hi = 8, 80
    for _ in range(30):
        mid = (lo + hi) / 2
        f = ImageFont.truetype(FONT, int(round(mid * SS)))
        b = f.getbbox('H')
        h = (b[3] - b[1]) / SS
        if h < cap: lo = mid
        else: hi = mid
    return ImageFont.truetype(FONT, int(round(hi * SS)))

def disk(r):
    y, x = np.mgrid[-r:r + 1, -r:r + 1]
    return (x * x + y * y) <= r * r + r * 0.5

def dilate(mask, r):
    from scipy.ndimage import binary_dilation
    return binary_dilation(mask, structure=disk(r))

def render(text, cap, x, baseline, w, h, fill_rows, outline_rows, shadow_rgb, embolden=0.6, outline=3.0, shadow=(1, 2), shadow_alpha=60, align='left', tracking=0.0):
    """-> float RGBA (0..255 rgb, 0..128 alpha) of size h x w; fill_rows/outline_rows: dict y -> rgb"""
    f = font_for_cap(cap)
    W, H = w * SS, h * SS
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    asc = f.getbbox('H')[1]  # top of H from the drawing origin
    capb = f.getbbox('H')[3]
    # glyph by glyph, with tracking
    widths = []
    cx = 0.0
    for ch in text:
        widths.append(f.getlength(ch) + tracking * SS)
    total = sum(widths) - tracking * SS
    if align == 'center':
        x0 = x * SS - total / 2
    else:
        x0 = x * SS
    yorg = baseline * SS - capb
    cx = x0
    for ch, wd in zip(text, widths):
        d.text((cx, yorg), ch, font=f, fill=255)
        cx += wd
    m = np.array(img) > 127
    if embolden > 0:
        m = dilate(m, int(round(embolden * SS)))
    o = dilate(m, int(round(outline * SS)))
    def down(b):
        return b.reshape(h, SS, w, SS).mean(axis=(1, 3))
    F = down(m.astype(float))
    O = down(o.astype(float))
    sh = np.zeros_like(O)
    dx, dy = shadow
    sh[dy:, dx:] = O[:h - dy, :w - dx]
    sh = np.array(Image.fromarray((sh * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2))) / 255.0
    out = np.zeros((h, w, 4))
    def over(dst, rgb, a):  # a: 0..1 coverage, alpha scale 128
        a = a[..., None]
        src_a = a * 128.0
        da = dst[..., 3:4]
        oa = src_a + da * (1 - a)
        rgbo = np.where(oa > 0, (rgb * src_a + dst[..., :3] * da * (1 - a)) / np.maximum(oa, 1e-6), 0)
        dst[..., :3] = rgbo
        dst[..., 3:4] = oa
    ys = np.arange(h)
    def rowcolors(rows):
        keys = sorted(rows)
        # (a row without colours of its own takes the nearest that has them: inside a label's rows too, where a
        # thin stroke can leave one out)
        return np.array([rows[y] if y in rows else rows[min(keys, key=lambda k: (abs(k - y), k))] for y in ys], dtype=float)[:, None, :] * np.ones((1, w, 1))
    over(out, np.array(shadow_rgb, float)[None, None, :] * np.ones((h, w, 1)), sh * (shadow_alpha / 128.0))
    over(out, rowcolors(outline_rows), O)
    over(out, rowcolors(fill_rows), F)
    return out

def row_stats(rgba, y0, y1, x0=0, x1=None):
    """fill / outline colours per row of an existing label (PS2 alpha scale)"""
    a = rgba.astype(int)
    if x1 is None: x1 = a.shape[1]
    fill, outl = {}, {}
    for y in range(y0, y1):
        r = a[y, x0:x1]
        A = r[:, 3]; L = r[:, :3].sum(1)
        fm = (A > 100) & (L > 400); om = (A > 100) & (L <= 330)
        if fm.sum() > 3: fill[y] = r[fm][:, :3].mean(0)
        if om.sum() > 3: outl[y] = r[om][:, :3].mean(0)
    return fill, outl

def quantize(region, colors):
    """region h x w x 4 (alpha 0..128) -> palette indices (nearest; alpha weighted)"""
    c = colors.astype(float)
    r = region.reshape(-1, 4)
    # transparent-ish pixels: weight colour by alpha
    wa = (r[:, 3:4] / 128.0)
    d = ((r[:, None, :3] - c[None, :, :3]) ** 2 * (0.25 + wa[:, None, :])).sum(2) + ((r[:, None, 3] - c[None, :, 3]) * 3.0) ** 2
    return d.argmin(1).reshape(region.shape[:2])
