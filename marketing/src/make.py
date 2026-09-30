#!/usr/bin/env python3
"""Draw PenguinCAD's marketing images as vector art and render them to PNG.

    python3 marketing/src/make.py

Writes marketing/og.png (the 1200 x 630 link preview); the Instagram posts
come from casual.py. Every image is
an inline SVG in a small HTML page, rendered by headless Chrome so the
site's own fonts (Archivo, B612 Mono) are used; the matching .svg sources
are written next to each PNG.
"""

import base64
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "marketing"
FONTS = ROOT / "website" / "src" / "fonts"

BG = "#0b1119"
INK = "#e7eef5"
INK2 = "#a9b8c8"
INK3 = "#6f839a"
BLUE = "#22a3e6"
BLUE2 = "#7cc6ea"
ORANGE = "#ff9f43"
LINE = "#223142"
EDGE = "#0a0f15"

DISPLAY = "font-family:Archivo;font-stretch:125%;font-weight:800"
BODY = "font-family:Archivo;font-weight:500"
MONO = "font-family:'B612 Mono'"


def font_face():
    def data(name):
        return base64.b64encode((FONTS / name).read_bytes()).decode()

    return (
        "@font-face{font-family:Archivo;font-weight:400 800;font-stretch:100% 125%;"
        f"src:url(data:font/woff2;base64,{data('archivo.woff2')}) format('woff2')}}"
        "@font-face{font-family:'B612 Mono';"
        f"src:url(data:font/woff2;base64,{data('b612mono.woff2')}) format('woff2')}}"
    )


# ---------------------------------------------------------------- pieces


def defs():
    return f"""
<defs>
  <pattern id="minor" width="36" height="36" patternUnits="userSpaceOnUse">
    <path d="M36 0H0V36" fill="none" stroke="#80aad2" stroke-opacity=".06" stroke-width="1"/>
  </pattern>
  <pattern id="major" width="180" height="180" patternUnits="userSpaceOnUse">
    <path d="M180 0H0V180" fill="none" stroke="#80aad2" stroke-opacity=".11" stroke-width="1"/>
  </pattern>
  <radialGradient id="fade" cx="50%" cy="40%" r="75%">
    <stop offset="0" stop-color="#000" stop-opacity="0"/>
    <stop offset="1" stop-color="{BG}" stop-opacity=".95"/>
  </radialGradient>
  <radialGradient id="glow" cx="50%" cy="50%" r="50%">
    <stop offset="0" stop-color="{BLUE}" stop-opacity=".30"/>
    <stop offset="1" stop-color="{BLUE}" stop-opacity="0"/>
  </radialGradient>
  <radialGradient id="glowOrange" cx="50%" cy="50%" r="50%">
    <stop offset="0" stop-color="{ORANGE}" stop-opacity=".22"/>
    <stop offset="1" stop-color="{ORANGE}" stop-opacity="0"/>
  </radialGradient>
  <linearGradient id="top" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#eef3f7"/>
    <stop offset="1" stop-color="#aebccb"/>
  </linearGradient>
  <linearGradient id="side" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#4c5c6e"/>
    <stop offset=".45" stop-color="#9aa9b8"/>
    <stop offset=".7" stop-color="#7b8a9a"/>
    <stop offset="1" stop-color="#3f4e5f"/>
  </linearGradient>
  <linearGradient id="hole" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#0c131b"/>
    <stop offset="1" stop-color="#2a3847"/>
  </linearGradient>
  <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" orient="auto-start-reverse">
    <path d="M0 1.5 10 5 0 8.5z" fill="{ORANGE}"/>
  </marker>
  <symbol id="penguin" viewBox="0 0 64 64">
    <circle cx="32" cy="33" r="29" fill="none" stroke="{BLUE}" stroke-width="1.6" stroke-dasharray="4.2 3.2"/>
    <path d="M30.5 7.5c9.8 0 15.9 7.6 16.6 17.2.5 7 3.4 11.4 3.4 18.3 0 9.6-7.9 15.5-18.5 15.5S13.6 52.4 13.6 42.4c0-6.2 2.6-10.6 2.6-17.4C16.2 14.9 21.6 7.5 30.5 7.5z" fill="#0E1722" stroke="#3B5068" stroke-width="1.2"/>
    <path d="M33.8 14.6c5.6.4 8.6 4.6 9 10.2.4 6.2 3.4 10 3.4 17 0 7.4-5.6 11.9-13.3 11.9-7.4 0-12.4-4.4-12.4-11.3 0-6.5 3.6-9.8 4.4-15.6.9-7.3 3.4-12.6 8.9-12.2z" fill="#F4F7FA"/>
    <path d="M40.6 20.2 52.8 23.6 40.9 26.9c-1.4-2.2-1.5-4.4-.3-6.7z" fill="#FF9F43"/>
    <circle cx="35.6" cy="20.4" r="2.3" fill="#0E1722"/>
    <circle cx="36.3" cy="19.7" r=".75" fill="#F4F7FA"/>
    <path d="M22 58.6h19.5" stroke="#FF9F43" stroke-width="3.2" stroke-linecap="round"/>
  </symbol>
</defs>"""


def ground(w, h):
    return (f'<rect width="{w}" height="{h}" fill="{BG}"/>'
            f'<rect width="{w}" height="{h}" fill="url(#minor)"/>'
            f'<rect width="{w}" height="{h}" fill="url(#major)"/>'
            f'<rect width="{w}" height="{h}" fill="url(#fade)"/>')


def text(x, y, s, size, style=BODY, fill=INK, anchor="start", extra=""):
    return (f'<text x="{x}" y="{y}" font-size="{size}" style="{style}" fill="{fill}" '
            f'text-anchor="{anchor}" {extra}>{s}</text>')


K = 0.52  # the isometric ellipse's height over its width


def ell(cx, cy, r, **kw):
    attrs = " ".join(f'{k.replace("_", "-")}="{v}"' for k, v in kw.items())
    return f'<ellipse cx="{cx:.1f}" cy="{cy:.1f}" rx="{r:.1f}" ry="{r * K:.1f}" {attrs}/>'


def cylinder_side(cx, top, bottom, r, fill):
    """The visible front of a vertical cylinder between two heights."""
    return (f'<path d="M{cx - r:.1f} {top:.1f} L{cx - r:.1f} {bottom:.1f} '
            f'A{r:.1f} {r * K:.1f} 0 0 0 {cx + r:.1f} {bottom:.1f} '
            f'L{cx + r:.1f} {top:.1f} Z" fill="{fill}" stroke="{EDGE}" stroke-width="2"/>')


def flange(cx, cy, s=1.0, holes=6, hub=True, fillet=True, cut=True, glow=True):
    """An isometric hub flange: a plate with bolt holes, a hub and a bore.
    cy is the top face of the plate."""
    R, T = 260 * s, 64 * s
    P, hr = 204 * s, 25 * s
    r, H, bore = 148 * s, 118 * s, 84 * s
    out = []
    if glow:
        out.append(f'<ellipse cx="{cx}" cy="{cy + T + 18 * s}" rx="{R * 1.35:.0f}" ry="{R * 0.62:.0f}" fill="url(#glow)"/>')
        out.append(ell(cx, cy + T + 10 * s, R * 1.02, fill="#000", fill_opacity=".45"))
    # plate
    out.append(cylinder_side(cx, cy, cy + T, R, "url(#side)"))
    out.append(ell(cx, cy, R, fill="url(#top)", stroke=EDGE, stroke_width="2"))
    if cut:
        for i in range(holes):
            a = math.pi / 6 + i * 2 * math.pi / holes
            hx, hy = cx + P * math.cos(a), cy + P * K * math.sin(a)
            out.append(ell(hx, hy, hr, fill="url(#hole)", stroke=EDGE, stroke_width="1.6"))
            out.append(f'<path d="M{hx - hr:.1f} {hy:.1f} A{hr:.1f} {hr * K:.1f} 0 0 0 {hx + hr:.1f} {hy:.1f}" '
                       f'fill="none" stroke="#c8d3de" stroke-opacity=".35" stroke-width="1.2"/>')
    if hub:
        if fillet:
            out.append(ell(cx, cy - 3 * s, r + 22 * s, fill="#c6d1dc", stroke="#8c9aa9", stroke_width="1.2"))
            out.append(ell(cx, cy - 8 * s, r + 11 * s, fill="#aebbc8"))
        out.append(cylinder_side(cx, cy - H, cy - 8 * s, r, "url(#side)"))
        out.append(ell(cx, cy - H, r, fill="url(#top)", stroke=EDGE, stroke_width="2"))
        out.append(ell(cx, cy - H, bore, fill="url(#hole)", stroke=EDGE, stroke_width="1.8"))
        out.append(f'<path d="M{cx - bore:.1f} {cy - H:.1f} A{bore:.1f} {bore * K:.1f} 0 0 0 {cx + bore:.1f} {cy - H:.1f}" '
                   f'fill="none" stroke="#c8d3de" stroke-opacity=".3" stroke-width="1.4"/>')
    return "\n".join(out)


def dim_h(x1, x2, y, label, size=26, ext=40):
    """A horizontal dimension: extension lines, arrows and a centred value."""
    mid = (x1 + x2) / 2
    w = len(label) * size * 0.62 + 20
    return (f'<g stroke="{ORANGE}" stroke-width="1.6" fill="none">'
            f'<path d="M{x1} {y - 10}V{y + ext}M{x2} {y - 10}V{y + ext}"/>'
            f'<path d="M{x1 + 3} {y}H{mid - w / 2}M{mid + w / 2} {y}H{x2 - 3}" marker-start="url(#arrow)" marker-end="url(#arrow)"/></g>'
            + text(mid, y + size * 0.36, label, size, MONO, ORANGE, "middle"))


def dim_v(x, y1, y2, label, size=26):
    mid = (y1 + y2) / 2
    return (f'<g stroke="{ORANGE}" stroke-width="1.6" fill="none">'
            f'<path d="M{x} {y1 + 3}V{y2 - 3}" marker-start="url(#arrow)" marker-end="url(#arrow)"/>'
            f'<path d="M{x - 26} {y1}H{x + 26}M{x - 26} {y2}H{x + 26}"/></g>'
            + text(x + 22, mid + size * 0.36, label, size, MONO, ORANGE))


def brand(x, y, size=44):
    return (f'<use href="#penguin" x="{x}" y="{y - size * 0.82}" width="{size * 1.15}" height="{size * 1.15}"/>'
            f'<text x="{x + size * 1.35}" y="{y}" font-size="{size * 0.62}" style="{DISPLAY}" fill="{INK}">'
            f'Penguin<tspan fill="{BLUE}">CAD</tspan></text>')


def footer(w, h, n, total=6):
    y = h - 64
    return (brand(64, y + 10, 46)
            + text(w - 64, y + 4, f"{n:02d} / {total:02d}", 22, MONO, INK3, "end")
            + f'<path d="M64 {y - 52}H{w - 64}" stroke="{LINE}" stroke-width="1.5"/>')


def eyebrow(x, y, s):
    return text(x, y, s, 24, MONO + ";letter-spacing:.14em", ORANGE)


def headline(x, y, lines, size, fill=INK, gap=1.02):
    return "".join(text(x, y + i * size * gap, ln, size, DISPLAY + ";letter-spacing:-.02em", fill)
                   for i, ln in enumerate(lines))


def para(x, y, lines, size=30, fill=INK2, gap=1.45):
    return "".join(text(x, y + i * size * gap, ln, size, BODY, fill) for i, ln in enumerate(lines))


def pill(x, y, label, lit=False, dim=False, size=30):
    w = len(label) * size * 0.56 + 40
    h = size * 1.7
    fill = BLUE if lit else "#16202c"
    stroke = BLUE if lit else "#34485e"
    color = "#03121c" if lit else (INK3 if dim else INK)
    return (f'<rect x="{x - w / 2:.1f}" y="{y - h / 2:.1f}" width="{w:.1f}" height="{h:.1f}" rx="8" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="2"/>'
            + text(x, y + size * 0.35, label, size, "font-family:Archivo;font-weight:600", color, "middle"))


def cursor(x, y, s=1.6):
    return (f'<path transform="translate({x} {y}) scale({s})" d="M0 0v26l7-7 5 11 4.4-2-5-10.6h9.8z" '
            f'fill="{INK}" stroke="{BG}" stroke-width="1.6" stroke-linejoin="round"/>')


def timeline_icon(kind, cx, cy, color, opacity=1):
    """Sketch, extrude or fillet, drawn to match the app's own icons."""
    g = f'<g transform="translate({cx} {cy})" fill="none" stroke="{color}" stroke-width="3" stroke-linejoin="round" stroke-linecap="round" opacity="{opacity}">'
    if kind == "Sketch":
        body = ('<rect x="-18" y="-10" width="26" height="22" rx="2"/>'
                f'<path d="M4 8 18 -6 22 -2 8 12 2 13z" fill="{ORANGE}" stroke="{ORANGE}" stroke-width="1.5"/>')
    elif kind == "Extrude":
        body = ('<path d="M-18 10 0 1 18 10 0 19z"/>'
                f'<path d="M0 6V-16" stroke="{BLUE}"/><path d="M-7 -9 0 -18 7 -9z" fill="{BLUE}" stroke="{BLUE}" stroke-width="1.5"/>')
    else:
        body = ('<path d="M-16 16V-16H-2V2A6 6 0 0 0 4 8H16V16Z"/>'
                f'<path d="M-2 -4V2A6 6 0 0 0 4 8H10A12 12 0 0 1 -2 -4Z" fill="{BLUE}" stroke="none"/>')
    return g + body + "</g>"


def keycap(x, y, key, label, same=True, u=150):
    return (f'<rect x="{x}" y="{y + 10}" width="{u}" height="{u}" rx="20" fill="#070b10"/>'
            f'<rect x="{x}" y="{y}" width="{u}" height="{u}" rx="20" fill="#172230" stroke="{BLUE if same else "#34485e"}" stroke-width="2.5"/>'
            + text(x + 22, y + 62, key, 54, DISPLAY, INK)
            + text(x + 22, y + u - 26, label, 21, "font-family:Archivo;font-weight:600", INK2)
            + (f'<circle cx="{x + u - 26}" cy="{y + 26}" r="8" fill="{ORANGE}"/>' if same else ""))


# ---------------------------------------------------------------- images


def og():
    w, h = 1200, 630
    body = [ground(w, h),
            shadow(890, 440, 0.82),
            flange(890, 360, 0.82, glow=False),
            dim_h(677, 1103, 572, "Ø80.00", 22, 26),
            dim_v(1128, 263, 360, "d5", 22),
            brand(64, 112, 58),
            eyebrow(64, 200, "OPEN SOURCE · LINUX · HELP WANTED"),
            headline(64, 286, ["A Fusion-style", "CAD for Linux."], 64),
            para(64, 438, ["Early and free. Try it, break it,", "and help build it."], 27),
            text(64, 560, "kaiyao7572-creator.github.io/PenguinCAD", 22, MONO, BLUE)]
    return w, h, body


def shadow(cx, cy, s):
    return ell(cx, cy, 270 * s, fill="#000", fill_opacity=".35")


def tick(x, y, ok=True):
    """A drawn check mark, or an open orange circle for "not yet"."""
    if ok:
        return (f'<circle cx="{x}" cy="{y}" r="17" fill="#46c08a" fill-opacity=".16" stroke="#46c08a" stroke-width="2"/>'
                f'<path d="M{x - 8} {y} {x - 2} {y + 7} {x + 9} {y - 7}" fill="none" stroke="#46c08a" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/>')
    return f'<circle cx="{x}" cy="{y}" r="15" fill="none" stroke="{ORANGE}" stroke-width="2.6" stroke-dasharray="5 4"/>'


def checklist(x, y, items, ok=True, size=34, gap=86):
    out = []
    for i, item in enumerate(items):
        yy = y + i * gap
        out.append(tick(x + 17, yy - size * 0.32, ok))
        out.append(text(x + 58, yy, item, size, BODY, INK))
    return "".join(out)


TALK = "font-family:Archivo;font-stretch:112%;font-weight:800;letter-spacing:-.01em"


def talk(x, y, lines, size=78, fill=INK):
    return "".join(text(x, y + i * size * 1.1, ln, size, TALK, fill) for i, ln in enumerate(lines))


def post1():
    w, h = 1080, 1350
    return w, h, [ground(w, h),
                  eyebrow(80, 150, "OPEN SOURCE · LINUX"),
                  talk(80, 250, ["I'm building a", "Fusion-style CAD", "for Linux."], 80),
                  para(80, 540, ["It's free, it's early, and it's rough around", "the edges. I'd love some help with it."], 32),
                  shadow(540, 1000, 1.05),
                  flange(540, 900, 1.05, glow=False),
                  dim_h(267, 813, 1165, "Ø80.00", 28, 30),
                  dim_v(880, 776, 900, "18.00", 26),
                  footer(w, h, 1)]


def post2():
    w, h = 1080, 1350
    items = ["Sketches with constraints and dimensions",
             "Extrude, revolve, sweep and loft",
             "Fillet, chamfer, shell, patterns",
             "Parameters: type d3 * 1.8 into any box",
             "Save and open your designs",
             "STEP import and export"]
    return w, h, [ground(w, h),
                  eyebrow(80, 150, "TRY IT"),
                  talk(80, 250, ["Here's what", "already works."], 84),
                  checklist(80, 520, items, True, 34, 88),
                  para(80, 1100, ["Enough to model a real part.", "Not enough to replace your day job's CAD. Yet."], 30),
                  footer(w, h, 2)]


def post3():
    w, h = 1080, 1350
    cx, cy, rr = 330, 790, 190
    labels = ["Repeat", "Press Pull", "Redo", "Hole", "Sketch", "Move", "Undo", "Delete"]
    parts = [ground(w, h),
             eyebrow(80, 150, "IF YOU KNOW FUSION"),
             talk(80, 250, ["It works the way", "Fusion does."], 84),
             f'<circle cx="{cx}" cy="{cy}" r="{rr}" fill="none" stroke="{LINE}" stroke-width="2" stroke-dasharray="6 10"/>']
    for i, lab in enumerate(labels):
        a = i * math.pi / 4
        parts.append(pill(cx + rr * math.sin(a), cy - rr * math.cos(a), lab, lit=(i == 1), dim=lab in ("Hole", "Delete"), size=24))
    parts += [f'<circle cx="{cx}" cy="{cy}" r="34" fill="#16202c" stroke="#34485e" stroke-width="2.5"/>',
              f'<path d="M{cx + 26} {cy - 26} L{cx + 105} {cy - 105}" stroke="{BLUE}" stroke-width="6" stroke-linecap="round"/>',
              cursor(cx + 100, cy - 108, 1.8)]
    for i, (k, lab) in enumerate((("E", "Extrude"), ("Q", "Press Pull"), ("F", "Fillet"), ("L", "Line"))):
        y = 600 + i * 112
        parts += [f'<rect x="640" y="{y + 7}" width="86" height="86" rx="14" fill="#070b10"/>',
                  f'<rect x="640" y="{y}" width="86" height="86" rx="14" fill="#172230" stroke="{BLUE}" stroke-width="2"/>',
                  text(683, y + 58, k, 44, DISPLAY, INK, "middle"),
                  text(750, y + 56, lab, 32, BODY, INK2)]
    parts += [para(80, 1100, ["Right-click brings up the same ring of commands.", "If you learned on Fusion, you already know your way around."], 30),
              footer(w, h, 3)]
    return w, h, parts


def post4():
    w, h = 1080, 1350
    items = ["Assemblies and joints",
             "2D drawings",
             "A proper Hole tool",
             "Fillets on edges you pick",
             "A nicer timeline",
             "Getting it onto Flathub"]
    return w, h, [ground(w, h),
                  eyebrow(80, 150, "HELP WANTED"),
                  talk(80, 250, ["What's still", "missing."], 84),
                  checklist(80, 520, items, False, 34, 88),
                  para(80, 1100, ["Pick one and it's yours to build.", "Small fixes help a lot too. It's C++ and Qt."], 30),
                  footer(w, h, 4)]


def post5():
    w, h = 1080, 1350
    rows = [("Try it and tell me what breaks", "Every bug report makes it better."),
            ("Test it on your distro", "I can only check so many machines."),
            ("Draw icons, write a tutorial", "Design and docs count just as much."),
            ("Show it to a Fusion user on Linux", "That's how it finds its people.")]
    parts = [ground(w, h),
             eyebrow(80, 150, "NO CODE NEEDED"),
             talk(80, 250, ["You don't need", "to code to help."], 84)]
    for i, (head, sub) in enumerate(rows):
        y = 490 + i * 150
        parts += [f'<rect x="80" y="{y}" width="920" height="124" rx="16" fill="#111a25" stroke="{LINE}" stroke-width="2"/>',
                  text(116, y + 56, f"{i + 1}", 34, DISPLAY, ORANGE),
                  text(170, y + 56, head, 34, "font-family:Archivo;font-weight:700", INK),
                  text(170, y + 98, sub, 26, BODY, INK3)]
    parts += [footer(w, h, 5)]
    return w, h, parts


def post6():
    w, h = 1080, 1350
    tx, ty, tw, th = 80, 520, 920, 330
    parts = [ground(w, h),
             eyebrow(80, 150, "TRY IT TONIGHT"),
             talk(80, 250, ["One command", "installs it."], 84),
             f'<rect x="{tx}" y="{ty + 12}" width="{tw}" height="{th}" rx="22" fill="#05080c"/>',
             f'<rect x="{tx}" y="{ty}" width="{tw}" height="{th}" rx="22" fill="#0a1018" stroke="{LINE}" stroke-width="2"/>',
             f'<rect x="{tx}" y="{ty}" width="{tw}" height="60" rx="22" fill="#0d151e"/>',
             f'<rect x="{tx}" y="{ty + 38}" width="{tw}" height="22" fill="#0d151e"/>']
    for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        parts.append(f'<circle cx="{tx + 36 + i * 30}" cy="{ty + 30}" r="9" fill="{c}" opacity=".8"/>')
    parts += [text(tx + 40, ty + 128, "$ curl -fsSL \\", 34, MONO, INK),
              text(tx + 76, ty + 180, "kaiyao7572-creator.github.io/", 34, MONO, BLUE2),
              text(tx + 76, ty + 232, "PenguinCAD/install.sh | sh", 34, MONO, BLUE2),
              text(tx + 40, ty + 290, "==> Done. Find PenguinCAD in your apps.", 25, MONO, "#46c08a"),
              para(80, 940, ["Most Linux distros, free, and it updates itself.", "Then tell me what breaks, or what you'd add."], 30),
              text(80, 1100, "Link in bio  →", 32, "font-family:Archivo;font-stretch:112%;font-weight:700", ORANGE),
              footer(w, h, 6)]
    return w, h, parts


# ---------------------------------------------------------------- render


def render(name, w, h, parts, target):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
           + defs() + "\n".join(parts) + "</svg>")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.with_suffix(".svg").write_text(svg, encoding="utf-8")
    page = (f"<!doctype html><meta charset=utf-8><style>{font_face()}html,body{{margin:0;background:{BG}}}"
            f"svg{{display:block}}</style>{svg}")
    with tempfile.TemporaryDirectory() as tmp:
        html = Path(tmp) / f"{name}.html"
        html.write_text(page, encoding="utf-8")
        png = Path(tmp) / f"{name}.png"
        subprocess.run(["google-chrome", "--headless=new", "--hide-scrollbars", "--force-device-scale-factor=1",
                        f"--window-size={w},{h}", "--virtual-time-budget=1500", f"--screenshot={png}",
                        f"file://{html}"], check=True, capture_output=True)
        shutil.copy(png, target)
    print("wrote", target.relative_to(ROOT))


def main():
    # The Instagram posts come from casual.py now; post1..post6 here are the
    # earlier, polished set, kept for reference.
    w, h, p = og()
    render("og", w, h, p, OUT / "og.png")


if __name__ == "__main__":
    main()
