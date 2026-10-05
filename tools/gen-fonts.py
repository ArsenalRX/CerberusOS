#!/usr/bin/env python3
"""Renders the desktop's anti-aliased fonts into kernel/gfx/fonts.bin.

The kernel has no TrueType renderer yet (phase 13), so the glyphs are drawn
here, on the build machine, by FreeType (through Pillow) and stored as 8-bit
coverage: how much of each pixel the glyph covers. The kernel blends them.

The output is committed, so building Cerberus needs neither Pillow nor the
font files; run this only to change fonts or sizes:

    sudo apt install python3-pil fonts-dejavu-core
    python3 tools/gen-fonts.py

Fonts: DejaVu (free licence, https://dejavu-fonts.github.io/License.html).

File format (little-endian):
    "CFNT", u32 font_count
    per font, 40 bytes: char name[16]; u16 line_height, ascent, first, count;
                        u16 fixed_advance (0 = proportional), pad;
                        u32 glyphs_offset, coverage_offset, coverage_size
    glyph, 12 bytes:    u32 offset (into the font's coverage); u8 w, h;
                        i8 xoff; u8 yoff (from the top of the line);
                        u8 advance; u8 pad[3]
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont

DEJAVU = "/usr/share/fonts/truetype/dejavu/"
ASCII = [chr(c) for c in range(32, 127)]
LETTERS = [chr(c) for c in range(32, 127) if chr(c).isalpha() or chr(c) == " "]
# The large fonts also show the clock (lock screen): digits and a colon.
CLOCK = [chr(c) for c in range(32, 127) if chr(c).isalpha() or chr(c).isdigit() or chr(c) in " :"]

# name, file, pixel size, characters, forced cell (advance, line height) or None
FONTS = [
    ("ui", "DejaVuSans.ttf", 14, ASCII, None),
    ("ui-bold", "DejaVuSans-Bold.ttf", 14, ASCII, None),
    # 13.3 px gives an advance of 8.0: the terminal keeps its 8x16 cells.
    ("mono", "DejaVuSansMono.ttf", 13.3, ASCII, (8, 16)),
    ("display", "DejaVuSans-Bold.ttf", 64, CLOCK, None),
    ("display-big", "DejaVuSans-Bold.ttf", 104, CLOCK, None),
]

def render(name, path, size, chars, cell):
    font = ImageFont.truetype(os.path.join(DEJAVU, path), size)
    ascent, descent = font.getmetrics()
    line = cell[1] if cell else ascent + descent
    first, last = ord(chars[0]), ord(chars[-1])
    present = set(chars)
    glyphs, coverage = [], bytearray()
    pad = int(size) + 4
    for code in range(first, last + 1):
        ch = chr(code)
        if ch not in present:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        advance = cell[0] if cell else int(round(font.getlength(ch)))
        img = Image.new("L", (pad * 3, line + pad * 2), 0)
        ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=255)
        box = img.getbbox()
        if not box:
            glyphs.append((len(coverage), 0, 0, 0, 0, advance))
            continue
        x0, y0, x1, y1 = box
        # Keep the glyph inside its line vertically (a forced cell can clip a
        # pixel of a descender; better than drawing outside the row).
        y0, y1 = max(y0, pad), min(y1, pad + line)
        crop = img.crop((x0, y0, x1, y1))
        w, h = crop.size
        if w > 255 or h > 255 or advance > 255:
            sys.exit(f"{name}: glyph {ch!r} too large for the format")
        glyphs.append((len(coverage), w, h, x0 - pad, y0 - pad, advance))
        coverage += crop.tobytes()
    return {"name": name, "line": line, "ascent": ascent, "first": first, "count": last - first + 1,
            "fixed": cell[0] if cell else 0, "glyphs": glyphs, "coverage": bytes(coverage)}

def main():
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "gfx", "fonts.bin")
    fonts = [render(*f) for f in FONTS]
    header_size = 8 + 40 * len(fonts)
    body = bytearray()
    headers = bytearray(b"CFNT" + struct.pack("<I", len(fonts)))
    for f in fonts:
        glyphs_off = header_size + len(body)
        for off, w, h, xoff, yoff, adv in f["glyphs"]:
            body += struct.pack("<IBBbBB3x", off, w, h, xoff, yoff, adv)
        cov_off = header_size + len(body)
        body += f["coverage"]
        body += b"\0" * (-len(body) % 4)
        headers += struct.pack("<16sHHHHHHIII", f["name"].encode(), f["line"], f["ascent"], f["first"], f["count"],
                               f["fixed"], 0, glyphs_off, cov_off, len(f["coverage"]))
        print(f"{f['name']:12} line {f['line']:3} px, {f['count']:3} glyphs, {len(f['coverage']) / 1024:7.1f} KiB")
    with open(out_path, "wb") as fh:
        fh.write(headers + body)
    print(f"wrote {os.path.normpath(out_path)} ({(len(headers) + len(body)) / 1024:.0f} KiB)")

if __name__ == "__main__":
    main()
