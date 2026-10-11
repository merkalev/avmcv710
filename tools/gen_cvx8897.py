#!/usr/bin/env python3
"""Emit a kernel C header embedding the CVX8897 frame-rate-converter tables.

The tables were recovered from the vendor drivers and live in the userspace
repository at ../cv710userspace/tools/cvx8897/cvx8897_tables.json (see that
directory's README.md).  All modes share one 251-entry register write order and
differ only in the values, so the header exposes a single order array plus one
value array per mode.

Usage:
    python3 tools/gen_cvx8897.py [cvx8897_tables.json] > avmcv710-cvx8897.h
"""
import json
import os
import sys

DEFAULT_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "cv710userspace", "tools", "cvx8897",
                            "cvx8897_tables.json")


def arr(vals, per=12):
    lines = []
    for i in range(0, len(vals), per):
        lines.append("\t" + ", ".join("0x%02x" % v for v in vals[i:i + per]) + ",")
    return "\n".join(lines)


def ident(name):
    # Mode names are already [A-Za-z0-9_]; prefix so the identifier cannot
    # start with a digit.
    return "avm_cvx_" + name


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_JSON
    with open(path) as fp:
        table = json.load(fp)
    order = table["reg_order"]
    modes = table["modes"]

    out = []
    out.append("/* SPDX-License-Identifier: GPL-2.0-only */")
    out.append("/*")
    out.append(" * avmcv710-cvx8897.h - generated file, do not edit.")
    out.append(" *")
    out.append(" * CVX8897 frame-rate converter register image, recovered from the")
    out.append(" * vendor drivers (see ../cv710userspace/tools/cvx8897/README.md).")
    out.append(" * Regenerate with tools/gen_cvx8897.py.")
    out.append(" */")
    out.append("#ifndef AVM_CVX8897_H")
    out.append("#define AVM_CVX8897_H")
    out.append("")
    out.append("/* CVX8897 7-bit address on the FX3 I2C bridge (01 18 01 r v). */")
    out.append("#define AVM_CVX_I2C_ADDR\t0x18")
    out.append("#define AVM_CVX_REG_COUNT\t%d" % len(order))
    out.append("")
    out.append("/* Register write order, identical for every mode. */")
    out.append("static const u8 avm_cvx_reg_order[AVM_CVX_REG_COUNT] = {")
    out.append(arr(order))
    out.append("};")
    out.append("")
    for name, vals in modes.items():
        out.append("static const u8 %s[AVM_CVX_REG_COUNT] = {" % ident(name))
        out.append(arr(vals))
        out.append("};")
        out.append("")
    out.append("#endif /* AVM_CVX8897_H */")
    print("\n".join(out))


if __name__ == "__main__":
    main()
