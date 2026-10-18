#!/usr/bin/env python3
"""Generate the RecBean 16 px Chinese LVGL font (GB2312 + ASCII + UI symbols).

Steps (reproducible; see assets/README.md):
  1. Build the character inventory assets/fonts/recbean_font_16_symbols.txt:
     every printable GB2312 character (symbol rows 0xA1-0xA9, hanzi rows 0xB0-0xF7)
     plus the non-ASCII characters used by the firmware UI strings in main/*.c
     plus a few common punctuation marks that ASR output uses but GB2312 lacks.
  2. Run lv_font_conv 1.5.3 on Noto Sans SC Regular (SIL OFL 1.1).

Usage:
  python3 tools/gen_recbean_font.py --symbols-only
  python3 tools/gen_recbean_font.py --font NotoSansSC-Regular.otf --lv-font-conv <path>
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOLS = ROOT / "assets" / "fonts" / "recbean_font_16_symbols.txt"
OUTPUT = ROOT / "assets" / "fonts" / "recbean_font_16.c"
# Punctuation outside GB2312 that speech recognizers commonly emit.
EXTRA = "—…·‘’“”°€™®©～"


def gb2312_chars() -> list[str]:
    chars: list[str] = []
    for hi in list(range(0xA1, 0xAA)) + list(range(0xB0, 0xF8)):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes((hi, lo)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if ch.isprintable() and not ch.isspace():
                chars.append(ch)
    return chars


def ui_chars() -> set[str]:
    """Non-ASCII characters appearing in C string literals of main/*.c."""
    found: set[str] = set()
    literal = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for path in sorted((ROOT / "main").glob("*.c")):
        text = path.read_text(encoding="utf-8")
        # Drop // and /* */ comments so only displayable literals count.
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for m in literal.finditer(text):
            for ch in m.group(1):
                if ord(ch) > 0x7E:
                    found.add(ch)
    return found


def inventory() -> str:
    seen: dict[str, None] = {}
    for ch in gb2312_chars():
        seen.setdefault(ch, None)
    for ch in EXTRA:
        seen.setdefault(ch, None)
    for ch in sorted(ui_chars()):
        seen.setdefault(ch, None)
    return "".join(seen)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="path to NotoSansSC-Regular.otf")
    ap.add_argument("--lv-font-conv", default="lv_font_conv",
                    help="lv_font_conv executable, or path to lv_font_conv.js")
    ap.add_argument("--symbols-only", action="store_true")
    args = ap.parse_args()

    chars = inventory()
    SYMBOLS.parent.mkdir(parents=True, exist_ok=True)
    SYMBOLS.write_text(chars + "\n", encoding="utf-8", newline="\n")
    print(f"{SYMBOLS.relative_to(ROOT)}: {len(chars)} non-ASCII characters")
    if args.symbols_only:
        return 0
    if not args.font:
        ap.error("--font is required unless --symbols-only")
    # A .js path is run with node directly (avoids cmd.exe mangling the long CJK argument
    # on Windows); otherwise the given executable is used.
    conv = [args.lv_font_conv]
    if args.lv_font_conv.endswith(".js"):
        conv = ["node", args.lv_font_conv]
    cmd = [
        *conv,
        "--font", args.font,
        "--range", "0x20-0x7E",
        "--symbols", chars,
        "--size", "16", "--bpp", "4", "--format", "lvgl", "--no-compress", "--no-kerning",
        "--lv-font-name", "recbean_font_16", "--lv-include", "lvgl.h",
        "--output", str(OUTPUT),
    ]
    subprocess.run(cmd, check=True)
    # The converter records its full command line in the header; keep the committed file free of
    # developer-specific absolute paths.
    text = OUTPUT.read_text(encoding="utf-8")
    text = text.replace(args.font, Path(args.font).name).replace(str(OUTPUT), "assets/fonts/recbean_font_16.c")
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUTPUT.relative_to(ROOT)} written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
