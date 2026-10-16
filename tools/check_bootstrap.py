#!/usr/bin/env python3
"""Verify the generated bootstrap table (BOOTSTRAP-PARITY gate).

`avmcv710-bootstrap.h` is generated from the community port's *fast-start*
capture, which is itself a trimmed capture of the official driver's real USB
EP1 traffic.  This script re-checks the three claims that make the replay
trustworthy, so a regenerated table can never silently drift:

  1. our 871-command table is byte-for-byte identical to
     `commanddata_cv710_fast.cpp` (the reference capture);
  2. every fast write appears, in order, in the full capture
     (`commanddata_cv710.cpp`), and the final I2C register state the two
     reach is identical - i.e. the trim drops only reads/poll loops;
  3. the EDID the replay uploads to the EEPROM bridge is byte-for-byte the
     EDID embedded in the vendor binaries (`avmu3_x64.sys`,
     `libC877Driver.dylib`).

Exit status is 0 only if every check passes.  Run manually after editing
tools/gen_bootstrap.py or the capture:

  python3 tools/check_bootstrap.py
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent                      # /home/hp/Projects/Piecap
DRIVER = HERE.parent                           # avmcv710/
FAST = ROOT / "cv710userspace/src/liblgx/liblgx-common/src/bootstrap/commanddata_cv710_fast.cpp"
FULL = ROOT / "cv710userspace/src/liblgx/liblgx-common/src/bootstrap/commanddata_cv710.cpp"
VENDOR = [
    ("win/avmu3_x64.sys", ROOT / "driver_extracted/win/avmu3_x64.sys"),
    ("mac/libC877Driver.dylib",
     ROOT / "driver_extracted/mac/RECentral Install Disk/RECentral.app/"
            "Contents/Resources/libC877Driver.dylib"),
]

EDID_SLAVE = 0x36           # FX3 bridge id for the EDID EEPROM
I2C_OP = 0x01               # "01 ss rl rr.. dd.." I2C write frame


def parse_header(path):
    """Return the generated table as [(op, bytes|len)]."""
    txt = path.read_text()
    blob = bytes(int(x, 16)
                 for x in re.findall(r"0x([0-9A-Fa-f]{2})",
                                     txt.split("avm_boot_blob")[1]))
    cmds = re.findall(r"\{\s*(\d+)u?,\s*(\d+)u?,\s*(\d+)u?\s*\}", txt)
    out = []
    for op, ln, off in cmds:
        if op == "0":
            out.append(("W", blob[int(off):int(off) + int(ln)]))
        else:
            out.append(("R", int(ln)))
    return out


def parse_capture(path, varname):
    """Parse a '>HEX <N' capture string.  '<N' is decimal (UsbStream.cpp
    uses std::stoi with base 10); the '>' payload is hex."""
    src = path.read_text()
    m = re.search(r"std::string\s+" + varname + r"\s*\{\s*\"(.*)\"\s*\}",
                  src, re.S)
    if not m:
        sys.exit(f"FATAL: no string literal in {path}")
    s = m.group(1).replace("\\", "").replace("\n", "").replace('"', "")
    out = []
    for tok in re.findall(r"[<>][0-9A-Fa-f]+", s):
        if tok[0] == ">":
            out.append(("W", bytes.fromhex(tok[1:])))
        else:
            out.append(("R", int(tok[1:], 10)))
    return out


def regmap(seq):
    """Final single-register I2C state: '01 ss 01 rr vv' -> (ss,rr)=vv."""
    m = {}
    for op, b in seq:
        if op == "W" and len(b) == 5 and b[0] == I2C_OP and b[2] == 0x01:
            m[(b[1], b[3])] = b[4]
    return m


def edid_bytes(seq):
    """Concatenate the multi-byte writes to the EDID bridge."""
    edid = bytearray()
    for op, b in seq:
        if op == "W" and len(b) > 5 and b[0] == I2C_OP and b[1] == EDID_SLAVE:
            edid += b[4:]
    return bytes(edid)


def main():
    fails = []

    ours = parse_header(DRIVER / "avmcv710-bootstrap.h")
    fast = parse_capture(FAST, "cv710_setup_commands_fast")
    full = parse_capture(FULL, "cv710_setup_commands")

    # 1. header == reference fast capture
    if ours == fast:
        print(f"[ok] header == fast capture ({len(ours)} commands, "
              f"{sum(len(b) for o, b in ours if o == 'W')} write bytes)")
    else:
        fails.append("header != fast capture")
        n = min(len(ours), len(fast))
        d = [i for i in range(n) if ours[i] != fast[i]]
        print(f"[FAIL] header != fast capture: {len(d)} diffs "
              f"(len {len(ours)} vs {len(fast)}), first at {d[:6]}")
        for i in d[:6]:
            a = ours[i] if ours[i][0] == "R" else ("W", ours[i][1].hex())
            b = fast[i] if fast[i][0] == "R" else ("W", fast[i][1].hex())
            print(f"    [{i}] ours={a} fast={b}")

    # 2. fast writes in order inside the full capture; identical final state
    fw = [b for o, b in fast if o == "W"]
    uw = [b for o, b in full if o == "W"]
    i, missing = 0, 0
    for b in fw:
        try:
            i = uw.index(b, i) + 1
        except ValueError:
            missing += 1
    fm, um = regmap(fast), regmap(full)
    only_full = {k: v for k, v in um.items() if k not in fm}
    only_fast = {k: v for k, v in fm.items() if k not in um}
    diff = {k: (fm[k], um[k]) for k in fm if k in um and fm[k] != um[k]}
    if not (missing or only_full or only_fast or diff):
        print(f"[ok] fast writes are an ordered subset of the full capture "
              f"({len(fw)}/{len(uw)}); final regmap identical "
              f"({len(fm)} registers)")
    else:
        fails.append("fast trim is not state-preserving")
        print(f"[FAIL] trim: {missing} write(s) missing, "
              f"{len(only_full)} reg(s) only in full, "
              f"{len(only_fast)} only in fast, {len(diff)} differing")

    # 3. EDID == the vendor binaries' EDID
    edid = edid_bytes(ours)
    if edid[:8] != bytes([0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00]):
        fails.append("EDID header invalid")
        print(f"[FAIL] EDID header invalid: {edid[:8].hex()}")
    else:
        for name, path in VENDOR:
            if not path.exists():
                print(f"[skip] {name}: not present")
                continue
            blob = path.read_bytes()
            off = blob.find(edid[:64])
            if off < 0:
                fails.append(f"EDID absent from {name}")
                print(f"[FAIL] {name}: EDID prefix absent")
                continue
            n = 0
            while n < len(edid) and blob[off + n] == edid[n]:
                n += 1
            if n == len(edid):
                print(f"[ok] {name}: EDID matches {n} bytes at {hex(off)}")
            else:
                fails.append(f"EDID differs in {name}")
                print(f"[FAIL] {name}: EDID matches {n}/{len(edid)} bytes "
                      f"at {hex(off)}")

    if fails:
        print("\nBOOTSTRAP-PARITY: FAILED")
        for f in fails:
            print("  -", f)
        return 1
    print("\nBOOTSTRAP-PARITY: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
