#!/usr/bin/env python3
"""Convert a PSF version 1 console font (optionally gzipped) to PSF version 2.

PSF1 glyphs are always 8 pixels wide, 1 byte per row; PSF2 stores the same
bitmap rows with an explicit header. The Unicode table, if present, is
re-encoded from UCS-2 pairs to PSF2's UTF-8 form.

Usage: psf1to2.py <in.psf[.gz]> <out.psf>
"""
import gzip, struct, sys

PSF1_MAGIC = b"\x36\x04"
PSF2_MAGIC = b"\x72\xb5\x4a\x86"
PSF1_MODE512, PSF1_MODEHASTAB = 0x01, 0x02
PSF2_HAS_UNICODE_TABLE = 0x01

def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if data[:4] == PSF2_MAGIC:
        open(dst, "wb").write(data); print("already PSF2"); return
    if data[:2] != PSF1_MAGIC:
        sys.exit("not a PSF font")
    mode, height = data[2], data[3]
    nglyphs = 512 if mode & PSF1_MODE512 else 256
    glyphs = data[4:4 + nglyphs * height]
    rest = data[4 + nglyphs * height:]

    flags = 0
    table = b""
    if mode & PSF1_MODEHASTAB and rest:
        flags = PSF2_HAS_UNICODE_TABLE
        pos = 0
        for _ in range(nglyphs):
            out = bytearray()
            while pos + 2 <= len(rest):
                cp = struct.unpack_from("<H", rest, pos)[0]; pos += 2
                if cp == 0xFFFF:
                    break
                if cp == 0xFFFE:
                    out.append(0xFE)          # start of a sequence
                else:
                    out += chr(cp).encode("utf-8")
            out.append(0xFF)                  # end of this glyph's entries
            table += bytes(out)

    header = struct.pack("<IIIIIIII", 0x864AB572, 0, 32, flags, nglyphs, height, height, 8)
    open(dst, "wb").write(header + glyphs + table)
    print(f"PSF2: {nglyphs} glyphs, 8x{height}, unicode table: {bool(flags)}")

if __name__ == "__main__":
    main()
