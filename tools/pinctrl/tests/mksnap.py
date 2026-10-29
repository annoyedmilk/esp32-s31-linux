#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write register snapshots for boards that are not at hand.

    mksnap.py DTS_DIR coreboard1|esp32s31 > FILE

The names and the reserved pins come from the device tree; the pin states
follow what the drivers of the board set.  The Korvo-1 snapshot is a dump
of the hardware.
"""

import re
import sys

NPINS, NSIGS, WORDS = 62, 256, 1024
IE, WPU, WPD, DRV_20MA = 1 << 9, 1 << 8, 1 << 7, 2 << 10
SIG_GMII_MDI, SIG_GMII_MDC, SIG_GMII_MDO, SIG_RMT_OUT0 = 107, 108, 109, 246
PULL = {"pn": 0, "pu": WPU, "pd": WPD}

# pin: (mode, pull, level, extra); mode a<N>, ip, op-hi, op-lo, no, mx
BOARDS = {
    "coreboard1": {
        "dts": "esp32s31_coreboard1.dts",
        "compatible": "esp,esp32s31-function-coreboard-1",
        "model": "Espressif ESP32-S31-Function-CoreBoard-1",
        "used": [5, 6, 7, *range(8, 20)],
        "cnnt": 1 << 1,
        "pins": {
            2: ("ip", "pu", 1),
            3: ("ip", "pu", 1),
            4: ("ip", "pu", 1),
            5: ("mx", "pn", None, {"out": SIG_GMII_MDC}),
            6: ("mx", "pn", 1, {"out": SIG_GMII_MDO, "in": [SIG_GMII_MDI]}),
            7: ("op-hi", "pn", 1),
            **{p: ("a2", "pn", None) for p in range(8, 14)},
            **{p: ("a2", "pn", p % 2) for p in range(14, 20)},
            **{p: ("ip", "pu", 1) for p in range(20, 26)},
            **{p: ("a0", "pu", 1) for p in (26, 27, 28, 30, 31, 32)},
            58: ("a0", "pu", 1),
            59: ("a0", "pu", 1),
            60: ("mx", "pn", None, {"out": SIG_RMT_OUT0}),
            61: ("ip", "pu", 1),
        },
    },
    "esp32s31": {
        "dts": "esp32s31.dts",
        "compatible": "esp,esp32s31-wroom-3",
        "model": "Espressif ESP32-S31-WROOM-3-N16R16V",
        "used": [],
        "cnnt": 0,
        "pins": {
            0: ("no", "pn", None),
            1: ("no", "pn", None),
            **{p: ("a0", "pu", 1) for p in (26, 27, 28, 30, 31, 32)},
            42: ("ip", "pd", 0),
            57: ("op-hi", "pn", 1),
            58: ("a0", "pu", 1),
            59: ("a0", "pu", 1),
            60: ("ip", "pu", 1),
            61: ("ip", "pu", 1),
        },
    },
}


def dt_prop(dts_dir: str, dts: str, prop: str) -> str:
    """The value of prop in the board file, else in esp32s31.dtsi."""
    for f in (dts, "esp32s31.dtsi"):
        with open(f"{dts_dir}/{f}") as fh:
            m = re.search(rf"\b{prop}\s*=\s*(.*?);", fh.read(), re.DOTALL)
        if m:
            return re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.DOTALL)
    return ""


def snapshot(dts_dir: str, b: dict) -> str:
    names = re.findall(r'"([^"]*)"', dt_prop(dts_dir, b["dts"], "gpio-line-names"))
    cells = [
        int(c, 0)
        for c in re.findall(r"\w+", dt_prop(dts_dir, b["dts"], "gpio-reserved-ranges"))
    ]
    reserved = 0
    for first, count in zip(cells[::2], cells[1::2]):
        reserved |= ((1 << count) - 1) << first
    used = reserved
    for p in b["used"]:
        used |= 1 << p

    iomux, gpio = [0] * WORDS, [0] * WORDS
    level = enable = out = 0
    for s in range(NSIGS):
        gpio[(0x2F4 + 4 * s) // 4] = 0x80
    for p in range(NPINS):
        mode, pull, lev, *extra = b["pins"].get(p, ("ip", "pn", 0))
        extra = extra[0] if extra else {}
        func = int(mode[1]) if mode[0] == "a" else 1
        ie = 0 if mode == "no" or lev is None else IE
        iomux[p] = func << 12 | DRV_20MA | PULL[pull] | ie
        gpio[(0xAF4 + 4 * p) // 4] = extra.get("out", 256)
        for s in extra.get("in", []):
            gpio[(0x2F4 + 4 * s) // 4] = 1 << 9 | p
        if mode.startswith("op"):
            enable |= 1 << p
            out |= (mode == "op-hi") << p
        level |= (lev or 0) << p
    # Bank 0 (pins 0-31) and bank 1 (32-61) of OUT, ENABLE and IN.
    for reg0, reg1, val in (
        (0x04, 0x10, out),
        (0x34, 0x40, enable),
        (0x64, 0x68, level),
    ):
        gpio[reg0 // 4] = val & 0xFFFFFFFF
        gpio[reg1 // 4] = val >> 32

    lines = [
        "# esp32s31-pinctrl snapshot 1",
        f"compatible {b['compatible']}",
        f"model {b['model']}",
        "names " + ",".join(names),
        f"reserved {reserved:016x}",
        f"used {used:016x}",
        f"cnnt {b['cnnt']:08x}",
    ]
    for page, words in (("iomux", iomux), ("gpio", gpio)):
        for off in range(0, WORDS * 4, 32):
            row = " ".join(f"{w:08x}" for w in words[off // 4 : off // 4 + 8])
            lines.append(f"{page} {off:03x} {row}")
    return "\n".join(lines) + "\n"


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[2] not in BOARDS:
        print(__doc__, file=sys.stderr)
        return 2
    sys.stdout.write(snapshot(sys.argv[1], BOARDS[sys.argv[2]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
