#!/usr/bin/env python3
"""Generate avmcv710-bmp.h from the official driver's overlay bitmaps.

The AVerMedia Windows/macOS drivers ship four 640x480x24 overlay bitmaps
(no-signal, out-of-range, HDCP and content-protection) that they composite
into the capture stream when the input is unavailable or encrypted.  This
tool converts them to the YUY2 raster the FPGA delivers (640x480, 2 bytes
per pixel, top-down) and emits a C header so the kernel driver can blit
them with the exact same behavior: show the overlay instead of a frozen
or black frame.

Inputs (found in the extracted vendor installer, driver_extracted/win/):
    aver_custom_no_signal.bmp
    aver_custom_out_of_range.bmp
    aver_custom_hdcp_protection.bmp
    aver_custom_content_protection.bmp

Usage:
    python3 tools/gen_bmp.py [drvdir] [outfile]

The bitmaps originate from the vendor driver package; keep this file and
the cross-repo path in sync if the assets move.
"""

import os
import struct
import sys

W = 640
H = 480

SRC = {
    "NO_SIGNAL": "aver_custom_no_signal.bmp",
    "OUT_OF_RANGE": "aver_custom_out_of_range.bmp",
    "HDCP": "aver_custom_hdcp_protection.bmp",
    "CONTENT_PROTECTION": "aver_custom_content_protection.bmp",
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
        # BMP rows are bottom-up
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

def emit_array(lines, name, data):
    lines.append(f"static const u8 {name}[{W * H * 2}] = {{")
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("	" + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")

def main():
    drvdir = sys.argv[1] if len(sys.argv) > 1 else (
        "/home/hp/Projects/Piecap/driver_extracted/win")
    outfile = sys.argv[2] if len(sys.argv) > 2 else (
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "avmcv710-bmp.h"))

    lines = []
    lines.append("/* SPDX-License-Identifier: GPL-2.0-only */")
    lines.append("/*")
    lines.append(" * avmcv710-bmp.h - generated file, do not edit.")
    lines.append(" *")
    lines.append(" * YUY2 640x480 overlays converted from the vendor driver's")
    lines.append(" * composer bitmaps by tools/gen_bmp.py.  Shown when the")
    lines.append(" * input is absent, out of range or encrypted, matching the")
    lines.append(" * behavior of the official Windows/macOS driver.")
    lines.append(" */")
    lines.append("#ifndef AVM_CV710_BMP_H")
    lines.append("#define AVM_CV710_BMP_H")
    lines.append("")
    lines.append(f"#define AVM_BMP_W\t\t{W}u")
    lines.append(f"#define AVM_BMP_H\t\t{H}u")
    lines.append(f"#define AVM_BMP_WORDS\t\t({W}u * {H}u / 2u)")
    lines.append("")

    total = 0
    for key, fname in SRC.items():
        rows = load_bmp(os.path.join(drvdir, fname))
        yuyv = bgr_to_yuyv(rows)
        if len(yuyv) != W * H * 2:
            raise SystemExit(f"{fname}: bad conversion size {len(yuyv)}")
        total += len(yuyv)
        # name them after the enum avm_splash values for a 1:1 mapping
        emit_array(lines, f"avm_bmp_{key.lower()}", yuyv)

    lines.append(f"/* {total} bytes of overlay payload total */")
    lines.append("#endif /* AVM_CV710_BMP_H */")

    os.makedirs(os.path.dirname(outfile), exist_ok=True)
    with open(outfile, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {outfile} ({len(yuyv)*len(SRC)} bytes of overlay data)")

if __name__ == "__main__":
    main()