#!/usr/bin/env python3
"""Draw the Instagram posts in a hand-made style and render them to PNG.

    python3 marketing/src/casual.py

Graph paper, handwriting, real screenshots taped on at an angle, and
marker scribbles over them, so the set reads like someone showing their
side project rather than an agency campaign. Writes
marketing/instagram/01..06.png (1080 x 1350) and the .svg next to each.
The link preview card comes from make.py.
"""

import base64
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "marketing" / "instagram"
IMG = ROOT / "website" / "src" / "img"
FONTS = Path(__file__).resolve().parent / "fonts"
SITE_FONTS = ROOT / "website" / "src" / "fonts"

W, H = 1080, 1350
PAPER = "#f7f3ea"
PEN = "#1e2a36"
BLUEPEN = "#1f67a8"
MARKER = "#f07818"
GREEN = "#2f8f5b"

HAND = "font-family:Caveat;font-weight:700"
HAND_LIGHT = "font-family:Caveat;font-weight:500"
NOTE = "font-family:'Patrick Hand'"
MONO = "font-family:'B612 Mono'"


def b64(path):
    return base64.b64encode(Path(path).read_bytes()).decode()


def font_face():
    return (f"@font-face{{font-family:Caveat;font-weight:400 700;src:url(data:font/woff2;base64,{b64(FONTS / 'caveat.woff2')})}}"
            f"@font-face{{font-family:'Patrick Hand';src:url(data:font/woff2;base64,{b64(FONTS / 'patrickhand.woff2')})}}"
            f"@font-face{{font-family:'B612 Mono';src:url(data:font/woff2;base64,{b64(SITE_FONTS / 'b612mono.woff2')})}}")


def defs():
    return f"""<defs>
  <pattern id="grid" width="36" height="36" patternUnits="userSpaceOnUse">
    <path d="M36 0H0V36" fill="none" stroke="#9fc0dc" stroke-opacity=".38" stroke-width="1.2"/>
  </pattern>
  <filter id="rough" x="-10%" y="-10%" width="120%" height="120%">
    <feTurbulence type="fractalNoise" baseFrequency="0.035" numOctaves="2" seed="7"/>
    <feDisplacementMap in="SourceGraphic" scale="7"/>
  </filter>
  <filter id="shadow" x="-10%" y="-10%" width="130%" height="130%">
    <feDropShadow dx="4" dy="10" stdDeviation="10" flood-color="#3a2f1e" flood-opacity=".28"/>
  </filter>
  <symbol id="penguin" viewBox="0 0 64 64">
    <path d="M30.5 7.5c9.8 0 15.9 7.6 16.6 17.2.5 7 3.4 11.4 3.4 18.3 0 9.6-7.9 15.5-18.5 15.5S13.6 52.4 13.6 42.4c0-6.2 2.6-10.6 2.6-17.4C16.2 14.9 21.6 7.5 30.5 7.5z" fill="#0E1722"/>
    <path d="M33.8 14.6c5.6.4 8.6 4.6 9 10.2.4 6.2 3.4 10 3.4 17 0 7.4-5.6 11.9-13.3 11.9-7.4 0-12.4-4.4-12.4-11.3 0-6.5 3.6-9.8 4.4-15.6.9-7.3 3.4-12.6 8.9-12.2z" fill="#F4F7FA"/>
    <path d="M40.6 20.2 52.8 23.6 40.9 26.9c-1.4-2.2-1.5-4.4-.3-6.7z" fill="#FF9F43"/>
    <circle cx="35.6" cy="20.4" r="2.3" fill="#0E1722"/>
    <circle cx="36.3" cy="19.7" r=".75" fill="#F4F7FA"/>
    <path d="M22 58.6h19.5" stroke="#FF9F43" stroke-width="3.2" stroke-linecap="round"/>
  </symbol>
</defs>"""


def paper():
    return (f'<rect width="{W}" height="{H}" fill="{PAPER}"/>'
            f'<rect width="{W}" height="{H}" fill="url(#grid)"/>'
            f'<path d="M104 0V{H}" stroke="#e39a9a" stroke-width="2.5" stroke-opacity=".7"/>')


def t(x, y, s, size, style=NOTE, fill=PEN, rot=0, anchor="start"):
    tr = f' transform="rotate({rot} {x} {y})"' if rot else ""
    return (f'<text x="{x}" y="{y}" font-size="{size}" style="{style}" fill="{fill}" '
            f'text-anchor="{anchor}"{tr}>{s}</text>')


def lines(x, y, rows, size, style=NOTE, fill=PEN, gap=1.25, rot=0):
    return "".join(t(x, y + i * size * gap, r, size, style, fill, rot) for i, r in enumerate(rows))


def tape(x, y, w=120, rot=-8):
    return (f'<rect x="{x - w / 2}" y="{y - 20}" width="{w}" height="40" fill="#efe2b8" fill-opacity=".78" '
            f'stroke="#d6c48f" stroke-opacity=".5" transform="rotate({rot} {x} {y})"/>')


def photo(img, x, y, w, rot, crop=None, tapes=True):
    """A screenshot printed with a white border and taped to the page.
    crop is (cx, cy, cw, ch) in the 1800 x 1096 image."""
    cx, cy, cw, ch = crop or (0, 0, 1800, 1096)
    h = w * ch / cw
    pad = 16
    mid_x, mid_y = x + w / 2, y + h / 2
    href = f"data:image/webp;base64,{b64(IMG / img)}"
    out = [f'<g transform="rotate({rot} {mid_x} {mid_y})">',
           f'<rect x="{x - pad}" y="{y - pad}" width="{w + 2 * pad}" height="{h + 2 * pad}" fill="#fff" filter="url(#shadow)"/>',
           f'<svg x="{x}" y="{y}" width="{w}" height="{h}" viewBox="{cx} {cy} {cw} {ch}" preserveAspectRatio="xMidYMid slice">'
           f'<image href="{href}" width="1800" height="1096"/></svg>']
    if tapes:
        out += [tape(x + 30, y - 6, 130, -14), tape(x + w - 30, y - 6, 130, 12)]
    out.append("</g>")
    return "".join(out)


def scribble_circle(cx, cy, rx, ry, color=MARKER, rot=-6):
    return (f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}" fill="none" stroke="{color}" stroke-width="7" '
            f'stroke-linecap="round" filter="url(#rough)" transform="rotate({rot} {cx} {cy})"/>')


def arrow(d, tip, color=MARKER, width=6):
    """A hand-drawn arrow: the stroke d, and a two-line head at tip=(x, y, angle_deg)."""
    import math
    x, y, a = tip
    a = math.radians(a)
    l = 30
    p1 = (x - l * math.cos(a - 0.5), y - l * math.sin(a - 0.5))
    p2 = (x - l * math.cos(a + 0.5), y - l * math.sin(a + 0.5))
    return (f'<g fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round" filter="url(#rough)">'
            f'<path d="{d}"/><path d="M{p1[0]:.0f} {p1[1]:.0f} L{x} {y} L{p2[0]:.0f} {p2[1]:.0f}"/></g>')


def box(x, y, checked, size=40):
    out = (f'<rect x="{x}" y="{y}" width="{size}" height="{size}" rx="4" fill="none" stroke="{PEN}" '
           f'stroke-width="3.5" filter="url(#rough)"/>')
    if checked:
        out += (f'<path d="M{x + 6} {y + size * 0.5} L{x + size * 0.42} {y + size - 6} L{x + size + 12} {y - 14}" '
                f'fill="none" stroke="{GREEN}" stroke-width="7" stroke-linecap="round" stroke-linejoin="round" filter="url(#rough)"/>')
    return out


def checklist(x, y, items, checked, size=44, gap=86):
    out = []
    for i, item in enumerate(items):
        yy = y + i * gap
        out.append(box(x, yy - size * 0.78, checked, 40))
        out.append(t(x + 66, yy, item, size))
    return "".join(out)


def underline(x1, x2, y, color=MARKER):
    return (f'<path d="M{x1} {y} C{x1 + (x2 - x1) * 0.3} {y + 6} {x1 + (x2 - x1) * 0.7} {y - 5} {x2} {y + 3}" '
            f'fill="none" stroke="{color}" stroke-width="8" stroke-linecap="round" stroke-opacity=".85" filter="url(#rough)"/>')


def sticker(x, y, size=150, rot=-10):
    c = size / 2
    return (f'<g transform="rotate({rot} {x + c} {y + c})" filter="url(#shadow)">'
            f'<circle cx="{x + c}" cy="{y + c}" r="{c}" fill="#fff"/>'
            f'<circle cx="{x + c}" cy="{y + c}" r="{c - 8}" fill="#2aa7e8"/>'
            f'<use href="#penguin" x="{x + size * 0.14}" y="{y + size * 0.12}" width="{size * 0.72}" height="{size * 0.72}"/></g>')


def footer(n):
    return (t(140, H - 60, "penguincad", 44, HAND, PEN)
            + t(W - 70, H - 60, f"{n}/6", 44, HAND_LIGHT, "#8a8171", anchor="end"))


# ---------------------------------------------------------------- posts


def post1():
    return [paper(),
            lines(140, 170, ["i'm making a free", "CAD app for linux"], 104, HAND, PEN, 0.95),
            underline(140, 700, 300),
            lines(140, 390, ["it works like fusion 360: the timeline,", "the right-click menu, the same shortcuts."], 40, NOTE, BLUEPEN, 1.3),
            photo("app.webp", 150, 560, 800, -2.5),
            scribble_circle(560, 800, 150, 90, MARKER, -8),
            arrow("M820 1185 C 800 1120, 740 1050, 690 900", (690, 900, -110)),
            t(470, 1235, "a little part i made in it", 50, HAND_LIGHT, MARKER, -3),
            sticker(900, 60, 130, 12),
            footer(1)]


def post2():
    items = ["sketching, with constraints", "extrude, revolve, sweep, loft", "fillets, chamfers, patterns",
             "parameters (type d3 * 1.8 anywhere!)", "saving + opening your designs", "STEP import and export"]
    return [paper(),
            t(140, 190, "what works so far:", 100, HAND, PEN),
            underline(140, 760, 222),
            checklist(150, 360, items, True, 44, 92),
            photo("sketch.webp", 560, 930, 420, 4, (320, 180, 1260, 760)),
            t(200, 1115, "sketching :)", 54, HAND_LIGHT, BLUEPEN, -4),
            arrow("M470 1105 C 510 1110, 540 1090, 560 1060", (563, 1057, -50), BLUEPEN, 5),
            footer(2)]


def post3():
    return [paper(),
            lines(140, 180, ["right click =", "the fusion menu"], 104, HAND, PEN, 0.95),
            photo("marking-menu.webp", 170, 420, 740, 2, (700, 390, 580, 470)),
            scribble_circle(538, 725, 330, 250, MARKER, 4),
            lines(160, 1150, ["click it, flick it or hold it,", "same muscle memory"], 46, NOTE, BLUEPEN, 1.3),
            arrow("M720 1160 C 820 1150, 900 1100, 930 1010", (934, 1004, -70)),
            footer(3)]


def post4():
    items = ["assemblies + joints", "2D drawings", "a hole tool", "picking which edges to fillet",
             "a nicer timeline", "getting it on flathub"]
    return [paper(),
            lines(140, 180, ["stuff i still need", "help with:"], 100, HAND, PEN, 0.95),
            underline(140, 470, 290),
            checklist(150, 420, items, False, 44, 92),
            t(640, 1080, "pick one!!", 90, HAND, MARKER, -6),
            arrow("M650 1030 C 600 980, 560 960, 520 930", (516, 926, -140)),
            lines(160, 1170, ["it's C++ and Qt. small fixes help a ton too."], 38, NOTE, BLUEPEN),
            footer(4)]


def post5():
    rows = ["try it + tell me what breaks", "test it on your distro", "make icons or a tutorial",
            "send it to a friend who uses fusion"]
    out = [paper(),
           lines(140, 180, ["you don't have to", "code to help!"], 104, HAND, PEN, 0.95),
           underline(140, 620, 290)]
    for i, r in enumerate(rows):
        y = 470 + i * 150
        out += [t(150, y, f"{i + 1}.", 72, HAND, MARKER), t(230, y - 4, r, 48)]
    out += [lines(150, 1120, ["(there's a donate button too,", "if you want to chip in)"], 40, NOTE, "#8a8171", 1.3),
            sticker(820, 1030, 150, -12),
            footer(5)]
    return out


def post6():
    return [paper(),
            t(140, 200, "try it tonight:", 110, HAND, PEN),
            underline(140, 720, 232),
            f'<g transform="rotate(-2 540 520)" filter="url(#shadow)">'
            f'<rect x="120" y="400" width="840" height="240" rx="6" fill="#16202b"/>'
            + t(160, 470, "$ curl -fsSL \\", 36, MONO, "#e6eef5")
            + t(190, 530, "kaiyao7572-creator.github.io/", 36, MONO, "#7cc6ea")
            + t(190, 590, "PenguinCAD/install.sh | sh", 36, MONO, "#7cc6ea")
            + "</g>",
            tape(170, 400, 140, -18), tape(910, 395, 140, 14),
            lines(150, 780, ["works on most linux distros,", "and it updates itself."], 46, NOTE, BLUEPEN, 1.3),
            t(150, 1000, "then tell me what breaks!!", 76, HAND, MARKER, -3),
            t(640, 1150, "link in bio", 72, HAND, PEN, 2),
            arrow("M600 1135 C 560 1110, 540 1080, 540 1050", (540, 1046, -90), PEN, 5),
            sticker(880, 90, 130, 10),
            footer(6)]


# ---------------------------------------------------------------- render


def render(name, parts, target):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
           + defs() + "\n".join(parts) + "</svg>")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.with_suffix(".svg").write_text(svg, encoding="utf-8")
    page = (f"<!doctype html><meta charset=utf-8><style>{font_face()}html,body{{margin:0;background:{PAPER}}}"
            f"svg{{display:block}}</style>{svg}")
    with tempfile.TemporaryDirectory() as tmp:
        html = Path(tmp) / f"{name}.html"
        html.write_text(page, encoding="utf-8")
        png = Path(tmp) / f"{name}.png"
        subprocess.run(["google-chrome", "--headless=new", "--hide-scrollbars", "--force-device-scale-factor=1",
                        f"--window-size={W},{H}", "--virtual-time-budget=2000", f"--screenshot={png}",
                        f"file://{html}"], check=True, capture_output=True)
        shutil.copy(png, target)
    print("wrote", target.relative_to(ROOT))


def main():
    for i, fn in enumerate((post1, post2, post3, post4, post5, post6), 1):
        render(f"post{i}", fn(), OUT / f"{i:02d}.png")


if __name__ == "__main__":
    main()
