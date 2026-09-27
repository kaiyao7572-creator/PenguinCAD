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


def outline(w=1.25):
    return 'stroke="%s" stroke-width="%s" stroke-linejoin="round"' % (INK, f(w))


class Iso:
    def __init__(self, cx, cy, k=1.0):
        self.cx, self.cy, self.k = cx, cy, k

    def __call__(self, x, y, z):
        return (self.cx + (x - y) * C30 * self.k, self.cy + (x + y) * 0.5 * self.k - z * self.k)


def fit(points3d, x0=2.5, y0=2.5, x1=29.5, y1=29.5, kmax=1.6):
    """An Iso that centres the given world points in the box."""
    probe = Iso(0, 0, 1)
    sp = [probe(*p) for p in points3d]
    minx, maxx = min(p[0] for p in sp), max(p[0] for p in sp)
    miny, maxy = min(p[1] for p in sp), max(p[1] for p in sp)
    k = min((x1 - x0) / max(maxx - minx, 1e-6), (y1 - y0) / max(maxy - miny, 1e-6), kmax)
    cx = (x0 + x1) / 2 - k * (minx + maxx) / 2
    cy = (y0 + y1) / 2 - k * (miny + maxy) / 2
    return Iso(cx, cy, k)


def corners(x0, y0, z0, x1, y1, z1):
    return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def poly(ps, fill, stroke=True, w=1.25, extra=''):
    s = '<polygon points="%s" fill="%s"' % (pts(ps), fill)
    if stroke:
        s += ' ' + outline(w)
    return s + extra + '/>'


def box(P, x0, y0, z0, x1, y1, z1, shades=GREY, w=1.25):
    top, left, right = shades
    t = [P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)]
    r = [P(x1, y0, z1), P(x1, y1, z1), P(x1, y1, z0), P(x1, y0, z0)]
    l = [P(x0, y1, z1), P(x1, y1, z1), P(x1, y1, z0), P(x0, y1, z0)]
    return poly(l, left, w=w) + poly(r, right, w=w) + poly(t, top, w=w)


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


@icon('sketch.create')
def _():
    P = Iso(16, 13.2, 1.1)
    s = sketch_plane(P, 14)
    rect = [P(2.5, 3, 0), P(10.5, 3, 0), P(10.5, 11, 0), P(2.5, 11, 0)]
    s += path(d_of(rect, True), stroke=BLUE, w=1.9)
    s += dot(*P(10.5, 3, 0), r=2.0)
    s += pencil(*P(10.5, 3, 0), length=18, width=5.6, angle=-55)
    return s


@icon('sketch.edit')
def _():
    P = Iso(16, 13.2, 1.1)
    s = sketch_plane(P, 14)
    rect = [P(2.5, 3, 0), P(10.5, 3, 0), P(10.5, 11, 0), P(2.5, 11, 0)]
    s += path(d_of(rect, True), stroke=INK, w=1.5)
    s += pencil(*P(10.5, 3, 0), length=18, width=5.6, angle=-55, body=BLUE, band=DBLUE)
    return s


@icon('solid.box')
def _():
    P = fit(corners(0, 0, 0, 12, 12, 12), 3.5, 2.5, 28.5, 29.5)
    return box(P, 0, 0, 0, 12, 12, 12, BLUES)


@icon('solid.cylinder')
def _():
    P = Iso(16, 16, 1.0)
    ex, ey, rx, ry = iso_ellipse(P, 0, 0, 0, 9.2)
    P = Iso(16, 23.2, 1.0)
    return cylinder(P, 0, 0, 0, 15.5, 9.2, 'g')


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
    cx, cy = 16, 16.8
    R, r = 13.8, 8.6       # outer ellipse radii
    hx, hy = 5.6, 2.7      # the hole
    hcy = cy - 1.6
    s = defs(rad_grad('g', [(0, '#D2EEFB'), (0.38, '#45B3E6'), (0.8, BLUE), (1, DBLUE)], 0.42, 0.22, 0.95))
    outer = arc_pts(cx, cy, R, r, 0, 360, n=48)
    hole = arc_pts(cx, hcy, hx, hy, 0, 360, n=32)
    s += path(d_of(outer, True) + ' ' + d_of(hole, True), stroke=INK, w=1.25, fill='url(#g)',
              extra=' fill-rule="evenodd"')
    # The far inner wall of the tube -- the only part of the inside the eye
    # reaches through the hole -- fills its upper half.
    wall = arc_pts(cx, hcy, hx, hy, 180, 360, n=16) + list(
        reversed(arc_pts(cx, hcy + 0.2, hx, hy * 0.35, 180, 360, n=16)))
    s += poly(wall, DBLUE, w=1.0)
    return s


@icon('solid.extrude')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0) + [(4, 4, 8), (12, 12, 8), (8, 8, 19)], 2.5, 2, 29.5, 30)
    s = poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT)
    s += box(P, 4, 4, 0, 12, 12, 8, BLUES)
    x, y = P(8, 8, 8)
    x2, y2 = P(8, 8, 19)
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


@icon('solid.pattern.rectangular')
def _():
    a, g = 7, 5
    cells = [(0, 0), (a + g, 0), (0, a + g), (a + g, a + g)]
    P = fit(corners(0, 0, 0, 2 * a + g, 2 * a + g, a), 2.5, 3, 29.5, 29)
    s = ''
    for x, y in cells:
        s += box(P, x, y, 0, x + a, y + a, a, GREY if (x, y) == (0, a + g) else BLUES)
    return s


@icon('solid.pattern.circular')
def _():
    R, a = 9.5, 4.6
    places = []
    for i in range(6):
        t = math.radians(135 + i * 60)
        places.append((R * math.cos(t), R * math.sin(t), i == 0))
    P = fit([(x + dx, y + dy, z) for x, y, _ in places for dx in (-a / 2, a / 2) for dy in (-a / 2, a / 2)
             for z in (0, a)], 2.5, 3, 29.5, 29)
    ex, ey, rx, ry = iso_ellipse(P, 0, 0, 0, R)
    s = ellipse(ex, ey, rx, ry, stroke='currentColor', w=1.2, dash='2.2 1.8')
    s += dot(ex, ey, r=2.0, fill='currentColor')
    for x, y, first in sorted(places, key=lambda p: p[0] + p[1]):
        s += box(P, x - a / 2, y - a / 2, 0, x + a / 2, y + a / 2, a, GREY if first else BLUES, w=1.1)
    return s


@icon('solid.mirror')
def _():
    P = fit(corners(0, -2, -1.5, 15, 9, 11), 2.5, 2.5, 29.5, 29.5)
    s = box(P, 0, 0, 0, 5.5, 7, 9)
    s += ('<polygon points="%s" fill="%s" fill-opacity="0.45" stroke="%s" stroke-width="1.3" '
          'stroke-linejoin="round"/>' % (
              pts([P(7.5, -2, -1.5), P(7.5, 9, -1.5), P(7.5, 9, 11), P(7.5, -2, 11)]), LBLUE, BLUE))
    s += box(P, 9.5, 0, 0, 15, 7, 9, BLUES)
    return s


# ---------------------------------------------------------------- SOLID: MODIFY

@icon('modify.press_pull')
def _():
    P = fit(corners(0, 0, 0, 14, 14, 7) + [(7, 7, 20)], 3, 2, 29, 30)
    s = box(P, 0, 0, 0, 14, 14, 7, (BLUE, MID, DARK))
    x, y = P(7, 7, 7)
    x2, y2 = P(7, 7, 19.5)
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
        s += arrow(c + dx * 4, c + dy * 4, c + dx * 13.6, c + dy * 13.6, stroke=BLUE, w=2.1, size=5.0)
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
    s += poly([P(0, 14, 14), P(14, 14, 14), P(14, 14, 0), P(0, 14, 0)], LBLUE, stroke=False, extra=' fill-opacity="0.35"')
    s += poly([P(14, 0, 14), P(14, 14, 14), P(14, 14, 0), P(14, 0, 0)], BLUE, stroke=False, extra=' fill-opacity="0.3"')
    s += poly(big, LBLUE, stroke=False, extra=' fill-opacity="0.45"')
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
    s += '<rect x="13.5" y="15.5" width="16" height="13.5" rx="1.5" fill="%s" %s/>' % (LIGHT, outline())
    s += '<rect x="16" y="18.5" width="11" height="3.2" fill="#FFFFFF" stroke="%s" stroke-width="0.9"/>' % INK
    s += '<rect x="16" y="23.4" width="11" height="3.2" fill="#FFFFFF" stroke="%s" stroke-width="0.9"/>' % INK
    s += line([(17.6, 20.1), (21.5, 20.1)], stroke=BLUE, w=1.3, cap='butt')
    s += line([(17.6, 25), (23.5, 25)], stroke=BLUE, w=1.3, cap='butt')
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
    s += line([(6.6, 12.6), (14.4, 12.6)], w=2.0)
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
    return '<polygon points="%s" fill="%s" fill-opacity="0.7" stroke="%s" stroke-width="1.35" stroke-linejoin="round"/>' % (
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
    a = math.radians(58)
    L = 13
    P = fit(corners(0, 0, 0, 15, 14, 0) + [(L * math.cos(a), 0, L * math.sin(a)), (L * math.cos(a), 14, L * math.sin(a))], 3, 3, 29, 29)
    s = poly([P(0, 0, 0), P(15, 0, 0), P(15, 14, 0), P(0, 14, 0)], LIGHT)
    tip = lambda y: P(L * math.cos(a), y, L * math.sin(a))
    s += blue_plane([P(0, 0, 0), tip(0), tip(14), P(0, 14, 0)])
    # The angle, drawn at the near end of the hinge.
    arc = [P(6 * math.cos(math.radians(t)), 14, 6 * math.sin(math.radians(t))) for t in range(0, 59, 6)]
    s += line(arc, w=1.4)
    return s


@icon('construct.plane_midplane')
def _():
    P = fit(corners(0, -2, 0, 14, 12, 12) + [(7, -2, 14), (7, 14, -2)], 3, 2.5, 29, 29.5)
    s = box(P, 0, 0, 0, 7, 10, 9)
    s += blue_plane([P(7, -2.5, -1.5), P(7, 12.5, -1.5), P(7, 12.5, 12), P(7, -2.5, 12)])
    s += box(P, 7.6, 0, 0, 14, 10, 9)
    return s


@icon('construct.plane_three_points')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 7, 29.5, 26)
    s = blue_plane([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)])
    tri = [P(4, 3, 0), P(13.5, 5, 0), P(5, 13, 0)]
    s += line(tri + [tri[0]], w=1.2, dash='2 1.6')
    for x, y in tri:
        s += dot(x, y, r=2.4, fill='currentColor')
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
    s += line([(x, y), (x, 2.5)], stroke=BLUE, w=2.2)
    s += line([P(8, 11, 0), (P(8, 11, 0)[0], P(8, 11, 0)[1] - 3.2), (x, y - 3.2)], stroke=INK, w=1.1, cap='butt')
    s += dot(x, y, r=1.8, fill=BLUE)
    return s


@icon('construct.point_coordinates')
def _():
    o = (9, 22)
    s = arrow(o[0], o[1], o[0], 5, w=1.5, size=3.6)
    s += arrow(o[0], o[1], 27, o[1] + 0.01, w=1.5, size=3.6)
    s += arrow(o[0], o[1], 3.2, 28.2, w=1.5, size=3.4)
    p = (21, 10)
    s += line([(p[0], p[1]), (p[0], o[1])], w=1.1, dash='1.8 1.4')
    s += line([(p[0], p[1]), (o[0], p[1])], w=1.1, dash='1.8 1.4')
    s += dot(p[0], p[1], r=3.0, fill=BLUE)
    return s


@icon('construct.point_axis_plane')
def _():
    P = fit(corners(0, 0, 0, 16, 16, 0), 2.5, 13, 29.5, 27)
    x, y = P(8, 8, 0)
    s = line([(x + 5.5, y + 12), (x, y)], stroke=BLUE, w=2.0, dash='1.8 1.6')
    s += poly([P(0, 0, 0), P(16, 0, 0), P(16, 16, 0), P(0, 16, 0)], LIGHT)
    s += line([(x, y), (x - 8.5, 2.5)], stroke=BLUE, w=2.2)
    s += dot(x, y, r=3.1, fill='currentColor')
    s += dot(x, y, r=1.6, fill=BLUE)
    return s


# ---------------------------------------------------------------- SELECT

def select_cube(P, what):
    a = 12
    shades = BLUES if what == 'body' else GREY
    s = box(P, 0, 0, 0, a, a, a, shades)
    if what == 'face':
        s += poly([P(0, 0, a), P(a, 0, a), P(a, a, a), P(0, a, a)], BLUE)
    elif what == 'edge':
        s += line([P(a, a, a), P(a, a, 0)], stroke=BLUE, w=3.2)
        s += line([P(a, a, a), P(a, a, 0)], stroke=LBLUE, w=1.0)
    elif what == 'vertex':
        x, y = P(a, a, a)
        s += dot(x, y, r=3.4, fill=BLUE, ring='#FFFFFF')
    return s


@icon('select.bodies')
def _():
    return select_cube(fit(corners(0, 0, 0, 12, 12, 12), 4, 3, 28, 29), 'body')


@icon('select.faces')
def _():
    return select_cube(fit(corners(0, 0, 0, 12, 12, 12), 4, 3, 28, 29), 'face')


@icon('select.edges')
def _():
    return select_cube(fit(corners(0, 0, 0, 12, 12, 12), 4, 3, 28, 29), 'edge')


@icon('select.vertices')
def _():
    return select_cube(fit(corners(0, 0, 0, 12, 12, 12), 4, 3, 28, 29), 'vertex')


def priority(what):
    P = fit(corners(0, 0, 0, 12, 12, 12), 2, 2, 23, 24)
    return select_cube(P, what) + pointer(19.5, 16.5, 1.02)


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
    P = Iso(12, 16, 0.95)
    s = poly([P(0, 0, 0), P(13, 0, 0), P(13, 13, 0), P(0, 13, 0)], LIGHT)
    s += path(d_of([P(3, 3, 0), P(9.5, 3, 0), P(9.5, 9.5, 0), P(3, 9.5, 0)], True), stroke=INK, w=1.2)
    s += line([(11.5, 16.5), (17.5, 23), (28.5, 7.5)], stroke=INK, w=6.4)
    s += line([(11.5, 16.5), (17.5, 23), (28.5, 7.5)], stroke=GREEN, w=4.0)
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
    s = eye(14, 12)
    s += '<rect x="18.5" y="18.5" width="11" height="11" rx="1.6" fill="%s" stroke="%s" stroke-width="1.1"/>' % (LIGHT, INK)
    s += line([(21, 26.8), (27, 26.8)], stroke=INK, w=1.5, cap='butt')
    s += line([(24, 26.8), (24, 21)], stroke=INK, w=1.5, cap='butt')
    return s


@icon('sketch.visible')
def _():
    s = eye(14, 12)
    s += path('M19,28.5 L19,20 L28.5,20 L28.5,28.5 Z', stroke='currentColor', w=1.5)
    s += dot(19, 28.5, r=1.9) + dot(28.5, 20, r=1.9)
    return s


@icon('sketch.line')
def _():
    return line([(6, 26), (26, 6)]) + dot(6, 26, 2.6) + dot(26, 6, 2.6)


@icon('sketch.rectangle')
def _():
    s = path('M5,8 L27,8 L27,24 L5,24 Z')
    return s + dot(5, 8, 2.6) + dot(27, 24, 2.6)


@icon('sketch.rectangle.centre')
def _():
    s = path('M5,8 L27,8 L27,24 L5,24 Z')
    s += line([(16, 16), (27, 24)], w=1.2, dash='1.8 1.5')
    return s + dot(16, 16, 2.6) + dot(27, 24, 2.6)


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
    r = 9.2
    R = r / math.cos(math.radians(30))
    hexa = polygon_pts(16, 16, R, 6, 30)
    s = circle(16, 16, r, w=1.1).replace('/>', ' stroke-opacity="0.5"/>')
    s += path(d_of(hexa, True))
    return s + dot(16, 16, 2.4) + dot(16, 16 + r, 2.6)


@icon('sketch.polygon.inscribed')
def _():
    R = 12
    hexa = polygon_pts(16, 16, R, 6, 0)
    s = circle(16, 16, R, w=1.1) .replace('/>', ' stroke-opacity="0.5"/>')
    s += path(d_of(hexa, True))
    return s + dot(16, 16, 2.4) + dot(16 + R, 16, 2.6)


@icon('sketch.polygon.edge')
def _():
    R = 11
    hexa = polygon_pts(16, 15, R, 6, 30)
    s = path(d_of(hexa, True))
    a, b = hexa[1], hexa[2]
    s += line([a, b], stroke=BLUE, w=2.4)
    return s + dot(*a, r=2.6) + dot(*b, r=2.6)


@icon('sketch.ellipse')
def _():
    s = ellipse(16, 16, 13, 8, stroke='currentColor', w=1.75)
    s += line([(3, 16), (29, 16)], w=1.1, dash='1.8 1.5')
    s += line([(16, 8), (16, 24)], w=1.1, dash='1.8 1.5')
    return s + dot(16, 16, 2.4) + dot(29, 16, 2.4) + dot(16, 8, 2.4)


@icon('sketch.slot')
def _():
    s = path('M9,9.5 L23,9.5 A6.5,6.5 0 0 1 23,22.5 L9,22.5 A6.5,6.5 0 0 1 9,9.5 Z')
    s += line([(9, 16), (23, 16)], w=1.2, dash='1.8 1.5')
    return s + dot(9, 16, 2.4) + dot(23, 16, 2.4)


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
    s = line([(6, 7), (6, 16), ], w=1.2, dash='1.8 1.5') + line([(6, 7), (15, 7)], w=1.2, dash='1.8 1.5')
    s += line([(6, 28), (6, 16)])
    s += line([(15, 7), (28, 7)])
    s += path('M6,16 A9,9 0 0 1 15,7', stroke=BLUE, w=2.3)
    return s + dot(6, 16, 2.3) + dot(15, 7, 2.3)


@icon('sketch.trim')
def _():
    s = line([(3, 8), (10, 8)]) + line([(22, 8), (29, 8)])
    s += line([(10, 8), (22, 8)], stroke=BLUE, w=1.75, dash='2 1.6')
    s += line([(10, 3), (10, 13)], w=1.4) + line([(22, 3), (22, 13)], w=1.4)
    # Scissors, blades up.
    s += line([(12.4, 21.5), (19.8, 11.6)], w=1.8)
    s += line([(19.6, 21.5), (12.2, 11.6)], w=1.8)
    s += circle(11.2, 25.2, 3.1, w=1.7) + circle(20.8, 25.2, 3.1, w=1.7)
    s += dot(16, 16.6, 1.1, fill='currentColor')
    return s


@icon('sketch.extend')
def _():
    s = line([(27, 4), (27, 28)], w=1.75)
    s += line([(4, 20), (13, 20)])
    s += line([(13, 20), (20.5, 20)], stroke=BLUE, w=1.75, dash='2 1.6', cap='butt')
    s += arrowhead(13, 20, 26, 20, BLUE, size=5, width=5)
    return s + dot(4, 20, 2.0, fill='currentColor')


@icon('sketch.offset')
def _():
    s = '<rect x="10" y="10" width="12" height="12" rx="3" fill="none" stroke="currentColor" stroke-width="1.75"/>'
    s += '<rect x="4" y="4" width="24" height="24" rx="7" fill="none" stroke="%s" stroke-width="1.9"/>' % BLUE
    return s


@icon('sketch.mirror')
def _():
    s = line([(16, 3), (16, 29)], w=1.4, dash='5 1.6 1.2 1.6')
    s += path('M12.5,8 L12.5,24 L3.5,24 Z', w=1.75)
    s += path('M19.5,8 L19.5,24 L28.5,24 Z', stroke=BLUE, w=1.9)
    return s


@icon('sketch.pattern.rectangular')
def _():
    s = ''
    for i in range(3):
        for j in range(3):
            x, y = 4 + i * 9, 4 + j * 9
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
    s += line([(5, 12), (27, 12)], stroke=BLUE, w=2.2)
    return s + dot(5, 12, 2.4, fill='currentColor') + dot(27, 12, 2.4, fill='currentColor')


@icon('sketch.constraint.vertical')
def _():
    s = line([(24, 5), (20, 27)], w=1.2, dash='1.8 1.5')
    s += line([(12, 5), (12, 27)], stroke=BLUE, w=2.2)
    return s + dot(12, 5, 2.4, fill='currentColor') + dot(12, 27, 2.4, fill='currentColor')


@icon('sketch.constraint.parallel')
def _():
    return line([(4, 20), (18, 5)]) + line([(14, 27), (28, 12)], stroke=BLUE, w=2.2)


@icon('sketch.constraint.perpendicular')
def _():
    s = line([(5, 27), (27, 27)])
    s += line([(14, 27), (14, 5)], stroke=BLUE, w=2.2)
    s += line([(14, 21), (20, 21), (20, 27)], w=1.2, cap='butt')
    return s


@icon('sketch.constraint.equal')
def _():
    s = line([(6, 5), (6, 27)]) + line([(26, 5), (26, 27)], stroke=BLUE, w=2.2)
    s += line([(11.5, 13.5), (20.5, 13.5)], w=2.0) + line([(11.5, 18.5), (20.5, 18.5)], w=2.0)
    return s


@icon('sketch.constraint.tangent')
def _():
    s = circle(14, 19, 9)
    s += line([(3, 10), (29, 10)], stroke=BLUE, w=2.2)
    return s + dot(14, 10, 2.6, fill='currentColor')


@icon('sketch.constraint.midpoint')
def _():
    s = line([(4, 24), (28, 8)])
    s += poly([(16, 9.2), (21, 17.6), (11, 17.6)], BLUE, stroke=False)
    return s + dot(4, 24, 2.0, fill='currentColor') + dot(28, 8, 2.0, fill='currentColor')


@icon('sketch.constraint.concentric')
def _():
    return circle(16, 16, 12) + circle(16, 16, 6.4, stroke=BLUE, w=2.1) + dot(16, 16, 2.2, fill='currentColor')


@icon('sketch.constraint.collinear')
def _():
    s = line([(3, 26), (13, 17.5)])
    s += line([(13, 17.5), (19, 12.4)], w=1.2, dash='1.8 1.5')
    s += line([(19, 12.4), (29, 4)], stroke=BLUE, w=2.2)
    return s


@icon('sketch.constraint.fix')
def _():
    s = path('M11,15 L11,10.5 A5,5 0 0 1 21,10.5 L21,15', w=2.2)
    s += '<rect x="7.5" y="14.5" width="17" height="13" rx="2" fill="%s" stroke="%s" stroke-width="1.25"/>' % (BLUE, INK)
    s += dot(16, 20, 1.9, fill='#FFFFFF') + line([(16, 20.5), (16, 24)], stroke='#FFFFFF', w=1.8)
    return s


@icon('sketch.constraint.symmetry')
def _():
    s = line([(16, 3), (16, 29)], w=1.4, dash='5 1.6 1.2 1.6')
    s += line([(4, 24), (11, 8)]) + line([(28, 24), (21, 8)], stroke=BLUE, w=2.2)
    return s + dot(4, 24, 2.2, fill='currentColor') + dot(28, 24, 2.4)


# ---------------------------------------------------------------- SKETCH INSPECT

@icon('sketch.dimension')
def _():
    s = line([(5, 27), (5, 12)], w=1.2) + line([(27, 27), (27, 12)], w=1.2)
    s += arrow(9.5, 17, 5.4, 17, w=1.4, size=4) + arrow(22.5, 17, 26.6, 17, w=1.4, size=4)
    s += '<rect x="9.5" y="12.5" width="13" height="9" rx="1.2" fill="%s"/>' % BLUE
    s += line([(12.5, 17), (19.5, 17)], stroke='#FFFFFF', w=1.6, cap='butt')
    s += line([(5, 27), (27, 27)], w=1.75)
    return s


@icon('sketch.dimension.clear')
def _():
    s = line([(5, 26), (5, 13)], w=1.2) + line([(23, 26), (23, 13)], w=1.2)
    s += arrow(13, 17, 5.4, 17, w=1.4, size=4, double=False) + arrow(15, 17, 22.6, 17, w=1.4, size=4)
    s += circle(24, 8.5, 6, stroke=None, fill=RED)
    s += line([(21.4, 5.9), (26.6, 11.1)], stroke='#FFFFFF', w=1.8)
    s += line([(26.6, 5.9), (21.4, 11.1)], stroke='#FFFFFF', w=1.8)
    return s


# ---------------------------------------------------------------- VIEW

def view_cube(face):
    a = 12
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29)
    hidden = face in ('back', 'left', 'bottom')
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
        s += poly(faces[face], BLUE, stroke=False)
        for e in ([P(0, 0, 0), P(a, 0, 0)], [P(0, 0, 0), P(0, a, 0)], [P(0, 0, 0), P(0, 0, a)]):
            s += line(e, stroke=INK, w=1.1, dash='1.6 1.3')
        op = ' fill-opacity="0.55"'
        s += poly(faces['front'], MID, extra=op) + poly(faces['right'], DARK, extra=op) + poly(faces['top'], LIGHT, extra=op)
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
    for (x, y, dx, dy) in ((4, 4, 1, 1), (28, 4, -1, 1), (4, 28, 1, -1), (28, 28, -1, -1)):
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
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29)
    return box(P, 0, 0, 0, a, a, a, BLUES, w=1.9)


@icon('view.display_wireframe')
def _():
    a = 12
    P = fit(corners(0, 0, 0, a, a, a), 4, 3, 28, 29)
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
        x = 3 + i * 26 / 9
        s += line([(x, 23), (x, 25.2 if i % 3 else 26.8)], stroke=INK, w=1.0, cap='butt')
    s += line([(5, 5), (5, 18)], w=1.2) + line([(27, 5), (27, 18)], w=1.2)
    s += arrow(5.2, 11, 26.8, 11, stroke=BLUE, w=2.0, size=4.6, double=True)
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


@icon('inspect.section_view')
def _():
    A, H, c = 14, 11, 7
    P = fit(corners(-1.5, 0, -1.5, A + 1.5, A, H + 1.5), 2.5, 2.5, 29.5, 29.5)
    # The half the section plane cuts away, only as a ghost.
    ghost = [[P(0, A, H), P(A, A, H), P(A, c, H)], [P(A, A, H), P(A, A, 0), P(A, c, 0)],
             [P(0, A, H), P(0, A, 0), P(A, A, 0)]]
    s = ''.join(line(g, w=1.0, dash='1.6 1.4') for g in ghost)
    s += poly([P(0, 0, H), P(A, 0, H), P(A, c, H), P(0, c, H)], LIGHT)
    s += poly([P(A, 0, H), P(A, c, H), P(A, c, 0), P(A, 0, 0)], DARK)
    s += poly([P(0, c, H), P(A, c, H), P(A, c, 0), P(0, c, 0)], BLUE)
    # Hatching on the cut face, as a drawing marks a section.
    for i in range(1, 6):
        t = i * A / 6
        k = min(t, H)
        s += line([P(t, c, H), P(t - k, c, H - k)], stroke=LBLUE, w=1.0, cap='butt')
    s += ('<polygon points="%s" fill="none" stroke="%s" stroke-width="1.3" stroke-dasharray="2.4 1.6" '
          'stroke-linejoin="round"/>' % (
              pts([P(-1.5, c, H + 1.5), P(A + 1.5, c, H + 1.5), P(A + 1.5, c, -1.5), P(-1.5, c, -1.5)]), BLUE))
    return s


# ---------------------------------------------------------------- output

def render(name):
    body = ICONS[name]()
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32" '
            'color="%s">\n%s\n</svg>\n' % (INK, body.replace('/><', '/>\n<')))


def main():
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
