#!/usr/bin/env python3
"""
Generate an Apple Device Tree (ADT) for Redmi Note 8 Pro (begonia / MT6785) targeting XNU-11215 (iOS 18).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from typing import Dict, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402
from adt import cstr, make_node, u32, u64  # noqa: E402

# Verified MT6785 / begonia constants (mt6785.dts, xiaomi-mt6785-common.dtsi)
SOC_BASE = 0x0C000000
SOC_SIZE = 0x14000000
GICD_PA = 0x0C000000
GICD_SIZE = 0x00040000
GICR_PA = 0x0C040000
GICR_SIZE = 0x00200000
INT_POL_PA = 0x0C53A650
INT_POL_SIZE = 0x00000050

TIMEBASE_HZ = 13_000_000
VTIMER_INTID = 27  # ARM PPI 11 -> GIC INTID 16 + 11 = 27

FB_FALLBACK_PA = 0x7D9B0000
FB_FALLBACK_SIZE = 0x02250000
PANEL_W = 1080
PANEL_H = 2340

# Clean contiguous high DRAM window on MT6785 (mblock[17]: 0xc0000000, size 0x3fe00000),
# avoiding modem/SCP/ATF carve-outs in 0x40000000..0xbfffffff.
# Offset +0x01004000 matches XNU's 32 MiB L2 block alignment for __TEXT (0xfffffe0007004000).
DEFAULT_DRAM_BASE = 0xC1004000
DEFAULT_DRAM_SIZE = 0x3EBFC000  # ends at 0xFFC00000 (~1004 MiB, 16 KiB aligned)

BEGONIA_ECID = 0x4245474F4E494101
BOOT_MANIFEST_HASH = hashlib.sha384(b"begonia-ios boot manifest").digest()

IDENTITIES: Dict[str, Dict[str, str]] = {
    "d22ap": {"target": "D22AP", "machine": "iPhone10,3", "target_type": "D22", "chip_id": 0x8015},
    "n841ap": {"target": "N841AP", "machine": "iPhone11,8", "target_type": "N841", "chip_id": 0x8020},
    "n104ap": {"target": "N104AP", "machine": "iPhone12,1", "target_type": "N104", "chip_id": 0x8030},
}


class PhandleAlloc:
    def __init__(self) -> None:
        self._next = 1

    def prop(self) -> Tuple[str, bytes]:
        val = u32(self._next)
        self._next += 1
        return ("AAPL,phandle", val)


def extract_product_props(path: str) -> Optional[list]:
    with open(path, "rb") as fp:
        raw = adt.unwrap_im4p(fp.read())
    root, _ = adt.parse(raw)
    prod = root.child("product")
    if prod is None:
        raise ValueError(f"{path}: no /product node")
    props = []
    for k, flags, v in prod.props:
        if flags or k in ("name", "AAPL,phandle"):
            continue
        if k == "artwork-display-gamut":
            v = cstr("sRGB")
        props.append((k, v))
    return props


def build_begonia_adt(
    dram_base: int = DEFAULT_DRAM_BASE,
    dram_size: int = DEFAULT_DRAM_SIZE,
    fb_base: int = FB_FALLBACK_PA,
    fb_size: int = FB_FALLBACK_SIZE,
    cpus_count: int = 1,
    identity: str = "n841ap",
    display_scale: int = 2,
    points: Tuple[int, int] = (414, 896),
    product_adt_path: Optional[str] = None,
    seed: Optional[bytes] = None,
) -> adt.Node:
    """Build a complete begonia (MT6785) ADT suitable for XNU-11215 bring-up."""
    if identity not in IDENTITIES:
        raise ValueError(f"unknown identity {identity!r}; choose from {sorted(IDENTITIES)}")
    if not (1 <= cpus_count <= 8):
        raise ValueError("cpus_count must be between 1 and 8")

    ident = IDENTITIES[identity]
    ph = PhandleAlloc()
    rng_seed = seed if seed is not None else os.urandom(256)

    asmb = make_node("asmb", [ph.prop(), ("lp-sip0", u64(0x6F)), ("lp-sip1", u32(1))])
    memory_map = make_node("memory-map", [ph.prop()])

    chosen = make_node(
        "chosen",
        [
            ph.prop(),
            ("dram-base", u64(dram_base)),
            ("dram-size", u64(dram_size)),
            ("random-seed", rng_seed),
            ("kernel-ctrr-to-be-enabled", u32(1)),
            ("debug-enabled", u32(1)),
            ("firmware-version", cstr("mtkloader-begonia")),
            ("system-firmware-version", cstr("mtkloader-begonia")),
            ("boot-manifest-hash", BOOT_MANIFEST_HASH),
            ("crypto-hash-method", cstr("sha2-384")),
            ("display-scale", u32(display_scale)),
            ("unique-chip-id", u64(BEGONIA_ECID)),
            ("chip-id", u32(ident["chip_id"])),
            ("certificate-production-status", u32(0)),
            ("effective-production-status-ap", u32(0)),
            ("certificate-security-mode", u32(1)),
            ("effective-security-mode-sep", u32(1)),
            ("mac-address-wifi0", bytes([0x02, 0x00, 0x5E, 0xBE, 0x60, 0x01])),
            ("mac-address-bluetooth0", bytes([0x02, 0x00, 0x5E, 0xBE, 0x60, 0x02])),
        ],
        [memory_map, asmb],
    )

    cpu_nodes = []
    for core in range(cpus_count):
        is_big = core >= 6
        core_compat = "arm,cortex-a76" if is_big else "arm,cortex-a55"
        freq_hz = 2_171_000_000 if is_big else 1_701_000_000
        cpu_nodes.append(
            make_node(
                f"cpu{core}",
                [
                    ph.prop(),
                    ("device_type", cstr("cpu")),
                    ("compatible", cstr(core_compat) + cstr("qcom,kryo470") + cstr("ARM,v8")),
                    ("reg", u32(core << 8)),  # mt6785.dts MPIDR Aff1 = core (0x000..0x700)
                    ("cpu-id", u32(core)),
                    ("cluster-id", u32(0)),
                    ("cluster-type", u32(0)),
                    ("die-cluster-id", u32(0)),
                    ("cluster-core-id", u32(core)),
                    ("l2-cache-id", u32(core)),
                    ("l3-cache-id", u32(0)),
                    ("state", cstr("running" if core == 0 else "stopped")),
                    ("timebase-frequency", u32(TIMEBASE_HZ)),
                    ("clock-frequency", u32(freq_hz)),
                    ("bus-frequency", u32(100_000_000)),
                    ("memory-frequency", u32(1_866_000_000)),
                    ("peripheral-frequency", u32(TIMEBASE_HZ)),
                    ("fixed-frequency", u32(TIMEBASE_HZ)),
                ],
            )
        )

    cpus = make_node("cpus", [ph.prop(), ("#address-cells", u32(1)), ("#size-cells", u32(0))], cpu_nodes)

    gic = make_node(
        "gic",
        [
            ph.prop(),
            ("compatible", cstr("arm,gic-v3")),
            ("interrupt-controller", b""),
            ("virtual-timer-intid", u32(VTIMER_INTID)),
            ("reg", u64(GICD_PA - SOC_BASE, GICD_SIZE, GICR_PA - SOC_BASE, GICR_SIZE)),
            ("mtk,intpol-reg", u64(INT_POL_PA, INT_POL_SIZE)),
        ],
    )
    wdt = make_node("wdt", [ph.prop(), ("no-pmu", u32(1))])
    disp0 = make_node(
        "disp0",
        [
            ph.prop(),
            ("device_type", cstr("display-subsystem")),
            ("compatible", cstr("sweet,display")),
            ("canvas-width", u32(points[0] * display_scale)),
            ("canvas-height", u32(points[1] * display_scale)),
        ],
    )
    mali = make_node(
        "mali",
        [
            ph.prop(),
            ("compatible", cstr("mediatek,mali") + cstr("arm,mali-bifrost")),
            ("reg", u64(0x13040000 - SOC_BASE, 0x10000)),
        ],
    )
    arm_io = make_node(
        "arm-io",
        [
            ph.prop(),
            ("device_type", cstr("sweet-io")),
            ("compatible", cstr("begonia-io") + cstr("sweet-io") + cstr("arm-io")),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
            ("ranges", u64(0, SOC_BASE, SOC_SIZE)),
        ],
        [gic, wdt, disp0, mali],
    )

    sweet_ufs = make_node(
        "sweet-ufs",
        [
            ph.prop(),
            ("compatible", cstr("mediatek,ufshci") + cstr("qcom,ufshc")),
            ("reg", u64(0x11270000, 0x2300)),
        ],
    )

    fbcon = make_node(
        "sweet-fbcon",
        [
            ph.prop(),
            ("reg", u64(fb_base, fb_size)),
            ("width", u32(PANEL_W)),
            ("height", u32(PANEL_H)),
            ("stride", u32(PANEL_W * 4)),
        ],
    )
    defaults = make_node("defaults", [ph.prop(), ("no-effaceable-storage", u32(1))])
    buttons = make_node(
        "buttons",
        [
            ph.prop(),
            ("button-names", cstr("volup") + cstr("voldown") + cstr("hold")),
            ("home-button-type", u32(2)),
            ("opposed-power-vol-buttons", u32(1)),
        ],
    )

    children = [chosen, defaults, cpus, arm_io, sweet_ufs, fbcon, buttons]
    if product_adt_path:
        children.append(make_node("product", [ph.prop()] + extract_product_props(product_adt_path)))

    return make_node(
        "device-tree",
        [
            ph.prop(),
            (
                "compatible",
                cstr(ident["target"])
                + cstr(ident["machine"])
                + cstr("xiaomi,begonia")
                + cstr("xiaomi,sweet")
                + cstr("mediatek,MT6785")
                + cstr("AppleARM"),
            ),
            ("model", cstr(ident["machine"])),
            ("target-type", cstr(ident["target_type"])),
            ("target-sub-type", cstr(ident["target"])),
            ("manufacturer", cstr("Xiaomi / AppleARM")),
            ("#address-cells", u32(2)),
            ("#size-cells", u32(2)),
        ],
        children,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate begonia ADT for XNU-11215")
    ap.add_argument("out", help="Output .adt file path")
    ap.add_argument("--dram-base", type=lambda v: int(v, 0), default=DEFAULT_DRAM_BASE)
    ap.add_argument("--dram-size", type=lambda v: int(v, 0), default=DEFAULT_DRAM_SIZE)
    ap.add_argument("--fb-base", type=lambda v: int(v, 0), default=FB_FALLBACK_PA)
    ap.add_argument("--fb-size", type=lambda v: int(v, 0), default=FB_FALLBACK_SIZE)
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--identity", default="n841ap", choices=sorted(IDENTITIES))
    ap.add_argument("--display-scale", type=int, default=2)
    ap.add_argument("--points", default=None,
                    help="UI size in points, WxH (default 414x896); canvas = points x display-scale")
    ap.add_argument("--product-adt", default=None)
    args = ap.parse_args()

    tree = build_begonia_adt(
        dram_base=args.dram_base,
        dram_size=args.dram_size,
        fb_base=args.fb_base,
        fb_size=args.fb_size,
        cpus_count=args.cpus,
        identity=args.identity,
        display_scale=args.display_scale,
        points=tuple(int(v) for v in args.points.split("x")) if args.points else (414, 896),
        product_adt_path=args.product_adt,
    )
    blob = adt.serialize(tree)
    with open(args.out, "wb") as fp:
        fp.write(blob)
    print(f"{args.out}: {len(blob)} bytes (identity={args.identity}, cpus={args.cpus})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
