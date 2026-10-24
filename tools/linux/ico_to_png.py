#!/usr/bin/env python3
"""The largest PNG inside a Windows .ico, written as a .png (the AppImage's icon; no ImageMagick needed).

    python3 ico_to_png.py windows/resources/BlueWake.ico OUT.png
"""
import struct
import sys


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    data = open(argv[0], "rb").read()
    _, _, count = struct.unpack("<HHH", data[:6])
    best = None
    for i in range(count):
        width, height, _c, _r, _planes, _bits, size, offset = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
        if data[offset:offset + 8] == b"\x89PNG\r\n\x1a\n" and (best is None or (width or 256) > best[0]):
            best = ((width or 256), data[offset:offset + size])
    if best is None:
        print("ico_to_png: no PNG image in the icon", file=sys.stderr)
        return 1
    open(argv[1], "wb").write(best[1])
    print(f"ico_to_png: {best[0]}x{best[0]} -> {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
