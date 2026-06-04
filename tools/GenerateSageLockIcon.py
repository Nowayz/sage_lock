
from pathlib import Path
from math import sqrt
import struct

from PIL import Image, ImageDraw
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen
from fontTools.misc.transform import Transform

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "sage_lock"
FONT_PATH = Path(r"C:\Windows\Fonts\seguiemj.ttf")
EMOJI_CODEPOINT = 0x1F512
PREVIEW_SIZES = [16, 24, 32, 48, 64, 128, 256]
MASTER_SIZE = 1024
MARGIN = 96

class FlatGlyphPen(BasePen):
    def __init__(self, glyph_set, transform):
        super().__init__(glyph_set)
        self.transform = transform
        self.contours = []
        self.current = []

    def _tp(self, p):
        return self.transform.transformPoint(p)

    def _moveTo(self, p0):
        if self.current:
            self.contours.append(self.current)
        self.current = [self._tp(p0)]

    def _lineTo(self, p1):
        self.current.append(self._tp(p1))

    def _qCurveToOne(self, p1, p2):
        if not self.current:
            self.current = [self._tp(p2)]
            return
        p0 = self.current[-1]
        q1 = self._tp(p1)
        q2 = self._tp(p2)
        for i in range(1, 18):
            t = i / 17.0
            mt = 1.0 - t
            x = mt * mt * p0[0] + 2 * mt * t * q1[0] + t * t * q2[0]
            y = mt * mt * p0[1] + 2 * mt * t * q1[1] + t * t * q2[1]
            self.current.append((x, y))

    def _curveToOne(self, p1, p2, p3):
        if not self.current:
            self.current = [self._tp(p3)]
            return
        p0 = self.current[-1]
        c1 = self._tp(p1)
        c2 = self._tp(p2)
        c3 = self._tp(p3)
        for i in range(1, 22):
            t = i / 21.0
            mt = 1.0 - t
            x = mt**3 * p0[0] + 3 * mt**2 * t * c1[0] + 3 * mt * t**2 * c2[0] + t**3 * c3[0]
            y = mt**3 * p0[1] + 3 * mt**2 * t * c1[1] + 3 * mt * t**2 * c2[1] + t**3 * c3[1]
            self.current.append((x, y))

    def _closePath(self):
        if self.current:
            self.contours.append(self.current)
            self.current = []

    def _endPath(self):
        self._closePath()

def color_tuple(c, alpha=1.0):
    return (int(c.red), int(c.green), int(c.blue), int(c.alpha * alpha))

def lerp(a, b, t):
    return int(a + (b - a) * t)

def color_stops(palette, color_line):
    stops = []
    for stop in color_line.ColorStop:
        c = palette[stop.PaletteIndex]
        stops.append((float(stop.StopOffset), color_tuple(c, float(stop.Alpha))))
    return sorted(stops, key=lambda x: x[0])

def sample_stops(stops, t):
    if not stops:
        return (0, 0, 0, 0)
    if t <= stops[0][0]:
        return stops[0][1]
    if t >= stops[-1][0]:
        return stops[-1][1]
    for (o0, c0), (o1, c1) in zip(stops, stops[1:]):
        if o0 <= t <= o1:
            local = 0.0 if o1 == o0 else (t - o0) / (o1 - o0)
            return tuple(lerp(c0[i], c1[i], local) for i in range(4))
    return stops[-1][1]

def render_linear(size, stops, p0, p1):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pix = img.load()
    vx = p1[0] - p0[0]
    vy = p1[1] - p0[1]
    denom = vx * vx + vy * vy or 1.0
    for y in range(size):
        for x in range(size):
            t = ((x - p0[0]) * vx + (y - p0[1]) * vy) / denom
            pix[x, y] = sample_stops(stops, max(0.0, min(1.0, t)))
    return img

def render_radial(size, stops, center, radius):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pix = img.load()
    r = radius or 1.0
    for y in range(size):
        for x in range(size):
            t = sqrt((x - center[0]) ** 2 + (y - center[1]) ** 2) / r
            pix[x, y] = sample_stops(stops, max(0.0, min(1.0, t)))
    return img

def make_mask(size, glyph_set, glyph_name, transform):
    pen = FlatGlyphPen(glyph_set, transform)
    glyph_set[glyph_name].draw(pen)
    pen._endPath()
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    for contour in pen.contours:
        if len(contour) >= 3:
            draw.polygon([(round(x), round(y)) for x, y in contour], fill=255)
    return mask

def tx_from_paint_transform(t):
    return Transform(t.xx, t.xy, t.yx, t.yy, t.dx, t.dy)

def collect_glyph_bounds(font, colr, paint, transform, boxes):
    if paint.Format == 1:
        for i in range(paint.FirstLayerIndex, paint.FirstLayerIndex + paint.NumLayers):
            collect_glyph_bounds(font, colr, colr.LayerList.Paint[i], transform, boxes)
    elif paint.Format == 12:
        collect_glyph_bounds(font, colr, paint.Paint, transform.transform(tx_from_paint_transform(paint.Transform)), boxes)
    elif paint.Format == 16:
        collect_glyph_bounds(font, colr, paint.Paint, transform.transform(Transform(paint.scaleX, 0, 0, paint.scaleY, 0, 0)), boxes)
    elif paint.Format == 10:
        glyph = font.getGlyphSet()[paint.Glyph]
        box = glyph.width, 0
        pen = FlatGlyphPen(font.getGlyphSet(), transform)
        glyph.draw(pen)
        pen._endPath()
        pts = [pt for contour in pen.contours for pt in contour]
        if pts:
            boxes.append((min(x for x, y in pts), min(y for x, y in pts), max(x for x, y in pts), max(y for x, y in pts)))

def render_paint(base, font, colr, palette, paint, transform, layout_transform, clip_mask=None):
    size = base.size[0]
    glyph_set = font.getGlyphSet()
    if paint.Format == 1:
        for i in range(paint.FirstLayerIndex, paint.FirstLayerIndex + paint.NumLayers):
            render_paint(base, font, colr, palette, colr.LayerList.Paint[i], transform, layout_transform, clip_mask)
    elif paint.Format == 12:
        render_paint(base, font, colr, palette, paint.Paint, transform.transform(tx_from_paint_transform(paint.Transform)), layout_transform, clip_mask)
    elif paint.Format == 16:
        render_paint(base, font, colr, palette, paint.Paint, transform.transform(Transform(paint.scaleX, 0, 0, paint.scaleY, 0, 0)), layout_transform, clip_mask)
    elif paint.Format == 10:
        shape_transform = transform.transform(layout_transform)
        mask = make_mask(size, glyph_set, paint.Glyph, shape_transform)
        if clip_mask is not None:
            mask = Image.composite(mask, Image.new("L", (size, size), 0), clip_mask)
        render_paint(base, font, colr, palette, paint.Paint, transform, layout_transform, mask)
    elif paint.Format == 2:
        if clip_mask is None:
            return
        fill = Image.new("RGBA", (size, size), color_tuple(palette[paint.PaletteIndex], float(paint.Alpha)))
        base.alpha_composite(Image.composite(fill, Image.new("RGBA", (size, size), (0, 0, 0, 0)), clip_mask))
    elif paint.Format == 4:
        if clip_mask is None:
            return
        full = transform.transform(layout_transform)
        p0 = full.transformPoint((paint.x0, paint.y0))
        p1 = full.transformPoint((paint.x1, paint.y1))
        fill = render_linear(size, color_stops(palette, paint.ColorLine), p0, p1)
        base.alpha_composite(Image.composite(fill, Image.new("RGBA", (size, size), (0, 0, 0, 0)), clip_mask))
    elif paint.Format == 6:
        if clip_mask is None:
            return
        full = transform.transform(layout_transform)
        c = full.transformPoint((paint.x1, paint.y1))
        edge = full.transformPoint((paint.x1 + paint.r1, paint.y1))
        radius = sqrt((edge[0] - c[0]) ** 2 + (edge[1] - c[1]) ** 2)
        fill = render_radial(size, color_stops(palette, paint.ColorLine), c, radius)
        base.alpha_composite(Image.composite(fill, Image.new("RGBA", (size, size), (0, 0, 0, 0)), clip_mask))

def center_alpha_horizontally(image):
    bbox = image.getchannel("A").getbbox()
    if not bbox:
        return image

    left, _, right, _ = bbox
    left_padding = left
    right_padding = image.width - right
    shift_x = round((right_padding - left_padding) / 2.0)
    if shift_x == 0:
        return image

    centered = Image.new("RGBA", image.size, (0, 0, 0, 0))
    centered.alpha_composite(image, (shift_x, 0))
    return centered

def make_icon():
    font = TTFont(str(FONT_PATH))
    glyph_name = font.getBestCmap()[EMOJI_CODEPOINT]
    colr = font["COLR"].table
    palette = font["CPAL"].palettes[0]
    rec = next(r for r in colr.BaseGlyphList.BaseGlyphPaintRecord if r.BaseGlyph == glyph_name)

    boxes = []
    collect_glyph_bounds(font, colr, rec.Paint, Transform(), boxes)
    left = min(b[0] for b in boxes)
    bottom = min(b[1] for b in boxes)
    right = max(b[2] for b in boxes)
    top = max(b[3] for b in boxes)
    scale = min((MASTER_SIZE - 2 * MARGIN) / (right - left), (MASTER_SIZE - 2 * MARGIN) / (top - bottom))
    used_w = (right - left) * scale
    used_h = (top - bottom) * scale
    dx = (MASTER_SIZE - used_w) / 2 - left * scale
    dy = (MASTER_SIZE - used_h) / 2 + top * scale
    layout = Transform(scale, 0, 0, -scale, dx, dy)

    master = Image.new("RGBA", (MASTER_SIZE, MASTER_SIZE), (0, 0, 0, 0))
    render_paint(master, font, colr, palette, rec.Paint, Transform(), layout)
    master = center_alpha_horizontally(master)
    master.save(OUT_DIR / "sage_lock-1024.png")

    frames = []
    for size in PREVIEW_SIZES:
        frame = master.resize((size, size), Image.Resampling.LANCZOS)
        frame_path = OUT_DIR / f"sage_lock-{size}.png"
        frame.save(frame_path)
        frames.append((size, frame))

    ico_path = OUT_DIR / "sage_lock.ico"
    frames[-1][1].save(ico_path, sizes=[(s, s) for s in PREVIEW_SIZES])
    print(f"Generated {ico_path}")

if __name__ == "__main__":
    make_icon()
