#!/usr/bin/env python3
"""Glyph-coverage contract for the RecBean 16 px Chinese font (Chinese-font checklist).

Checks, by Unicode code point (not UTF-8 bytes):
  * every non-ASCII character used in main/*.c string literals is in the generated font;
  * every printable GB2312 character (the declared dynamic-text range) is in the font;
  * printable ASCII is in the font;
  * the committed symbol inventory matches what tools/gen_recbean_font.py would produce
    (so a UI text change without regenerating the font fails here);
  * a known out-of-range character (U+9F98) is absent, so the check cannot pass vacuously.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_C = ROOT / "assets" / "fonts" / "recbean_font_16.c"
SYMBOLS = ROOT / "assets" / "fonts" / "recbean_font_16_symbols.txt"


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_recbean_font", ROOT / "tools" / "gen_recbean_font.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def font_codepoints() -> set[int]:
    text = FONT_C.read_text(encoding="utf-8")
    return {int(m.group(1), 16) for m in re.finditer(r"/\* U\+([0-9A-F]{4,6}) ", text)}


class RecbeanFontTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gen = load_generator()
        cls.cps = font_codepoints()

    def missing(self, chars) -> list[str]:
        return [f"U+{ord(c):04X}" for c in sorted(set(chars)) if ord(c) not in self.cps]

    def test_ui_strings_covered(self) -> None:
        ui = self.gen.ui_chars()
        self.assertGreater(len(ui), 50)
        self.assertEqual(self.missing(ui), [])

    def test_gb2312_covered(self) -> None:
        chars = self.gen.gb2312_chars()
        self.assertGreater(len(chars), 7000)
        self.assertEqual(self.missing(chars), [])

    def test_ascii_covered(self) -> None:
        self.assertEqual(self.missing(chr(c) for c in range(0x20, 0x7F)), [])

    def test_inventory_in_sync(self) -> None:
        committed = SYMBOLS.read_text(encoding="utf-8").rstrip("\n")
        self.assertEqual(committed, self.gen.inventory(),
                         "UI text changed: rerun tools/gen_recbean_font.py (see assets/README.md)")

    def test_known_missing_glyph(self) -> None:
        self.assertNotIn(0x9F98, self.cps)


if __name__ == "__main__":
    unittest.main()
