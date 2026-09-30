#!/usr/bin/env python3
"""Draw the Instagram posts: white, plain, screenshots first.

    python3 marketing/src/plain.py

Each post is a short sentence in ordinary type and a real screenshot of
the app, the way someone posts about their own project. Writes
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
FONTS = ROOT / "website" / "src" / "fonts"

W, H = 1080, 1350
BG = "#ffffff"
INK = "#111418"
GREY = "#6b7280"
LIGHT = "#eef0f3"
BLUE = "#0a7cc2"

HEAD = "font-family:Archivo;font-stretch:100%;font-weight:700;letter-spacing:-.01em"
BODY = "font-family:Archivo;font-stretch:100%;font-weight:400"
MONO = "font-family:'B612 Mono'"
X = 90  # the left margin everything hangs from


def b64(path):
    return base64.b64encode(Path(path).read_bytes()).decode()


def font_face():
    return (f"@font-face{{font-family:Archivo;font-weight:400 800;font-stretch:100% 125%;"
            f"src:url(data:font/woff2;base64,{b64(FONTS / 'archivo.woff2')})}}"
            f"@font-face{{font-family:'B612 Mono';src:url(data:font/woff2;base64,{b64(FONTS / 'b612mono.woff2')})}}")


def t(x, y, s, size, style=BODY, fill=INK, anchor="start"):
    return f'<text x="{x}" y="{y}" font-size="{size}" style="{style}" fill="{fill}" text-anchor="{anchor}">{s}</text>'


def lines(y, rows, size, style=BODY, fill=INK, gap=1.3, x=X):
    return "".join(t(x, y + i * size * gap, r, size, style, fill) for i, r in enumerate(rows))


def shot(img, y, w=W - 2 * X, crop=None, x=X):
    """A screenshot, straight, with a hairline border and rounded corners.
    crop is (cx, cy, cw, ch) in the 1800 x 1096 image."""
    cx, cy, cw, ch = crop or (0, 0, 1800, 1096)
    h = w * ch / cw
    href = f"data:image/webp;base64,{b64(IMG / img)}"
    clip = f"clip{abs(hash((img, y))) % 10**6}"
    return (f'<clipPath id="{clip}"><rect x="{x}" y="{y}" width="{w}" height="{h:.0f}" rx="14"/></clipPath>'
            f'<g clip-path="url(#{clip})"><svg x="{x}" y="{y}" width="{w}" height="{h:.0f}" viewBox="{cx} {cy} {cw} {ch}" '
            f'preserveAspectRatio="xMidYMid slice"><image href="{href}" width="1800" height="1096"/></svg></g>'
            f'<rect x="{x}" y="{y}" width="{w}" height="{h:.0f}" rx="14" fill="none" stroke="#d5d9df" stroke-width="2"/>')


def plain_list(y, rows, size=40, gap=76, mark="•", mark_fill=GREY):
    out = []
    for i, r in enumerate(rows):
        yy = y + i * gap
        out.append(t(X, yy, mark, size, BODY, mark_fill))
        out.append(t(X + 44, yy, r, size))
    return "".join(out)


def footer(n):
    return (t(X, H - 70, "PenguinCAD", 30, HEAD, INK)
            + t(W - X, H - 70, f"{n} / 6", 28, BODY, GREY, "end"))


def post1():
    return [t(X, 170, "I'm making a free CAD app", 62, HEAD),
            t(X, 245, "for Linux.", 62, HEAD),
            lines(330, ["It works like Fusion 360: the timeline,", "the right-click menu, the shortcuts."], 34, BODY, GREY),
            shot("app.webp", 470),
            t(X, 1080, "This is it right now: sketched, extruded, filleted.", 32, BODY, GREY),
            t(X, 1150, "Linux for now. Windows and Mac should be doable", 32, BODY, INK),
            t(X, 1192, "too, and I'd love help getting it there.", 32, BODY, INK),
            footer(1)]


def post2():
    rows = ["Sketches with constraints and dimensions", "Extrude, revolve, sweep, loft",
            "Fillet, chamfer, shell, patterns", "Parameters (type d3 * 1.8 in any box)",
            "Saving and opening designs", "STEP import and export"]
    return [t(X, 170, "What works so far", 62, HEAD),
            plain_list(290, rows, 36, 68, "✓", "#1f8a52"),
            shot("sketch.webp", 730, crop=(300, 160, 1300, 720)),
            footer(2)]


def post3():
    return [t(X, 170, "Right-click gives you the", 62, HEAD),
            t(X, 245, "same menu as Fusion.", 62, HEAD),
            shot("marking-menu.webp", 340, crop=(700, 400, 580, 440)),
            lines(1080, ["Click it, flick toward a command, or hold.", "Your hands already know where Undo is."], 34, BODY, GREY),
            footer(3)]


def post4():
    rows = ["Assemblies and joints", "2D drawings", "A Hole tool", "Choosing which edges to fillet",
            "A better timeline", "Getting it onto Flathub"]
    return [t(X, 170, "What I still need help with", 62, HEAD),
            lines(260, ["It's just me right now. If one of these sounds", "fun, it's yours."], 34, BODY, GREY),
            plain_list(440, rows, 40, 84),
            lines(1020, ["It's written in C++ with Qt. Small fixes", "help a lot too."], 34, BODY, GREY),
            footer(4)]


def post5():
    rows = ["Try it and tell me what breaks", "Test it on your Linux distro", "Make icons or write a tutorial",
            "Show it to a friend who uses Fusion"]
    out = [t(X, 170, "You don't need to code", 62, HEAD),
           t(X, 245, "to help.", 62, HEAD)]
    for i, r in enumerate(rows):
        y = 400 + i * 120
        out += [t(X, y, f"{i + 1}.", 44, HEAD, GREY), t(X + 64, y, r, 44)]
    out += [lines(1010, ["Even a quick \"this crashed on my laptop\"", "helps more than you'd think."], 34, BODY, GREY),
            footer(5)]
    return out


def post6():
    return [t(X, 170, "Try it", 62, HEAD),
            lines(250, ["One command installs it on most Linux distros,", "and it keeps itself updated."], 34, BODY, GREY),
            f'<rect x="{X}" y="370" width="{W - 2 * X}" height="230" rx="14" fill="{LIGHT}"/>',
            t(X + 36, 440, "curl -fsSL \\", 34, MONO, INK),
            t(X + 36, 500, "kaiyao7572-creator.github.io/", 34, MONO, BLUE),
            t(X + 36, 560, "PenguinCAD/install.sh | sh", 34, MONO, BLUE),
            lines(710, ["Then tell me what breaks, or what you'd", "want it to do next."], 40, BODY, INK),
            t(X, 900, "Link's in my bio.", 40, HEAD),
            shot("app.webp", 945, crop=(420, 330, 1200, 340)),
            footer(6)]


def render(name, parts, target):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
           f'<rect width="{W}" height="{H}" fill="{BG}"/>' + "\n".join(parts) + "</svg>")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.with_suffix(".svg").write_text(svg, encoding="utf-8")
    page = (f"<!doctype html><meta charset=utf-8><style>{font_face()}html,body{{margin:0;background:{BG}}}"
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
