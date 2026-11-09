#!/usr/bin/env python3
"""Makes an .icns that Mac OS X 10.3 and 10.4 can read: the classic 16, 32, 48
and 128 pixel RGB images (RLE packed) with their 8-bit masks. Today's .icns
files hold PNGs, which those Finders do not understand.

    scripts/make-legacy-icns.py resources/AppIcon.icns resources/AppIcon-legacy.icns
(the input may be any image macOS's sips or Pillow can read)
"""
import io
import struct
import subprocess
import sys
import tempfile

from PIL import Image


def rle(channel: bytes) -> bytes:
    out = bytearray()
    i, n = 0, len(channel)
    while i < n:
        run = 1
        while i + run < n and run < 130 and channel[i + run] == channel[i]:
            run += 1
        if run >= 3:
            out += bytes([run + 125, channel[i]])
            i += run
            continue
        lit = bytearray()
        while i < n and len(lit) < 128:
            if i + 2 < n and channel[i] == channel[i + 1] == channel[i + 2]:
                break
            lit.append(channel[i])
            i += 1
        out += bytes([len(lit) - 1]) + lit
    return bytes(out)


def load(path: str) -> Image.Image:
    try:
        return Image.open(path).convert("RGBA")
    except Exception:
        with tempfile.NamedTemporaryFile(suffix=".png") as t:
            subprocess.run(["sips", "-s", "format", "png", path, "--out", t.name], check=True,
                           stdout=subprocess.DEVNULL)
            return Image.open(io.BytesIO(open(t.name, "rb").read())).convert("RGBA")


def main() -> None:
    src = load(sys.argv[1])
    chunks = []
    for size, rgb_type, mask_type in ((16, b"is32", b"s8mk"), (32, b"il32", b"l8mk"), (48, b"ih32", b"h8mk"),
                                      (128, b"it32", b"t8mk")):
        img = src.resize((size, size), Image.LANCZOS)
        r, g, b, a = img.split()
        data = (b"\0\0\0\0" if rgb_type == b"it32" else b"") + rle(r.tobytes()) + rle(g.tobytes()) + rle(b.tobytes())
        chunks.append(rgb_type + struct.pack(">I", 8 + len(data)) + data)
        chunks.append(mask_type + struct.pack(">I", 8 + size * size) + a.tobytes())
    body = b"".join(chunks)
    with open(sys.argv[2], "wb") as f:
        f.write(b"icns" + struct.pack(">I", 8 + len(body)) + body)


if __name__ == "__main__":
    main()
