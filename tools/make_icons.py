#!/usr/bin/env python3
"""Draws PenguinCAD's toolbar icons: resources/icons/<command id>.svg.

    python3 tools/make_icons.py            # rewrites resources/icons/ and
                                           # resources/icons.qrc

The SVGs are committed, so the app builds without Python; this script is how
they are kept to one house style (docs/ARCHITECTURE.md, "Toolbar icons").
Every icon is a function of a few shared parts -- an isometric projection,
shaded boxes, arrows, sketch strokes -- so a change of palette or line
weight is one edit here rather than a hundred by hand.

House style, modelled on Fusion 360's toolbar: a 32 x 32 viewBox with two
units of margin. Solids are small isometric shapes with a light top, a mid
left face and a dark right face, outlined in 1.25 px ink; the part an
operation adds or changes is Fusion blue. Sketch tools are 1.75 px line
drawings with blue points. Line work that sits on bare background strokes
in currentColor, which the root sets to the ink colour: the app swaps that
one attribute on a dark palette (src/ui/CommandIcon.cpp), so a sketch line
turns light while a solid keeps its dark edges against its own light faces.
"""
import math
import os
import sys

LIGHT, MID, DARK, INK = '#DDE6EE', '#AAB7C4', '#6E7F90', '#2B3A48'
BLUE, LBLUE, DBLUE = '#0696D7', '#7CC6EA', '#0473A6'
GREEN, ORANGE, RED = '#3DA84A', '#E0892B', '#D9463E'
C30 = math.cos(math.radians(30))

GREY = (LIGHT, MID, DARK)
BLUES = (LBLUE, BLUE, DBLUE)

ICONS = {}


def icon(name):
    def wrap(fn):
        ICONS[name] = fn
        return fn
    return wrap


def f(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def pts(ps):
    return ' '.join('%s,%s' % (f(x), f(y)) for x, y in ps)


def outline(w=1.25, ink=INK):
    return 'stroke="%s" stroke-width="%s" stroke-linejoin="round"' % (ink, f(w))


class Iso:
    def __init__(self, cx, cy, k=1.0):
        self.cx, self.cy, self.k = cx, cy, k

    def __call__(self, x, y, z):
        return (self.cx + (x - y) * C30 * self.k, self.cy + (x + y) * 0.5 * self.k - z * self.k)


def fit(points3d, x0=2.5, y0=2.5, x1=29.5, y1=29.5, kmax=1.6, snap=0.5):
    """An Iso that centres the given world points in the box.

    The leftmost and rightmost points -- on a box, its outer vertical
    edges -- land on x = n + snap, and a symmetric shape's middle edge
    with them: 0.5 suits a 1.25 px outline, 0 a stroke of 1.5 or more.
    Left anywhere, a vertical edge renders as two half-grey columns."""
    probe = Iso(0, 0, 1)
    sp = [probe(*p) for p in points3d]
    minx, maxx = min(p[0] for p in sp), max(p[0] for p in sp)
    miny, maxy = min(p[1] for p in sp), max(p[1] for p in sp)
    k = min((x1 - x0) / max(maxx - minx, 1e-6), (y1 - y0) / max(maxy - miny, 1e-6), kmax)
    if snap is not None and maxx - minx > 1e-6:
        k = 2 * math.floor(k * (maxx - minx) / 2) / (maxx - minx)
    cx = (x0 + x1) / 2 - k * (minx + maxx) / 2
    if snap is not None:
        left = cx + k * minx
        cx += math.floor(left - snap + 0.5) + snap - left
    cy = (y0 + y1) / 2 - k * (miny + maxy) / 2
    return Iso(cx, cy, k)


def corners(x0, y0, z0, x1, y1, z1):
    return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def poly(ps, fill, stroke=True, w=1.25, extra='', ink=INK):
    s = '<polygon points="%s" fill="%s"' % (pts(ps), fill)
    if stroke:
        s += ' ' + outline(w, ink)
    return s + extra + '/>'


def box(P, x0, y0, z0, x1, y1, z1, shades=GREY, w=1.25, ink=INK):
    top, left, right = shades
    t = [P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)]
    r = [P(x1, y0, z1), P(x1, y1, z1), P(x1, y1, z0), P(x1, y0, z0)]
    l = [P(x0, y1, z1), P(x1, y1, z1), P(x1, y1, z0), P(x0, y1, z0)]
    return poly(l, left, w=w, ink=ink) + poly(r, right, w=w, ink=ink) + poly(t, top, w=w, ink=ink)


def path(d, stroke='currentColor', w=1.75, fill='none', dash=None, cap='round', join='round', extra=''):
    s = '<path d="%s" fill="%s"' % (d, fill)
    if stroke:
        s += ' stroke="%s" stroke-width="%s" stroke-linecap="%s" stroke-linejoin="%s"' % (stroke, f(w), cap, join)
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    return s + extra + '/>'


def line(ps, stroke='currentColor', w=1.75, dash=None, cap='round'):
    d = 'M' + ' L'.join('%s,%s' % (f(x), f(y)) for x, y in ps)
    return path(d, stroke=stroke, w=w, dash=dash, cap=cap)


def dot(x, y, r=2.1, fill=BLUE, ring=None):
    s = '<circle cx="%s" cy="%s" r="%s" fill="%s"' % (f(x), f(y), f(r), fill)
    if ring:
        s += ' stroke="%s" stroke-width="1"' % ring
    return s + '/>'


def square_pt(x, y, a=3.6, fill=BLUE):
    return '<rect x="%s" y="%s" width="%s" height="%s" fill="%s"/>' % (f(x - a / 2), f(y - a / 2), f(a), f(a), fill)


def circle(cx, cy, r, stroke='currentColor', w=1.75, fill='none', dash=None):
    s = '<circle cx="%s" cy="%s" r="%s" fill="%s"' % (f(cx), f(cy), f(r), fill)
    if stroke:
        s += ' stroke="%s" stroke-width="%s"' % (stroke, f(w))
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    return s + '/>'


def ellipse(cx, cy, rx, ry, stroke=INK, w=1.25, fill='none', dash=None, extra=''):
    s = '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="%s"' % (f(cx), f(cy), f(rx), f(ry), fill)
    if stroke:
        s += ' stroke="%s" stroke-width="%s"' % (stroke, f(w))
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    return s + extra + '/>'


def arrowhead(x1, y1, x2, y2, fill, size=4.6, width=None):
    """Filled triangle whose tip is (x2, y2), pointing away from (x1, y1)."""
    width = width if width is not None else size * 0.95
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    bx, by = x2 - ux * size, y2 - uy * size
    nx, ny = -uy * width / 2, ux * width / 2
    return '<polygon points="%s" fill="%s" stroke="%s" stroke-width="0.6" stroke-linejoin="round"/>' % (
        pts([(x2, y2), (bx + nx, by + ny), (bx - nx, by - ny)]), fill, fill)


def arrow(x1, y1, x2, y2, stroke='currentColor', w=1.75, size=4.6, double=False, dash=None):
    """A shaft with a head at (x2, y2), and at (x1, y1) too when double."""
    dx, dy = x2 - x1, y2 - y1
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    sx, sy = (x1 + ux * size * 0.8, y1 + uy * size * 0.8) if double else (x1, y1)
    ex, ey = x2 - ux * size * 0.8, y2 - uy * size * 0.8
    s = line([(sx, sy), (ex, ey)], stroke=stroke, w=w, dash=dash, cap='butt')
    s += arrowhead(x1, y1, x2, y2, stroke, size)
    if double:
        s += arrowhead(x2, y2, x1, y1, stroke, size)
    return s


def arc_pts(cx, cy, rx, ry, a0, a1, n=24, rot=0.0):
    out = []
    cr, sr = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        x, y = rx * math.cos(a), ry * math.sin(a)
        out.append((cx + x * cr - y * sr, cy + x * sr + y * cr))
    return out


def d_of(ps, close=False):
    d = 'M' + ' L'.join('%s,%s' % (f(x), f(y)) for x, y in ps)
    return d + (' Z' if close else '')


def lin_grad(gid, stops, x1=0, y1=0, x2=1, y2=0):
    s = '<linearGradient id="%s" x1="%s" y1="%s" x2="%s" y2="%s">' % (gid, f(x1), f(y1), f(x2), f(y2))
    for off, col in stops:
        s += '<stop offset="%s" stop-color="%s"/>' % (f(off), col)
    return s + '</linearGradient>'


def rad_grad(gid, stops, cx=0.35, cy=0.3, r=0.75):
    s = '<radialGradient id="%s" cx="%s" cy="%s" r="%s">' % (gid, f(cx), f(cy), f(r))
    for off, col in stops:
        s += '<stop offset="%s" stop-color="%s"/>' % (f(off), col)
    return s + '</radialGradient>'


def defs(*items):
    return '<defs>' + ''.join(items) + '</defs>'


def iso_ellipse(P, cx, cy, cz, r):
    """Screen centre and radii of a horizontal circle of radius r."""
    x, y = P(cx, cy, cz)
    return x, y, r * math.sqrt(2) * C30 * P.k, r * math.sqrt(2) * 0.5 * P.k


# Blue gradients for curved new geometry, horizontal (light at the left, as
# the flat faces are lit from the upper left).
BLUE_SIDE = [(0, '#35AEE4'), (0.45, BLUE), (1, DBLUE)]
GREY_SIDE = [(0, LIGHT), (0.45, MID), (1, DARK)]


def cylinder(P, cx, cy, z0, z1, r, gid, side=BLUE_SIDE, top=LBLUE):
    ex, ey0, rx, ry = iso_ellipse(P, cx, cy, z1, r)
    _, ey1, _, _ = iso_ellipse(P, cx, cy, z0, r)
    d = 'M%s,%s L%s,%s A%s,%s 0 0 0 %s,%s L%s,%s A%s,%s 0 0 1 %s,%s Z' % (
        f(ex - rx), f(ey0), f(ex - rx), f(ey1), f(rx), f(ry), f(ex + rx), f(ey1),
        f(ex + rx), f(ey0), f(rx), f(ry), f(ex - rx), f(ey0))
    return (defs(lin_grad(gid, side)) + path(d, stroke=INK, w=1.25, fill='url(#%s)' % gid)
            + ellipse(ex, ey0, rx, ry, fill=top))


def pointer(x, y, s=1.0):
    """A mouse pointer with its tip at (x, y)."""
    shape = [(0, 0), (0, 12), (3, 9.2), (5.2, 13.6), (7.2, 12.6), (5, 8.3), (8.8, 8.3)]
    return poly([(x + px * s, y + py * s) for px, py in shape], '#FFFFFF', w=1.1)


def pencil(tipx, tipy, length=17, width=5.2, body=LIGHT, band=MID, angle=-45):
    a = math.radians(angle)
    ux, uy = math.cos(a), math.sin(a)
    nx, ny = -uy, ux
    hw = width / 2
    cone = 5.0

    def at(t, s):
        return (tipx + ux * t + nx * s, tipy + uy * t + ny * s)

    out = poly([at(cone, -hw), at(length, -hw), at(length, hw), at(cone, hw)], body)
    out += poly([at(length - 3.2, -hw), at(length, -hw), at(length, hw), at(length - 3.2, hw)], band)
    out += poly([at(0, 0), at(cone, -hw), at(cone, hw)], '#F2E3C4')
    out += poly([at(0, 0), at(1.9, -hw * 0.38), at(1.9, hw * 0.38)], INK, stroke=False)
    return out


# ---------------------------------------------------------------- SOLID: CREATE

def sketch_plane(P, a=16):
    return poly([P(0, 0, 0), P(a, 0, 0), P(a, a, 0), P(0, a, 0)], LIGHT)


def sheet(u0=2.5, v0=29, w=19, h=14.5, lean=5.5):
    """A sketch sheet seen steeply from above: a map from (u, v), both 0..1
    across and into it, to the box. Not isometric -- an isometric plane is
    a flat diamond, which left a speck beside its own pencil on the ribbon."""
    return lambda u, v: (u0 + w * u + lean * v, v0 - h * v)


def sketch_on_plane(outline_colour, pencil_body, pencil_band):
    """A rectangle being drawn on a sheet, the pencil at its last corner."""
    on = sheet()
    s = poly([on(0, 0), on(1, 0), on(1, 1), on(0, 1)], LIGHT)
    near, far = 4 / 14.5, 12 / 14.5      # its long sides on the rows y = 25 and 17
    rect = [on(0.18, near), on(0.72, near), on(0.72, far), on(0.18, far)]
    s += path(d_of(rect, True), stroke=outline_colour, w=2.0)
    tip = rect[2]
    s += dot(*tip, r=2.1)
    return s + pencil(*tip, length=15.5, width=5.8, angle=-64, body=pencil_body, band=pencil_band)


@icon('sketch.create')
def _():
    return sketch_on_plane(BLUE, LIGHT, MID)


@icon('sketch.edit')
def _():
    return sketch_on_plane(INK, BLUE, DBLUE)


@icon('solid.box')
def _():
    P = fit(corners(0, 0, 0, 12, 12, 12), 3.5, 2.5, 28.5, 29.5)
    return box(P, 0, 0, 0, 12, 12, 12, BLUES)


@icon('solid.cylinder')
def _():
    P = Iso(16, 22.9, 1.0)
    return cylinder(P, 0, 0, 0, 14, 8.8, 'g')


@icon('solid.sphere')
def _():
    s = defs(rad_grad('g', [(0, '#D6F0FC'), (0.35, '#4DB7E8'), (0.8, BLUE), (1, DBLUE)], 0.36, 0.3, 0.78))
    s += circle(16, 16, 12.6, stroke=INK, w=1.25, fill='url(#g)')
    s += path('M3.6,16.6 A12.4,6.2 0 0 0 28.4,16.6', stroke=INK, w=0.9, extra=' stroke-opacity="0.55"')
    return s


@icon('solid.cone')
def _():
    cx, cy, rx, ry = 16, 24, 11.6, 5.2
    apex = (16, 3.2)
    d = 'M%s,%s L%s,%s A%s,%s 0 0 0 %s,%s Z' % (f(apex[0]), f(apex[1]), f(cx - rx), f(cy), f(rx), f(ry), f(cx + rx), f(cy))
    s = defs(lin_grad('g', BLUE_SIDE))
    s += path(d, stroke=INK, w=1.25, fill='url(#g)')
    return s


@icon('solid.torus')
def _():
    # A ring seen from 30 degrees up is taller than a flat washer: the tube
    # stands above and below its own centre circle. Drawn flatter, it read
    # as a washer at 24 px. The hole shows the far inner wall in shadow.
    cx, cy = 16, 16.4
    R, ry = 13.4, 9.8      # outer silhouette
    hx, hy = 5.0, 2.6      # the hole
    hcy = cy - 2.3
    s = defs(lin_grad('g', [(0, '#9ED6F1'), (0.3, '#3FAEE3'), (0.72, BLUE), (1, DBLUE)], 0, 0, 0, 1))
    outer = arc_pts(cx, cy, R, ry, 0, 360, n=48)
    hole = arc_pts(cx, hcy, hx, hy, 0, 360, n=32)
    s += path(d_of(outer, True) + ' ' + d_of(hole, True), stroke=INK, w=1.25, fill='url(#g)',
              extra=' fill-rule="evenodd"')
    wall = arc_pts(cx, hcy, hx, hy, 180, 360, n=16) + list(
        reversed(arc_pts(cx, hcy + 0.6, hx, hy * 0.25, 180, 360, n=16)))
    s += poly(wall, DBLUE, w=1.0)
    # The lit crown of the tube, where a torus catches the light first.
    s += path(d_of(arc_pts(cx, hcy + 0.4, 9.2, 5.4, 200, 340, n=20)), stroke='#D6F0FC', w=1.6,
              extra=' stroke-opacity="0.9"')
    return s


@icon('solid.extrude')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0) + [(4, 4, 8), (12, 12, 8), (8, 8, 19)], 2.5, 2, 29.5, 30)
    s = poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT)
    s += box(P, 4, 4, 0, 12, 12, 8, BLUES)
    x, y = P(8, 8, 8)
    x2, y2 = P(8, 8, 19)
    x = x2 = round(x)     # a 2 px shaft on whole columns, not straddling three
    s += arrow(x, y, x2, y2, w=2.0, size=5.0)
    return s


@icon('solid.revolve')
def _():
    # A rectangular profile (grey) revolved 270 degrees about an axis it
    # does not touch: a ring with its front quarter missing, which is what
    # makes it read as swept rather than just drawn.
    Ri, R, h = 4.6, 10.0, 6.5
    P = fit([(R * math.cos(math.radians(a)), R * math.sin(math.radians(a)), z)
             for a in range(0, 360, 15) for z in (0, h)] + [(0, 0, -3.5), (0, 0, h + 5.5)], 2.5, 2, 29.5, 30)

    def rim(r, a0, a1, z, n=12):
        return [P(r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                  r * math.sin(math.radians(a0 + (a1 - a0) * i / n)), z) for i in range(n + 1)]
    s = poly(rim(Ri, 135, 315, h, 18) + list(reversed(rim(Ri, 135, 315, h - 4.5, 18))), DBLUE)
    s += poly(rim(R, 90, 135, h) + list(reversed(rim(R, 90, 135, 0))), BLUE)
    s += poly(rim(R, -45, 0, h) + list(reversed(rim(R, -45, 0, 0))), DBLUE)
    s += poly([P(Ri, 0, 0), P(R, 0, 0), P(R, 0, h), P(Ri, 0, h)], BLUE)
    s += poly([P(0, Ri, 0), P(0, R, 0), P(0, R, h), P(0, Ri, h)], LIGHT)
    s += poly(rim(R, 90, 360, h, 36) + list(reversed(rim(Ri, 90, 360, h, 36))), LBLUE)
    s += line([P(0, 0, -3.5), P(0, 0, h + 5.5)], w=1.5, dash='4.5 1.5 1.2 1.5')
    return s


@icon('solid.sweep')
def _():
    d = 'M7,25 C7,14 14,8 25,8'
    s = path(d, stroke=INK, w=9.2, cap='butt')
    s += path(d, stroke=BLUE, w=6.7, cap='butt')
    s += path('M5.2,23 C5.4,14 12.4,6.6 23,6.2', stroke=LBLUE, w=1.3, cap='butt')
    s += ellipse(7, 25.6, 4.6, 1.9, fill=LIGHT, w=1.25)
    s += ellipse(25.6, 8, 1.9, 4.6, fill=LBLUE, w=1.25)
    return s


@icon('solid.loft')
def _():
    P = fit(corners(-2, -2, 0, 16, 16, 0) + [(7, 7, 14)], 2.5, 3, 29.5, 29.5)
    s = poly([P(-2, -2, 0), P(16, -2, 0), P(16, 16, 0), P(-2, 16, 0)], LIGHT)
    ex, ey, rx, ry = iso_ellipse(P, 7, 7, 13, 4.2)
    left, front, right = P(0, 14, 0), P(14, 14, 0), P(14, 0, 0)
    s += poly([left, front, (ex, ey + ry), (ex - rx, ey)], BLUE)
    s += poly([front, right, (ex + rx, ey), (ex, ey + ry)], DBLUE)
    s += ellipse(ex, ey, rx, ry, fill=LIGHT)
    return s


def boss(P, x, y, z0, h, r, shades=(LBLUE, BLUE), w=1.0):
    """A short upright cylinder standing on z0: a pattern's feature."""
    top, side = shades
    ex, ey0, rx, ry = iso_ellipse(P, x, y, z0 + h, r)
    _, ey1, _, _ = iso_ellipse(P, x, y, z0, r)
    d = 'M%s,%s L%s,%s A%s,%s 0 0 0 %s,%s L%s,%s Z' % (
        f(ex - rx), f(ey0), f(ex - rx), f(ey1), f(rx), f(ry), f(ex + rx), f(ey1), f(ex + rx), f(ey0))
    return path(d, stroke=INK, w=w, fill=side) + ellipse(ex, ey0, rx, ry, fill=top, w=w)


def disc(P, cx, cy, z0, z1, r, top=LIGHT, side=MID):
    """A flat round plate, flat-shaded like the boxes."""
    ex, ey0, rx, ry = iso_ellipse(P, cx, cy, z1, r)
    _, ey1, _, _ = iso_ellipse(P, cx, cy, z0, r)
    d = 'M%s,%s L%s,%s A%s,%s 0 0 0 %s,%s L%s,%s Z' % (
        f(ex - rx), f(ey0), f(ex - rx), f(ey1), f(rx), f(ry), f(ex + rx), f(ey1), f(ex + rx), f(ey0))
    return path(d, stroke=INK, w=1.25, fill=side) + ellipse(ex, ey0, rx, ry, fill=top)


# The seed of a pattern stays grey, its copies are what the command adds.
SEED = (LIGHT, MID)


# The two patterns have to part at 16 px, where the menus draw them, and
# tall pegs merged both into one blue clump there. So the features are
# squat and the plate carries the difference: a square plate is a diamond,
# a round one an oval. Square studs in rows against round studs in a ring
# say it a second time.
@icon('solid.pattern.rectangular')
def _():
    A, t, h, a = 16, 3.0, 4.5, 5.0
    P = fit(corners(0, 0, 0, A, A, t + h), 2.5, 2.5, 29.5, 29.5)
    s = box(P, 0, 0, 0, A, A, t)
    for x, y in sorted([(u, v) for u in (2, 9) for v in (2, 9)], key=lambda q: q[0] + q[1]):
        shades = GREY if (x, y) == (2, 2) else BLUES
        s += box(P, x, y, t, x + a, y + a, t + h, shades, w=1.0)
    return s


@icon('solid.pattern.circular')
def _():
    R, t, h, r, pitch = 11, 3.0, 4.5, 2.4, 7.4
    P = fit([(R * math.cos(math.radians(a)), R * math.sin(math.radians(a)), z)
             for a in range(0, 360, 10) for z in (0, t + h)], 2.5, 2.5, 29.5, 29.5)
    s = disc(P, 0, 0, 0, t, R)
    places = [(pitch * math.cos(math.radians(a)), pitch * math.sin(math.radians(a)), a == 225)
              for a in range(45, 405, 60)]
    for x, y, seed in sorted(places, key=lambda q: q[0] + q[1]):
        s += boss(P, x, y, t, h, r, SEED if seed else (LBLUE, BLUE))
    return s


def step_block(P, x0, x1, y0, y1, tall, low, split, along_y, shades):
    """An L-shaped block, low up to split along its long axis and tall
    beyond it. along_y picks which axis is the long one."""
    if along_y:
        return box(P, x0, y0, 0, x1, split, low, shades) + box(P, x0, split, 0, x1, y1, tall, shades)
    return box(P, x0, y0, 0, split, y1, low, shades) + box(P, split, y0, 0, x1, y1, tall, shades)


@icon('solid.mirror')
def _():
    # The mirror plane is x = y, which isometric shows edge-on as the
    # vertical through the middle: the grey original and its blue copy
    # stand either side of it as each other's reflection on screen too.
    # They are L-shaped, tall away from the plane, because a mirrored box
    # is only a second box. Placed along one isometric axis instead, the
    # two overlapped and the pair read as a clump.
    near, far, w, tall, low = 7.5, 17, 5.5, 10, 4
    P = fit(corners(0, near, 0, w, far, tall) + corners(near, 0, 0, far, w, tall), 2.5, 3, 29.5, 29, snap=0)
    split = near + (far - near) * 0.5
    s = step_block(P, 0, w, near, far, tall, low, split, True, GREY)
    s += step_block(P, near, far, 0, w, tall, low, split, False, BLUES)
    x, _y = P(0, 0, 0)
    s += line([(x, 2.5), (x, 29.5)], stroke=BLUE, w=2, dash='5 1.6 1.2 1.6', cap='butt')
    return s


# ---------------------------------------------------------------- SOLID: MODIFY

@icon('modify.press_pull')
def _():
    P = fit(corners(0, 0, 0, 14, 14, 7) + [(7, 7, 20)], 3, 2, 29, 30)
    s = box(P, 0, 0, 0, 14, 14, 7, (BLUE, MID, DARK))
    x, y = P(7, 7, 7)
    x2, y2 = P(7, 7, 19.5)
    x = x2 = round(x)
    s += arrow(x, y - 1.2, x2, y2, stroke='currentColor', w=2.0, size=5.0, double=True)
    return s


def rounded_block(P, r, chamfer=False):
    X, Y, Z = 14, 12, 10
    n = 1 if chamfer else 10

    def prof(y):
        return [P(X - r + r * math.sin(math.radians(90 * i / n)), y, Z - r + r * math.cos(math.radians(90 * i / n)))
                for i in range(n + 1)]
    front = prof(Y)
    back = prof(0)
    s = poly([P(0, Y, Z)] + front + [P(X, Y, 0), P(0, Y, 0)], MID)
    s += poly([P(X, 0, Z - r), P(X, Y, Z - r), P(X, Y, 0), P(X, 0, 0)], DARK)
    s += poly([P(0, 0, Z), P(X - r, 0, Z), P(X - r, Y, Z), P(0, Y, Z)], LIGHT)
    if chamfer:
        s += poly([P(X - r, 0, Z), P(X - r, Y, Z), P(X, Y, Z - r), P(X, 0, Z - r)], BLUE)
    else:
        s += defs(lin_grad('g', [(0, LBLUE), (0.55, BLUE), (1, DBLUE)], 0, 0, 1, 1))
        s += poly(front + list(reversed(back)), 'url(#g)')
    return s


@icon('modify.fillet')
def _():
    P = fit(corners(0, 0, 0, 14, 12, 10), 3, 3, 29, 29)
    return rounded_block(P, 6.5)


@icon('modify.chamfer')
def _():
    P = fit(corners(0, 0, 0, 14, 12, 10), 3, 3, 29, 29)
    return rounded_block(P, 6.5, chamfer=True)


@icon('modify.shell')
def _():
    A, H, t = 14, 11, 2.4
    P = fit(corners(0, 0, 0, A, A, H), 3, 3, 29, 29)
    s = poly([P(t, t, t), P(A - t, t, t), P(A - t, A - t, t), P(t, A - t, t)], LBLUE)
    s += poly([P(t, t, H), P(t, A - t, H), P(t, A - t, t), P(t, t, t)], DBLUE)
    s += poly([P(t, t, H), P(A - t, t, H), P(A - t, t, t), P(t, t, t)], BLUE)
    s += poly([P(0, A, H), P(A, A, H), P(A, A, 0), P(0, A, 0)], MID)
    s += poly([P(A, 0, H), P(A, A, H), P(A, A, 0), P(A, 0, 0)], DARK)
    outer = [P(0, 0, H), P(A, 0, H), P(A, A, H), P(0, A, H)]
    inner = [P(t, t, H), P(A - t, t, H), P(A - t, A - t, H), P(t, A - t, H)]
    s += path(d_of(outer, True) + ' ' + d_of(inner, True), stroke=INK, w=1.25, fill=LIGHT,
              extra=' fill-rule="evenodd"')
    return s


@icon('modify.combine')
def _():
    P = fit(corners(0, 0, 0, 17, 10, 11), 3, 3, 29, 29)
    s = box(P, 0, 0, 3, 10, 10, 11)
    s += box(P, 7, 2, 0, 17, 10, 7, BLUES)
    return s


def small_cube(P, a=8, shades=GREY):
    return box(P, 0, 0, 0, a, a, a, shades)


@icon('gizmo.move')
def _():
    s = ''
    c = 16
    for (dx, dy) in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        s += arrow(c + dx * 4, c + dy * 4, c + dx * 13.6, c + dy * 13.6, stroke=BLUE, w=2, size=5.0)
    P = Iso(16, 12.2, 0.95)
    s += small_cube(P, 7.8)
    return s


@icon('gizmo.rotate')
def _():
    P = Iso(16, 11.4, 1.0)
    s = ''
    arc = arc_pts(16, 17.5, 13, 7.6, 160, 470, n=40)
    s += path(d_of(arc[:22]), stroke=BLUE, w=2.3, cap='butt')
    s += small_cube(P, 8)
    s += path(d_of(arc[21:-3]), stroke=BLUE, w=2.3, cap='butt')
    x1, y1 = arc[-4]
    x2, y2 = arc[-1]
    s += arrowhead(x1, y1, x2, y2, BLUE, size=5.4, width=5.6)
    return s


@icon('gizmo.scale')
def _():
    P = fit(corners(0, 0, 0, 14, 14, 14), 3, 2.5, 29, 29.5)
    s = ''
    # The scaled result: a larger blue wire box.
    big = [P(0, 0, 14), P(14, 0, 14), P(14, 14, 14), P(0, 14, 14)]
    # Faint enough on white to show the grey cube it grew from, but no
    # fainter: at a third opaque the result went navy on the dark toolbar.
    s += poly([P(0, 14, 14), P(14, 14, 14), P(14, 14, 0), P(0, 14, 0)], LBLUE, stroke=False, extra=' fill-opacity="0.6"')
    s += poly([P(14, 0, 14), P(14, 14, 14), P(14, 14, 0), P(14, 0, 0)], BLUE, stroke=False, extra=' fill-opacity="0.55"')
    s += poly(big, LBLUE, stroke=False, extra=' fill-opacity="0.7"')
    s += box(P, 0, 7, 0, 7, 14, 7)
    edges = [[P(0, 0, 14), P(14, 0, 14), P(14, 14, 14), P(0, 14, 14), P(0, 0, 14)],
             [P(14, 0, 14), P(14, 0, 0), P(14, 14, 0), P(7, 14, 0)],
             [P(14, 14, 14), P(14, 14, 0)], [P(0, 14, 14), P(0, 14, 7)]]
    for e in edges:
        s += line(e, stroke=BLUE, w=1.4)
    x1, y1 = P(7, 7, 7)
    x2, y2 = P(13, 1, 13)
    s += arrow(x1 + 1, y1 - 1, x2, y2, stroke=BLUE, w=1.8, size=4.4)
    return s


@icon('gizmo.moveRotateDialog')
def _():
    s = ''
    for (dx, dy) in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        s += arrow(11 + dx * 3, 11 + dy * 3, 11 + dx * 9.4, 11 + dy * 9.4, stroke=BLUE, w=1.9, size=4.2)
    s += '<rect x="13.5" y="15.5" width="16" height="14" rx="1.5" fill="%s" %s/>' % (LIGHT, outline())
    s += '<rect x="16.5" y="18.5" width="10" height="4" fill="#FFFFFF" stroke="%s" stroke-width="1"/>' % INK
    s += '<rect x="16.5" y="23.5" width="10" height="4" fill="#FFFFFF" stroke="%s" stroke-width="1"/>' % INK
    s += line([(18, 20.5), (22, 20.5)], stroke=BLUE, w=1.3, cap='butt')
    s += line([(18, 25.5), (24, 25.5)], stroke=BLUE, w=1.3, cap='butt')
    return s


@icon('gizmo.press_pull')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 6) + [(8, 8, 21)], 3, 2, 29, 30)
    s = box(P, 0, 0, 0, 16, 16, 6, (LIGHT, MID, DARK))
    x, y = P(8, 8, 6)
    top = P(8, 8, 21)[1]
    shaft = [(x - 1.7, y), (x - 1.7, top + 7), (x - 5.2, top + 7), (x, top), (x + 5.2, top + 7), (x + 1.7, top + 7), (x + 1.7, y)]
    s += poly(shaft, BLUE, w=1.1)
    s += '<ellipse cx="%s" cy="%s" rx="2.6" ry="1.5" fill="%s" %s/>' % (f(x), f(y), BLUE, outline(1.0))
    return s


@icon('modify.change_parameters')
def _():
    s = path('M15.6,6.2 C13.4,4.6 11.2,5.6 10.8,8.6 L8.8,24.6 C8.4,27.6 6.4,28.2 4.4,26.8', w=2.2)
    s += line([(6.6, 13), (14.4, 13)], w=2.0)
    s += path('M19.6,9.6 C16.4,13.2 16.4,21.6 19.6,25.2', w=1.9)
    s += path('M26.6,9.6 C29.8,13.2 29.8,21.6 26.6,25.2', w=1.9)
    s += line([(20.6, 13.8), (25.6, 21.2)], stroke=BLUE, w=2.2)
    s += line([(25.6, 13.8), (20.6, 21.2)], stroke=BLUE, w=2.2)
    return s


# ---------------------------------------------------------------- CONSTRUCT

def plane(P, z, a=14, fill=LIGHT, blue=False, x0=0, y0=0):
    ps = [P(x0, y0, z), P(x0 + a, y0, z), P(x0 + a, y0 + a, z), P(x0, y0 + a, z)]
    if blue:
        return poly(ps, LBLUE, w=1.25, extra=' fill-opacity="0.75" stroke="%s"' % BLUE).replace(
            'stroke="%s" stroke-width' % INK, 'stroke-width', 1)
    return poly(ps, fill)


def blue_plane(ps):
    # Opaque enough to stay light blue on the dark toolbar: at 0.7 the
    # grey showed through and turned it a muddy steel colour.
    return '<polygon points="%s" fill="%s" fill-opacity="0.85" stroke="%s" stroke-width="1.35" stroke-linejoin="round"/>' % (
        pts(ps), LBLUE, BLUE)


@icon('construct.plane_offset')
def _():
    P = fit(corners(0, 0, 0, 15, 15, 10), 2.5, 3, 29.5, 29)
    s = plane(P, 0, 15)
    x, y = P(7.5, 7.5, 0)
    x2, y2 = P(7.5, 7.5, 10)
    s += arrow(x, y, x2, y2 + 1.0, w=1.6, size=4.0, double=True)
    s += blue_plane([P(0, 0, 10), P(15, 0, 10), P(15, 15, 10), P(0, 15, 10)])
    return s


@icon('construct.plane_angle')
def _():
    # The new plane leans back from the far edge of the given one, like the
    # lid of a laptop, so its face turns toward the viewer: hinged on the
    # near side it stood edge-on, a blue sliver.
    a = math.radians(62)
    A, L = 14, 13
    lid = lambda x: P(x, -L * math.cos(a), L * math.sin(a))
    P = fit(corners(0, 0, 0, A, A, 0) + [(0, -L * math.cos(a), L * math.sin(a)), (A, -L * math.cos(a), L * math.sin(a))],
            2.5, 2.5, 29.5, 29.5)
    s = blue_plane([P(0, 0, 0), P(A, 0, 0), lid(A), lid(0)])
    s += poly([P(0, 0, 0), P(A, 0, 0), P(A, A, 0), P(0, A, 0)], LIGHT)
    # The angle between them, at the near end of the hinge.
    r = 6.5
    arc = [P(A, r * math.cos(math.radians(t)), r * math.sin(math.radians(t))) for t in range(0, 181 - 62, 6)]
    arc += [P(A, -r * math.cos(a), r * math.sin(a))]
    s += line(arc, w=1.5)
    return s


@icon('construct.plane_midplane')
def _():
    # A grey block and the plane halfway between its two end faces,
    # reaching past it all round. Three free-standing cards said the same
    # at 32 px and nothing at 16.
    A, D, H, g = 16, 8, 9, 2.6
    M = A / 2
    P = fit(corners(0, -g, -g, A, D + g, H + g), 2.5, 2.5, 29.5, 29.5)
    s = box(P, 0, 0, 0, M, D, H)
    s += blue_plane([P(M, -g, H + g), P(M, D + g, H + g), P(M, D + g, -g), P(M, -g, -g)])
    s += box(P, M, 0, 0, A, D, H)
    return s


@icon('construct.plane_three_points')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 7, 29.5, 26)
    s = blue_plane([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)])
    tri = [P(4, 3, 0), P(13.5, 5, 0), P(5, 13, 0)]
    # Ink rather than currentColor: these sit on the plane, which stays
    # light blue on either palette, so light points would vanish into it.
    s += line(tri + [tri[0]], stroke=INK, w=1.2, dash='2 1.6')
    for x, y in tri:
        s += dot(x, y, r=2.5, fill=INK)
    return s


@icon('construct.axis_two_points')
def _():
    s = line([(4, 28), (28, 4)], stroke=BLUE, w=2.2, dash='7 2 1.6 2')
    s += dot(11, 21, r=2.9, fill='currentColor')
    s += dot(21, 11, r=2.9, fill='currentColor')
    return s


@icon('construct.axis_perpendicular')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 16, 29.5, 29)
    s = poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT)
    x, y = P(8, 8, 0)
    x = round(x)
    s += line([(x, y), (x, 2.5)], stroke=BLUE, w=2)
    foot = P(8, 11, 0)
    s += line([foot, (math.floor(foot[0]) + 0.5, foot[1] - 3.2), (x, y - 3.2)], stroke=INK, w=1.1, cap='butt')
    s += dot(x, y, r=1.8, fill=BLUE)
    return s


@icon('construct.point_coordinates')
def _():
    o = (9, 22)
    s = arrow(o[0], o[1], o[0], 5, w=1.5, size=3.6)
    s += arrow(o[0], o[1], 27, o[1] + 0.01, w=1.5, size=3.6)
    s += arrow(o[0], o[1], 3.2, 28.2, w=1.5, size=3.4)
    p = (21.5, 10.5)
    s += line([(p[0], p[1]), (p[0], o[1])], w=1.1, dash='1.8 1.4')
    s += line([(p[0], p[1]), (o[0], p[1])], w=1.1, dash='1.8 1.4')
    s += dot(p[0], p[1], r=3.0, fill=BLUE)
    return s


@icon('construct.point_axis_plane')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 13, 29.5, 27)
    x, y = P(8, 8, 0)
    s = line([(x + 4.2, y + 9.2), (x, y)], stroke=BLUE, w=2.0, dash='1.8 1.6')
    s += poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT)
    s += line([(x, y), (x - 8.5, 2.5)], stroke=BLUE, w=2.2)
    s += dot(x, y, r=3.1, fill=INK)
    s += dot(x, y, r=1.6, fill=BLUE)
    return s


# ---------------------------------------------------------------- SELECT

# The selection tools draw a stepped part rather than a cube: the cube
# already means "a view" on the View tab and "a new box" on Create, and a
# pick filter should not look like either.
PART = (14, 7, 6, 12)   # length, depth of the upper block, step height, height


def select_part(P, what):
    A, B, h, H = PART
    ink = BLUE if what == 'body' else INK
    w = 1.6 if what == 'body' else 1.25
    right = [P(A, 0, H), P(A, B, H), P(A, B, h), P(A, A, h), P(A, A, 0), P(A, 0, 0)]
    front = [P(0, A, h), P(A, A, h), P(A, A, 0), P(0, A, 0)]
    step = [P(0, B, h), P(A, B, h), P(A, A, h), P(0, A, h)]
    riser = [P(0, B, H), P(A, B, H), P(A, B, h), P(0, B, h)]
    top = [P(0, 0, H), P(A, 0, H), P(A, B, H), P(0, B, H)]
    s = poly(right, DARK, w=w, ink=ink) + poly(front, MID, w=w, ink=ink)
    s += poly(step, BLUE if what == 'face' else LIGHT, w=w, ink=ink)
    s += poly(riser, MID, w=w, ink=ink) + poly(top, LIGHT, w=w, ink=ink)
    if what == 'edge':
        s += line([P(0, B, H), P(A, B, H)], stroke=BLUE, w=3.2)
        s += line([P(0, B, H), P(A, B, H)], stroke=LBLUE, w=1.0)
    elif what == 'vertex':
        x, y = P(A, B, H)
        s += dot(x, y, r=3.4, fill=BLUE, ring='#FFFFFF')
    return s


def part_fit(x0, y0, x1, y1, what=None):
    A, B, h, H = PART
    # Body's heavier blue outline wants whole columns, the ink one halves.
    return fit(corners(0, 0, 0, A, A, H), x0, y0, x1, y1, snap=0 if what == 'body' else 0.5)


@icon('select.bodies')
def _():
    return select_part(part_fit(3, 3, 29, 29, 'body'), 'body')


@icon('select.faces')
def _():
    return select_part(part_fit(3, 3, 29, 29), 'face')


@icon('select.edges')
def _():
    return select_part(part_fit(3, 3, 29, 29), 'edge')


@icon('select.vertices')
def _():
    return select_part(part_fit(3, 3, 29, 29), 'vertex')


def priority(what):
    return select_part(part_fit(2, 2.5, 25, 25, what), what) + pointer(19.5, 16.2, 1.02)


@icon('select.priority.body')
def _():
    return priority('body')


@icon('select.priority.face')
def _():
    return priority('face')


@icon('select.priority.edge')
def _():
    return priority('edge')


# ---------------------------------------------------------------- SKETCH

@icon('sketch.finish')
def _():
    # The sketch sheet of Create Sketch, done: its rectangle in ink and
    # Fusion's green tick over it.
    on = sheet(2.5, 29, 17, 12.5, 5)
    s = poly([on(0, 0), on(1, 0), on(1, 1), on(0, 1)], LIGHT)
    near, far = 3 / 12.5, 10 / 12.5     # rows y = 26 and 19
    s += path(d_of([on(0.18, near), on(0.7, near), on(0.7, far), on(0.18, far)], True), stroke=INK, w=1.5)
    tick = [(10.5, 16.5), (16.5, 23), (27.5, 6.5)]
    s += line(tick, stroke=INK, w=6.6)
    s += line(tick, stroke=GREEN, w=4.2)
    return s


def eye(cx, cy, w=12.5, h=7.2):
    d = 'M%s,%s Q%s,%s %s,%s Q%s,%s %s,%s Z' % (f(cx - w), f(cy), f(cx), f(cy - h * 2 + 2), f(cx + w), f(cy),
                                               f(cx), f(cy + h * 2 - 2), f(cx - w), f(cy))
    s = path(d, w=1.75, fill='none')
    s += circle(cx, cy, 4.1, stroke='currentColor', w=1.6, fill=BLUE)
    s += dot(cx - 1.3, cy - 1.3, r=1.1, fill='#FFFFFF')
    return s


@icon('sketch.constraints.visible')
def _():
    return eye(14, 12) + constraint_badge(18.5, 18.5)


@icon('sketch.visible')
def _():
    s = eye(14, 12)
    s += path('M19,29 L19,21 L28,21 L28,29 Z', stroke='currentColor', w=1.5)
    s += dot(19, 29, r=1.9) + dot(28, 21, r=1.9)
    return s


@icon('sketch.line')
def _():
    return line([(6, 26), (26, 6)]) + dot(6, 26, 2.6) + dot(26, 6, 2.6)


@icon('sketch.rectangle')
def _():
    s = path('M5,9 L27,9 L27,23 L5,23 Z')
    return s + dot(5, 9, 2.6) + dot(27, 23, 2.6)


@icon('sketch.rectangle.centre')
def _():
    s = path('M5,9 L27,9 L27,23 L5,23 Z')
    s += line([(16, 16), (27, 23)], w=1.2, dash='1.8 1.5')
    return s + dot(16, 16, 2.6) + dot(27, 23, 2.6)


@icon('sketch.rectangle.three')
def _():
    a = math.radians(-24)
    ux, uy = math.cos(a), math.sin(a)
    nx, ny = -uy, ux
    o = (4.2, 20.5)
    L, W = 20, 11
    p0 = o
    p1 = (o[0] + ux * L, o[1] + uy * L)
    p2 = (p1[0] - nx * W, p1[1] - ny * W)
    p3 = (o[0] - nx * W, o[1] - ny * W)
    # Rotated the other way up so it sits in the square.
    ps = [(x, y + 6) for x, y in (p0, p1, (p1[0] + nx * W, p1[1] + ny * W - 12), (o[0] + nx * W, o[1] + ny * W - 12))]
    corner = [(4.5, 17.5), (21.5, 8.5), (27.5, 19.5), (10.5, 28.5)]
    s = path(d_of(corner, True))
    return s + dot(*corner[0], 2.6) + dot(*corner[1], 2.6) + dot(*corner[2], 2.6)


@icon('sketch.circle')
def _():
    s = circle(16, 16, 11)
    s += line([(16, 16), (16 + 11 * 0.7071, 16 - 11 * 0.7071)], w=1.2, dash='1.8 1.5')
    return s + dot(16, 16, 2.6) + dot(16 + 11 * 0.7071, 16 - 11 * 0.7071, 2.6)


@icon('sketch.circle.two')
def _():
    s = circle(16, 16, 11)
    a = 11 * 0.7071
    s += line([(16 - a, 16 + a), (16 + a, 16 - a)], w=1.2, dash='1.8 1.5')
    return s + dot(16 - a, 16 + a, 2.6) + dot(16 + a, 16 - a, 2.6)


@icon('sketch.circle.three')
def _():
    s = circle(16, 16, 11)
    out = ''
    for t in (200, 320, 85):
        out += dot(16 + 11 * math.cos(math.radians(t)), 16 - 11 * math.sin(math.radians(t)), 2.6)
    return s + out


def arc_path(cx, cy, r, a0, a1):
    """Arc from angle a0 to a1 (degrees, counter-clockwise, y up)."""
    x0, y0 = cx + r * math.cos(math.radians(a0)), cy - r * math.sin(math.radians(a0))
    x1, y1 = cx + r * math.cos(math.radians(a1)), cy - r * math.sin(math.radians(a1))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return 'M%s,%s A%s,%s 0 %d 0 %s,%s' % (f(x0), f(y0), f(r), f(r), large, f(x1), f(y1)), (x0, y0), (x1, y1)


@icon('sketch.arc')
def _():
    d, p0, p1 = arc_path(16, 22, 12, 15, 165)
    s = path(d)
    s += line([p0, (16, 22), p1], w=1.2, dash='1.8 1.5')
    return s + dot(16, 22, 2.6) + dot(*p0, r=2.6) + dot(*p1, r=2.6)


@icon('sketch.arc.three')
def _():
    d, p0, p1 = arc_path(16, 24, 13, 20, 160)
    s = path(d)
    return s + dot(*p0, r=2.6) + dot(*p1, r=2.6) + dot(16, 11, 2.6)


@icon('sketch.arc.tangent')
def _():
    s = line([(3.5, 25), (14, 25)])
    s += path('M14,25 A8,8 0 0 0 14,9', w=1.75)
    return s + dot(14, 25, 2.6) + dot(14, 9, 2.6) + dot(3.5, 25, 2.0, fill='currentColor')


def polygon_pts(cx, cy, r, n=6, rot=0):
    return [(cx + r * math.cos(math.radians(rot + 360 * i / n)), cy + r * math.sin(math.radians(rot + 360 * i / n)))
            for i in range(n)]


@icon('sketch.polygon.circumscribed')
def _():
    r = 9      # the flat sides land on x = 7 and 25
    R = r / math.cos(math.radians(30))
    hexa = polygon_pts(16, 16, R, 6, 30)
    # The reference circle is a tinted disc: as a thin ring it hid under
    # the very sides it touches, and the icon read as a bare hexagon.
    s = circle(16, 16, r, stroke=None, fill=LBLUE).replace('/>', ' fill-opacity="0.55"/>')
    s += path(d_of(hexa, True))
    return s + dot(16, 16, 2.4) + dot(16 + r, 16, 2.6)


@icon('sketch.polygon.inscribed')
def _():
    R = 11 / math.sin(math.radians(60))    # flat sides on y = 5 and 27
    hexa = polygon_pts(16, 16, R, 6, 0)
    s = circle(16, 16, R, stroke=None, fill=LBLUE).replace('/>', ' fill-opacity="0.55"/>')
    s += path(d_of(hexa, True))
    return s + dot(16, 16, 2.4) + dot(16 + R, 16, 2.6)


@icon('sketch.polygon.edge')
def _():
    R = 9 / math.cos(math.radians(30))     # flat sides on x = 7 and 25
    hexa = polygon_pts(16, 15, R, 6, 30)
    s = path(d_of(hexa, True))
    a, b = hexa[1], hexa[2]
    s += line([a, b], stroke=BLUE, w=2.4)
    return s + dot(*a, r=2.6) + dot(*b, r=2.6)


@icon('sketch.ellipse')
def _():
    # Centred on a half coordinate, so its thin dashed axes land on pixels.
    c, rx, ry = 15.5, 13, 8
    s = ellipse(c, c, rx, ry, stroke='currentColor', w=1.75)
    s += line([(c - rx, c), (c + rx, c)], w=1.1, dash='1.8 1.5')
    s += line([(c, c - ry), (c, c + ry)], w=1.1, dash='1.8 1.5')
    return s + dot(c, c, 2.4) + dot(c + rx, c, 2.4) + dot(c, c - ry, 2.4)


@icon('sketch.slot')
def _():
    # Straight sides on whole rows, so the dashed centre line between them
    # falls on a half one: both then land on pixels.
    s = path('M9,9 L23,9 A6.5,6.5 0 0 1 23,22 L9,22 A6.5,6.5 0 0 1 9,9 Z')
    s += line([(9, 15.5), (23, 15.5)], w=1.2, dash='1.8 1.5')
    return s + dot(9, 15.5, 2.4) + dot(23, 15.5, 2.4)


def through(ps):
    """Smooth curve through points (Catmull-Rom as cubic Beziers)."""
    d = 'M%s,%s' % (f(ps[0][0]), f(ps[0][1]))
    for i in range(len(ps) - 1):
        p0 = ps[i - 1] if i > 0 else ps[i]
        p1, p2 = ps[i], ps[i + 1]
        p3 = ps[i + 2] if i + 2 < len(ps) else p2
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        d += ' C%s,%s %s,%s %s,%s' % (f(c1[0]), f(c1[1]), f(c2[0]), f(c2[1]), f(p2[0]), f(p2[1]))
    return d


@icon('sketch.spline')
def _():
    s = path('M4,24 C7,8 13,6 16,16 S25,26 28,8')
    # Fit points ON the curve: its ends, its joint and each segment's middle.
    ps = [(4, 24), (10, 10.25), (16, 16), (22, 22.5), (28, 8)]
    return s + ''.join(dot(x, y, 2.4) for x, y in ps)


@icon('sketch.spline.control_point')
def _():
    cps = [(4, 26), (9, 5), (23, 27), (28, 6)]
    s = line(cps, w=1.1, dash='1.8 1.5')
    s += path('M4,26 C9,5 23,27 28,6')
    return s + ''.join(square_pt(x, y, 4.0) for x, y in cps)


@icon('sketch.conic')
def _():
    a, b, v = (4, 26), (28, 26), (16, 4)
    s = line([a, v, b], w=1.1, dash='1.8 1.5')
    s += path('M4,26 C10,13 22,13 28,26')
    return s + dot(*a, r=2.4) + dot(*b, r=2.4) + dot(16, 16.3, 2.4) + dot(*v, r=1.8, fill='currentColor')


@icon('sketch.point')
def _():
    s = line([(16, 4), (16, 11)], w=1.5) + line([(16, 21), (16, 28)], w=1.5)
    s += line([(4, 16), (11, 16)], w=1.5) + line([(21, 16), (28, 16)], w=1.5)
    return s + dot(16, 16, 3.6)


@icon('sketch.fillet')
def _():
    s = line([(7, 7), (7, 16)], w=1.5, dash='1.8 1.5') + line([(7, 7), (16, 7)], w=1.5, dash='1.8 1.5')
    s += line([(7, 28), (7, 16)])
    s += line([(16, 7), (28, 7)])
    s += path('M7,16 A9,9 0 0 1 16,7', stroke=BLUE, w=2.3)
    return s + dot(7, 16, 2.3) + dot(16, 7, 2.3)


@icon('sketch.trim')
def _():
    s = line([(3, 9), (10, 9)]) + line([(22, 9), (29, 9)])
    s += line([(10, 9), (22, 9)], stroke=BLUE, w=1.75, dash='2 1.6')
    s += line([(10.5, 3.5), (10.5, 14.5)], w=1.4) + line([(21.5, 3.5), (21.5, 14.5)], w=1.4)
    # Scissors, blades up.
    s += line([(12.4, 21.5), (19.8, 11.6)], w=1.8)
    s += line([(19.6, 21.5), (12.2, 11.6)], w=1.8)
    s += circle(11.2, 25.2, 3.1, w=1.7) + circle(20.8, 25.2, 3.1, w=1.7)
    s += dot(16, 16.6, 1.1, fill='currentColor')
    return s


@icon('sketch.extend')
def _():
    s = line([(27, 4), (27, 28)], w=1.75)
    s += line([(4, 19), (13, 19)])
    s += line([(13, 19), (20.5, 19)], stroke=BLUE, w=1.75, dash='2 1.6', cap='butt')
    s += arrowhead(13, 19, 26, 19, BLUE, size=5, width=5)
    return s + dot(4, 19, 2.0, fill='currentColor')


@icon('sketch.offset')
def _():
    s = '<rect x="10" y="10" width="12" height="12" rx="3" fill="none" stroke="currentColor" stroke-width="1.75"/>'
    s += '<rect x="4" y="4" width="24" height="24" rx="7" fill="none" stroke="%s" stroke-width="1.9"/>' % BLUE
    return s


@icon('sketch.mirror')
def _():
    s = line([(16, 3), (16, 29)], w=1.5, dash='5 1.6 1.2 1.6')
    s += path('M13,8 L13,25 L3.5,25 Z', w=1.75)
    s += path('M19,8 L19,25 L28.5,25 Z', stroke=BLUE, w=1.9)
    return s


@icon('sketch.pattern.rectangular')
def _():
    s = ''
    for i in range(3):
        for j in range(3):
            x, y = 3 + i * 10, 3 + j * 10     # every side on an odd column or row
            col = 'currentColor' if i == 0 and j == 0 else BLUE
            s += '<rect x="%s" y="%s" width="6" height="6" fill="none" stroke="%s" stroke-width="1.6"/>' % (
                f(x), f(y), col)
    return s


@icon('sketch.pattern.circular')
def _():
    s = ''
    for i in range(6):
        t = math.radians(-90 + i * 60)
        col = 'currentColor' if i == 0 else BLUE
        s += circle(16 + 10.4 * math.cos(t), 16 + 10.4 * math.sin(t), 3.3, stroke=col, w=1.7, fill='none')
    return s + dot(16, 16, 2.3)


@icon('sketch.construction')
def _():
    s = line([(5, 27), (27, 5)], stroke=ORANGE, w=2.0, dash='3.4 2.4', cap='butt')
    return s + dot(5, 27, 2.6) + dot(27, 5, 2.6)


# ---------------------------------------------------------------- CONSTRAINTS

@icon('sketch.constraint.coincident')
def _():
    s = line([(4, 26), (16, 12)]) + line([(16, 12), (28, 20)])
    return s + dot(16, 12, 3.5)


@icon('sketch.constraint.horizontal')
def _():
    s = line([(5, 24), (27, 20)], w=1.2, dash='1.8 1.5')
    s += line([(5, 11), (27, 11)], stroke=BLUE, w=2)
    return s + dot(5, 11, 2.4, fill='currentColor') + dot(27, 11, 2.4, fill='currentColor')


@icon('sketch.constraint.vertical')
def _():
    s = line([(24, 5), (20, 27)], w=1.2, dash='1.8 1.5')
    s += line([(11, 5), (11, 27)], stroke=BLUE, w=2)
    return s + dot(11, 5, 2.4, fill='currentColor') + dot(11, 27, 2.4, fill='currentColor')


@icon('sketch.constraint.parallel')
def _():
    return line([(4, 20), (18, 5)]) + line([(14, 27), (28, 12)], stroke=BLUE, w=2.2)


@icon('sketch.constraint.perpendicular')
def _():
    s = line([(5, 27), (27, 27)])
    s += line([(15, 27), (15, 5)], stroke=BLUE, w=2)
    s += line([(15, 21.5), (20.5, 21.5), (20.5, 27)], w=1.2, cap='butt')
    return s


@icon('sketch.constraint.equal')
def _():
    s = line([(7, 5), (7, 27)]) + line([(25, 5), (25, 27)], stroke=BLUE, w=2)
    s += line([(11.5, 13), (20.5, 13)], w=2.0) + line([(11.5, 19), (20.5, 19)], w=2.0)
    return s


@icon('sketch.constraint.tangent')
def _():
    s = circle(15, 20, 9)
    s += line([(3, 11), (29, 11)], stroke=BLUE, w=2)
    return s + dot(15, 11, 2.6, fill='currentColor')


@icon('sketch.constraint.midpoint')
def _():
    # Level, with a big triangle: drawn on a slant with a small one, it was
    # Collinear's twin in the 16 px flyout.
    s = line([(4, 25), (28, 25)])
    s += poly([(16, 12.5), (22.5, 23), (9.5, 23)], BLUE, stroke=False)
    return s + dot(4, 25, 2.2, fill='currentColor') + dot(28, 25, 2.2, fill='currentColor')


@icon('sketch.constraint.concentric')
def _():
    return circle(16, 16, 12) + circle(16, 16, 6.4, stroke=BLUE, w=2.1) + dot(16, 16, 2.2, fill='currentColor')


@icon('sketch.constraint.collinear')
def _():
    # Two segments on one line with a clear gap between them; a dotted
    # bridge filled the gap in at 16 px.
    s = line([(3.5, 25.5), (13, 17.5)]) + line([(19, 12.5), (28.5, 4.5)], stroke=BLUE, w=2.2)
    s += dot(3.5, 25.5, 2.2, fill='currentColor') + dot(13, 17.5, 2.2, fill='currentColor')
    return s + dot(19, 12.5, 2.4) + dot(28.5, 4.5, 2.4)


@icon('sketch.constraint.fix')
def _():
    s = path('M11,15 L11,10.5 A5,5 0 0 1 21,10.5 L21,15', w=2.2)
    s += '<rect x="7.5" y="14.5" width="17" height="13" rx="2" fill="%s" stroke="%s" stroke-width="1.25"/>' % (BLUE, INK)
    s += dot(16, 20, 1.9, fill='#FFFFFF') + line([(16, 20.5), (16, 24)], stroke='#FFFFFF', w=1.8)
    return s


@icon('sketch.constraint.symmetry')
def _():
    s = line([(16, 3), (16, 29)], w=1.5, dash='5 1.6 1.2 1.6')
    s += line([(4, 24), (11, 8)]) + line([(28, 24), (21, 8)], stroke=BLUE, w=2.2)
    return s + dot(4, 24, 2.2, fill='currentColor') + dot(28, 24, 2.4)


# ---------------------------------------------------------------- SKETCH INSPECT

@icon('sketch.dimension')
def _():
    # A sketch line, and above it the dimension Fusion puts on it: extension
    # lines, a dimension line with both arrows in the open, and the value's
    # box. The box used to sit ON the dimension line and hid its arrows.
    s = line([(5, 27), (27, 27)])
    s += line([(5.5, 24.5), (5.5, 12.5)], w=1.2) + line([(26.5, 24.5), (26.5, 12.5)], w=1.2)
    s += arrow(6.1, 17, 25.9, 17, w=1.5, size=4.4, double=True)
    s += '<rect x="10" y="3" width="12" height="8" rx="1.6" fill="%s"/>' % BLUE
    s += line([(13, 7), (19, 7)], stroke='#FFFFFF', w=2, cap='butt')
    return s + dot(5, 27, 2.3) + dot(27, 27, 2.3)


def constraint_badge(x, y, a=11):
    """The small square a sketch shows beside a constrained curve, here
    carrying Perpendicular's mark."""
    s = '<rect x="%s" y="%s" width="%s" height="%s" rx="1.6" fill="%s" stroke="%s" stroke-width="1.1"/>' % (
        f(x), f(y), f(a), f(a), LIGHT, INK)
    base = y + a - 2.5
    s += line([(x + 2.5, base), (x + a - 2.5, base)], stroke=INK, w=1.5, cap='butt')
    return s + line([(x + a / 2, base), (x + a / 2, y + 2.5)], stroke=INK, w=1.5, cap='butt')


@icon('sketch.dimension.clear')
def _():
    # Every constraint and every dimension goes: one of each, and the red
    # mark Fusion puts on a delete.
    s = line([(4.5, 13), (4.5, 4)], w=1.2) + line([(21.5, 13), (21.5, 4)], w=1.2)
    s += arrow(13, 8.5, 4.9, 8.5, w=1.4, size=4) + arrow(13, 8.5, 21.1, 8.5, w=1.4, size=4)
    s += constraint_badge(3.5, 16.5)
    s += circle(23, 22.5, 6.2, stroke=None, fill=RED)
    s += line([(20.3, 19.8), (25.7, 25.2)], stroke='#FFFFFF', w=1.8)
    s += line([(25.7, 19.8), (20.3, 25.2)], stroke='#FFFFFF', w=1.8)
    return s


# ---------------------------------------------------------------- VIEW

def view_cube(face):
    a = 12
    hidden = face in ('back', 'left', 'bottom')
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29, snap=0 if hidden else 0.5)
    s = ''
    faces = {
        'top': [P(0, 0, a), P(a, 0, a), P(a, a, a), P(0, a, a)],
        'front': [P(0, a, a), P(a, a, a), P(a, a, 0), P(0, a, 0)],
        'right': [P(a, 0, a), P(a, a, a), P(a, a, 0), P(a, 0, 0)],
        'bottom': [P(0, 0, 0), P(a, 0, 0), P(a, a, 0), P(0, a, 0)],
        'back': [P(0, 0, a), P(a, 0, a), P(a, 0, 0), P(0, 0, 0)],
        'left': [P(0, 0, a), P(0, a, a), P(0, a, 0), P(0, 0, 0)],
    }
    if hidden:
        # A face the eye cannot reach is drawn through a glass cube: solid
        # near faces with blue showing through them read, at 24 px, as a
        # cube painted half blue -- Back and Left looked like Right.
        s += poly(faces[face], BLUE, w=1.25, ink=DBLUE)
        for e in ([P(0, 0, 0), P(a, 0, 0)], [P(0, 0, 0), P(0, a, 0)], [P(0, 0, 0), P(0, 0, a)]):
            s += line(e, w=1.1, dash='1.6 1.3')
        for k in ('front', 'right', 'top'):
            s += poly(faces[k], LIGHT, stroke=False, extra=' fill-opacity="0.28"')
        s += wire_cube(P, a, hidden=False)
    else:
        shade = {'top': LIGHT, 'front': MID, 'right': DARK}
        for k in ('front', 'right', 'top'):
            s += poly(faces[k], BLUE if k == face else shade[k])
    return s


for _face in ('front', 'back', 'left', 'right', 'top', 'bottom'):
    ICONS['view.' + _face] = (lambda fc: (lambda: view_cube(fc)))(_face)


@icon('view.isometric')
def _():
    s = path('M4,15.5 L16,4.5 L28,15.5', stroke='currentColor', w=2.0)
    s += poly([(7.5, 14), (16, 6.5), (24.5, 14), (24.5, 27.5), (7.5, 27.5)], LIGHT)
    s += poly([(13, 27.5), (13, 19.5), (19, 19.5), (19, 27.5)], BLUE, w=1.1)
    return s


@icon('view.fit_all')
def _():
    s = ''
    for (x, y, dx, dy) in ((3, 3, 1, 1), (29, 3, -1, 1), (3, 29, 1, -1), (29, 29, -1, -1)):
        s += line([(x, y + dy * 7), (x, y), (x + dx * 7, y)], w=2.0, cap='round')
    P = fit(corners(0, 0, 0, 9, 9, 9), 9.5, 8.5, 22.5, 23.5)
    s += box(P, 0, 0, 0, 9, 9, 9, BLUES)
    return s


def wire_cube(P, a, hidden=True):
    s = ''
    if hidden:
        for e in ([P(0, 0, 0), P(a, 0, 0)], [P(0, 0, 0), P(0, a, 0)], [P(0, 0, 0), P(0, 0, a)]):
            s += line(e, w=1.1, dash='1.6 1.3')
    vis = [[P(0, 0, a), P(a, 0, a), P(a, a, a), P(0, a, a), P(0, 0, a)],
           [P(a, 0, a), P(a, 0, 0), P(a, a, 0), P(0, a, 0), P(0, a, a)],
           [P(a, a, a), P(a, a, 0)]]
    for e in vis:
        s += line(e, w=1.6)
    return s


@icon('view.display_shaded')
def _():
    a = 12
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29)
    faces = [([P(0, a, a), P(a, a, a), P(a, a, 0), P(0, a, 0)], BLUE),
             ([P(a, 0, a), P(a, a, a), P(a, a, 0), P(a, 0, 0)], DBLUE),
             ([P(0, 0, a), P(a, 0, a), P(a, a, a), P(0, a, a)], LBLUE)]
    return ''.join(poly(ps, c, stroke=False) for ps, c in faces)


@icon('view.display_shaded_edges')
def _():
    a = 12
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29, snap=0)
    return box(P, 0, 0, 0, a, a, a, BLUES, w=1.7, ink='currentColor')


@icon('view.display_wireframe')
def _():
    a = 12
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29, snap=0)
    return wire_cube(P, a)


@icon('view.grid_toggle')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 6, 29.5, 26)
    s = poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT, stroke=False, extra=' fill-opacity="0.35"')
    for i in range(0, 17, 4):
        s += line([P(i, 0, 0), P(i, 16, 0)], w=1.1 if 0 < i < 16 else 1.6)
        s += line([P(0, i, 0), P(16, i, 0)], w=1.1 if 0 < i < 16 else 1.6)
    s += line([P(8, 0, 0), P(8, 16, 0)], stroke=BLUE, w=1.5) + line([P(0, 8, 0), P(16, 8, 0)], stroke=BLUE, w=1.5)
    return s


@icon('view.projection_toggle')
def _():
    # A box drawn in perspective: the far face smaller than the near one.
    near = [(3, 11), (20, 11), (20, 28), (3, 28)]
    far = [(19, 4.5), (26.5, 4.5), (26.5, 12), (19, 12)]
    s = poly([near[0], far[0], far[1], near[1]], LBLUE)
    s += poly([near[1], far[1], far[2], near[2]], DBLUE)
    s += poly(near, BLUE)
    return s


@icon('view.view_cube')
def _():
    a = 10
    P = fit(corners(-4, -4, 0, a + 4, a + 4, a), 2.5, 3, 29.5, 29.5)
    ex, ey, rx, ry = iso_ellipse(P, a / 2, a / 2, 0, a * 0.95)
    s = ellipse(ex, ey, rx, ry, stroke='currentColor', w=1.6)
    s += box(P, 0, 0, 0, a, a, a)
    s += path('M%s,%s A%s,%s 0 0 0 %s,%s' % (f(ex - rx), f(ey), f(rx), f(ry), f(ex + rx), f(ey)), stroke='currentColor', w=1.6)
    x, y = P(a, a, a)
    s += poly([P(0, a, a), P(a, a, a), P(a, a, 0), P(0, a, 0)], BLUE)
    s += arrowhead(ex - 4, ey + ry, ex, ey + ry, BLUE, size=4, width=4)
    return s


@icon('view.units')
def _():
    s = poly([(3, 20), (20, 3), (29, 12), (12, 29)], LIGHT)
    for i in range(1, 8):
        t = i / 8
        x, y = 3 + 17 * t, 20 - 17 * t
        L = 4.6 if i % 2 == 0 else 2.8
        s += line([(x, y), (x + L * 0.7071, y + L * 0.7071)], stroke=INK, w=1.1, cap='butt')
    return s


# ---------------------------------------------------------------- INSPECT

@icon('inspect.measure_distance')
def _():
    s = poly([(3, 23), (29, 23), (29, 29), (3, 29)], LIGHT, w=1.1)
    for i in range(1, 9):
        x = math.floor(3 + i * 26 / 9) + 0.5
        s += line([(x, 23), (x, 25.2 if i % 3 else 26.8)], stroke=INK, w=1.0, cap='butt')
    s += line([(5.5, 5), (5.5, 18)], w=1.2) + line([(26.5, 5), (26.5, 18)], w=1.2)
    s += arrow(5.7, 11, 26.3, 11, stroke=BLUE, w=2.0, size=4.6, double=True)
    return s


@icon('inspect.model_properties')
def _():
    s = '<rect x="5" y="3.5" width="19" height="25" rx="1.8" fill="%s" %s/>' % (LIGHT, outline())
    for y in (15.5, 19.5, 23.5):
        s += line([(8.5, y), (13, y)], stroke=INK, w=1.3, cap='butt')
        s += line([(15, y), (20.5, y)], stroke=BLUE, w=1.3, cap='butt')
    P = fit(corners(0, 0, 0, 7, 7, 7), 9, 5.5, 20, 13.5)
    s += box(P, 0, 0, 0, 7, 7, 7, BLUES, w=1.0)
    return s


def hatch(P, rects, y, step=2.6):
    """45-degree hatching over rectangles (x0, z0, x1, z1) of the plane at
    y, clipped by hand: the SVG image plugin has no clip paths."""
    out = []
    for x0, z0, x1, z1 in rects:
        c = x0 - (z1 - z0)
        while c < x1:
            # The line x - z = c - z0 inside the rectangle.
            a = (max(x0, c), z0 + max(x0, c) - c)
            b = (min(x1, c + (z1 - z0)), z0 + min(x1, c + (z1 - z0)) - c)
            if b[0] - a[0] > 0.4:
                out.append([P(a[0], y, a[1]), P(b[0], y, b[1])])
            c += step
    return out


@icon('inspect.section_view')
def _():
    # A cup cut down the middle: the U of its walls and floor is the cut
    # face, blue and hatched the way a drawing marks a section, with the
    # inside of the cup in view behind it. A plain cut block said only
    # "blue square".
    A, D, H, t, m = 15, 8, 11, 4, 2.2
    P = fit(corners(-m, 0, -m, A + m, D, H + m), 2.5, 2.5, 29.5, 29.5)
    s = poly([P(t, t, t), P(A - t, t, t), P(A - t, t, H), P(t, t, H)], MID)
    s += poly([P(t, t, H), P(t, D, H), P(t, D, t), P(t, t, t)], DARK)
    s += poly([P(t, t, t), P(A - t, t, t), P(A - t, D, t), P(t, D, t)], LIGHT)
    rim = [(0, 0), (A, 0), (A, D), (A - t, D), (A - t, t), (t, t), (t, D), (0, D)]
    s += poly([P(x, y, H) for x, y in rim], LIGHT)
    s += poly([P(A, 0, H), P(A, D, H), P(A, D, 0), P(A, 0, 0)], DARK)
    cut = [(0, H), (t, H), (t, t), (A - t, t), (A - t, H), (A, H), (A, 0), (0, 0)]
    s += poly([P(x, D, z) for x, z in cut], BLUE)
    for seg in hatch(P, [(0, 0, t, H), (t, 0, A - t, t), (A - t, 0, A, H)], D, step=3.4):
        s += line(seg, stroke=LBLUE, w=1.0, cap='butt')
    s += poly([P(x, D, z) for x, z in cut], 'none', w=1.25)
    # The section plane is only its frame: a tinted pane over the cut
    # greyed the very face the icon is about.
    s += ('<polygon points="%s" fill="none" stroke="%s" stroke-width="1.4" stroke-linejoin="round"/>' % (
        pts([P(-m, D, H + m), P(A + m, D, H + m), P(A + m, D, -m), P(-m, D, -m)]), BLUE))
    return s


# ---------------------------------------------------------------- output

def render(name):
    body = ICONS[name]()
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32" '
            'color="%s">\n%s\n</svg>\n' % (INK, body.replace('/><', '/>\n<')))


# ---------------------------------------------------------------- audit

def straight_strokes(svg):
    """Every horizontal or vertical stroked segment in an SVG as
    (axis, position, width, element), axis 'x' for a vertical line at
    x = position. Curves are skipped; their ends still move the pen."""
    import re
    out = []
    for tag in re.findall(r'<(?:path|polygon|rect|line)\b[^>]*>', svg):
        attr = dict(re.findall(r'([\w-]+)="([^"]*)"', tag))
        if attr.get('stroke', 'none') == 'none' or 'stroke-width' not in attr:
            continue
        w = float(attr['stroke-width'])
        segs = []
        if tag.startswith('<rect'):
            x, y = float(attr['x']), float(attr['y'])
            x1, y1 = x + float(attr['width']), y + float(attr['height'])
            if float(attr.get('rx', 0)) == 0:
                segs = [((x, y), (x1, y)), ((x1, y), (x1, y1)), ((x1, y1), (x, y1)), ((x, y1), (x, y))]
        elif tag.startswith('<polygon'):
            p = [tuple(map(float, q.split(','))) for q in attr['points'].split()]
            segs = list(zip(p, p[1:] + p[:1]))
        elif tag.startswith('<path'):
            toks = re.findall(r'[MLHVZACQSmlhvzacqs]|-?[\d.]+', attr['d'])
            pen = start = (0.0, 0.0)
            i, cmd = 0, 'M'
            while i < len(toks):
                if toks[i].isalpha():
                    cmd = toks[i]
                    i += 1
                    if cmd in 'Zz':
                        segs.append((pen, start))
                        pen = start
                    continue
                n = {'M': 2, 'L': 2, 'H': 1, 'V': 1, 'A': 7, 'C': 6, 'Q': 4, 'S': 4}[cmd.upper()]
                v = [float(t) for t in toks[i:i + n]]
                i += n
                if cmd == 'M':
                    pen = start = (v[0], v[1])
                    cmd = 'L'
                    continue
                nxt = {'L': lambda: (v[0], v[1]), 'H': lambda: (v[0], pen[1]),
                       'V': lambda: (pen[0], v[0])}.get(cmd, lambda: (v[-2], v[-1]))()
                if cmd in 'LHV':
                    segs.append((pen, nxt))
                pen = nxt
        for (x0, y0), (x1, y1) in segs:
            if abs(x0 - x1) < 0.01 and abs(y0 - y1) >= 3:
                out.append(('x', x0, w, tag))
            elif abs(y0 - y1) < 0.01 and abs(x0 - x1) >= 3:
                out.append(('y', y0, w, tag))
    return out


def audit():
    """Straight lines that fall between pixels, where they render as two
    half-grey rows instead of one dark one. At 32 px -- the ribbon, and the
    menus on a 2x screen -- a stroke of 1.5 or more has to sit on a whole
    coordinate and a thinner one on a half. An ODD whole coordinate is
    also whole at 16 px, the menus on a 1x screen; an even one blurs there,
    which is only noted, since a line through the centre of a symmetric
    icon has no odd place to go. The sloping edges of the isometric solids
    are not checked: nothing can put those on the grid."""
    bad = notes = 0
    for name in sorted(ICONS):
        for axis, pos, w, tag in straight_strokes(render(name)):
            if w < 0.8 or 'stroke-dasharray' in tag or (w <= 1.4 and (
                    tag.startswith('<polygon') or 'fill="none"' not in tag)):
                continue   # an arrowhead's rim, a dashed line, or a filled face's outline
            where = 'vertical at x' if axis == 'x' else 'horizontal at y'
            if w >= 1.5 and abs(pos - round(pos)) > 0.01:
                bad += 1
                print('  FIX   %-32s %s = %s, %s wide: needs a whole coordinate' % (name, where, f(pos), f(w)))
            elif w < 1.5 and abs(pos % 1 - 0.5) > 0.01:
                bad += 1
                print('  FIX   %-32s %s = %s, %s wide: needs a half coordinate' % (name, where, f(pos), f(w)))
            elif w >= 1.5 and round(pos) % 2 == 0 and '--notes' in sys.argv:
                notes += 1
                print('  note  %-32s %s = %s: even, so it blurs at 16 px' % (name, where, f(pos)))
    print('%d straight strokes between pixels%s' % (bad, ', %d notes' % notes if notes else ''))
    return 1 if bad else 0


def main():
    if '--audit' in sys.argv:
        sys.exit(audit())
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'resources')
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'icons')
    os.makedirs(out, exist_ok=True)
    for name in sorted(ICONS):
        with open(os.path.join(out, name + '.svg'), 'w') as fh:
            fh.write(render(name))
    # AUTORCC only bundles what the .qrc lists, so it is written from the
    # same table as the pictures and cannot drift from them.
    if len(sys.argv) <= 1:
        with open(os.path.join(root, 'icons.qrc'), 'w') as fh:
            fh.write('<!DOCTYPE RCC>\n<!-- Written by tools/make_icons.py. -->\n'
                     '<RCC version="1.0">\n<qresource prefix="/">\n')
            for name in sorted(ICONS):
                fh.write('    <file>icons/%s.svg</file>\n' % name)
            fh.write('</qresource>\n</RCC>\n')
    print('%d icons -> %s' % (len(ICONS), os.path.normpath(out)))


if __name__ == '__main__':
    main()
