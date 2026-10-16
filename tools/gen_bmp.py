#!/usr/bin/env python3
"""Generate avmcv710-bmp.h from the official driver's overlay bitmaps.

The AVerMedia Windows/macOS drivers ship four 640x480x24 overlay bitmaps
(no-signal, out-of-range, HDCP and content-protection) that they composite
into the capture stream when the input is unavailable, unsupported or
encrypted.  This tool converts them to the YUY2 raster the FPGA delivers
(640x480, 2 bytes per pixel, top-down) and emits a C header so the kernel
driver can blit them with the same behavior: show the overlay instead of a
frozen or black frame.

Source of the bitmaps
---------------------
Two renditions of the same four overlays exist in the tree:

  * cv710userspace/assets/  - the macOS RECentral rendition (the AVerMedia
    logo is composited into the panel; a tighter dialog).  This is the
    default, because the vendor's macOS presentation is the branded one and
    it lives in-tree (no dependency on the extracted installer).
  * driver_extracted/win/   - the Windows installer's copy: the same text in
    a wider, logo-less panel.

Both are 640x480x24 and were produced by the vendor; either can be built in
by passing the directory as the first argument:
    python3 tools/gen_bmp.py driver_extracted/win   # Windows rendition

The raster is stored **run-length encoded** (word, count pairs) rather than
raw: the images are mostly flat limited-range black around a centred dialog
box, so RLE shrinks the generated header ~7x (14.8 MB -> ~2.2 MB) and the
compiled .rodata ~3x (2.4 MB -> 0.74 MB).  The driver expands it once into a
small sprite buffer (avmcv710-status.c, avm_splash_expand()).

Inputs, in the chosen directory:
    aver_custom_no_signal.bmp
    aver_custom_out_of_range.bmp
    aver_custom_hdcp_protection.bmp
    aver_custom_content_protection.bmp

Usage:
    python3 tools/gen_bmp.py [srcdir] [outfile]
"""

import os
import struct
import sys

W = 640
H = 480

HERE = os.path.dirname(os.path.abspath(__file__))
# cv710userspace/assets is the macOS rendition (see the module docstring).
DEFAULT_SRC = os.path.normpath(
    os.path.join(HERE, "..", "..", "cv710userspace", "assets"))

SRC = {
    "no_signal": "aver_custom_no_signal.bmp",
    "out_of_range": "aver_custom_out_of_range.bmp",
    "hdcp": "aver_custom_hdcp_protection.bmp",
    "content_protection": "aver_custom_content_protection.bmp",
}


def load_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise SystemExit(f"{path}: not a BMP")
    off = struct.unpack("<I", d[10:14])[0]
    w, h = struct.unpack("<ii", d[18:26])
    bpp = struct.unpack("<H", d[28:30])[0]
    if w != W or abs(h) != H or bpp != 24:
        raise SystemExit(f"{path}: expected {W}x{H}x24, got {w}x{abs(h)}x{bpp}")
    stride = ((w * 3 + 3) // 4) * 4
    px = d[off : off + stride * abs(h)]
    rows = []
    for y in range(abs(h)):
        row = px[y * stride : y * stride + w * 3]
        rows.append(row)
    if h > 0:
        rows.reverse()
    return rows


def bgr_to_yuyv(rows):
    """BT.601 limited-range YUY2, top-down raster."""
    out = bytearray()
    for y in range(H):
        row = rows[y]
        for x in range(0, W, 2):
            bgr0 = row[x * 3 : x * 3 + 3]
            bgr1 = row[x * 3 + 3 : x * 3 + 6]
            yuv = []
            for r, g, b in ((bgr0[2], bgr0[1], bgr0[0]),
                            (bgr1[2], bgr1[1], bgr1[0])):
                yy = int(0.257 * r + 0.504 * g + 0.098 * b + 16)
                yuv.append(max(0, min(255, yy)))
            # Chroma shared by the macropixel pair (average of the two).
            cbs = crs = 0
            for r, g, b in ((bgr0[2], bgr0[1], bgr0[0]),
                            (bgr1[2], bgr1[1], bgr1[0])):
                cbs += -0.148 * r - 0.291 * g + 0.439 * b + 128
                crs += 0.439 * r - 0.368 * g - 0.071 * b + 128
            cb = max(0, min(255, int(cbs / 2)))
            cr = max(0, min(255, int(crs / 2)))
            out += bytes((yuv[0], cb, yuv[1], cr))
    return bytes(out)


def rle_words(data):
    """Run-length encode a YUY2 raster into (u32 word, u16 count) runs."""
    runs = []
    prev = None
    n = 0
    for i in range(0, len(data), 4):
        w = int.from_bytes(data[i : i + 4], "little")
        if w == prev and n < 0xFFFF:
            n += 1
        else:
            if prev is not None:
                runs.append((prev, n))
            prev = w
            n = 1
    if prev is not None:
        runs.append((prev, n))
    return runs


def emit_rle(lines, name, runs):
    lines.append(f"static const u32 {name}[{len(runs) * 2}] = {{")
    lines.append("\t/* value, count */")
    for i in range(0, len(runs), 4):
        chunk = runs[i : i + 4]
        lines.append("\t" + " ".join(f"0x{v:08X}, {n}," for v, n in chunk))
    lines.append("};")
    lines.append("")


def main():
    drvdir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    outfile = sys.argv[2] if len(sys.argv) > 2 else (
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "avmcv710-bmp.h"))

    lines = []
    lines.append("/* SPDX-License-Identifier: GPL-2.0-only */")
    lines.append("/*")
    lines.append(" * avmcv710-bmp.h - generated file, do not edit.")
    lines.append(" *")
    lines.append(" * YUY2 640x480 overlays, run-length encoded, converted from the")
    lines.append(" * vendor driver's composer bitmaps by tools/gen_bmp.py.  Shown")
    lines.append(" * when the input is absent, out of range or encrypted, matching")
    lines.append(" * the behavior of the official Windows/macOS driver.")
    lines.append(" */")
    lines.append("#ifndef AVM_CV710_BMP_H")
    lines.append("#define AVM_CV710_BMP_H")
    lines.append("")
    lines.append(f"#define AVM_BMP_W\t\t{W}u")
    lines.append(f"#define AVM_BMP_H\t\t{H}u")
    lines.append(f"#define AVM_BMP_WORDS\t\t({W}u * {H}u / 2u)")
    lines.append("")

    total_runs = 0
    for key, fname in SRC.items():
        rows = load_bmp(os.path.join(drvdir, fname))
        yuyv = bgr_to_yuyv(rows)
        if len(yuyv) != W * H * 2:
            raise SystemExit(f"{fname}: bad conversion size {len(yuyv)}")
        runs = rle_words(yuyv)
        total_runs += len(runs)
        lines.append(f"#define AVM_BMP_{key.upper()}_RUNS\t{len(runs)}u")
        emit_rle(lines, f"avm_bmp_{key}_rle", runs)

    lines.append(f"/* {total_runs} runs total; decoded size "
                 f"{len(SRC) * W * H * 2} bytes */")
    lines.append("#endif /* AVM_CV710_BMP_H */")

    os.makedirs(os.path.dirname(outfile), exist_ok=True)
    with open(outfile, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {outfile}: {total_runs} runs, "
          f"{os.path.getsize(outfile)} bytes of header")


if __name__ == "__main__":
    main()
