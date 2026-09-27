#!/usr/bin/env python3
"""Build website/index.html: one self-contained page.

website/src/page.html is the source. Every image and font it references by
a relative path (src="img/...", href="img/...", url("fonts/...")) is inlined
as a data URI, so the result opens from disk, mails as one attachment and
serves from GitHub Pages without a single other request.

    python3 website/build.py
"""

import base64
import mimetypes
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
OUT = HERE / "index.html"

TYPES = {".webp": "image/webp", ".svg": "image/svg+xml", ".png": "image/png", ".woff2": "font/woff2"}


def data_uri(relative: str) -> str:
    path = SRC / relative
    if not path.is_file():
        sys.exit(f"build.py: {relative} is referenced by page.html but does not exist")
    kind = TYPES.get(path.suffix) or mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    return f"data:{kind};base64,{base64.b64encode(path.read_bytes()).decode('ascii')}"


def main() -> None:
    page = (SRC / "page.html").read_text(encoding="utf-8")
    page = re.sub(r'(src|href)="((?:img|fonts)/[^"]+)"', lambda m: f'{m[1]}="{data_uri(m[2])}"', page)
    page = re.sub(r'url\("((?:img|fonts)/[^"]+)"\)', lambda m: f'url("{data_uri(m[1])}")', page)
    left = re.findall(r'(?:src|href)="(?:img|fonts)/[^"]+"|url\("(?:img|fonts)/', page)
    if left:
        sys.exit(f"build.py: references left un-inlined: {left}")
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT.relative_to(HERE.parent)} ({OUT.stat().st_size / 1024:.0f} KiB)")


if __name__ == "__main__":
    main()
