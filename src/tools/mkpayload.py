#!/usr/bin/env python3
"""
Pack an XNU Mach-O kernel + begonia ADT into a SWEETXNU v1 or v2 payload.

Resolves LC_DYLD_CHAINED_FIXUPS rebases offline so the bare-metal mtkloader
can copy or inflate flat segments directly to physical memory.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_DYLD_CHAINED_FIXUPS = 0x80000034

XNU_PAGE = 0x4000
BOOT_LINE_LENGTH = 608
PAYLOAD_HDR_SIZE = 0x48 + BOOT_LINE_LENGTH  # 0x2a8
COMPRESS_THRESHOLD = 1 << 20


def parse_macho(buf: bytes) -> Dict[str, Any]:
    magic, cputype, cpusub, _, ncmds, _, _, _ = struct.unpack_from("<8I", buf, 0)
    if magic != MH_MAGIC_64:
        raise ValueError("input kernel is not a 64-bit Mach-O")
    segs: List[Dict[str, Any]] = []
    entry: Optional[int] = None
    fixups: Optional[Tuple[int, int]] = None
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8 : off + 24].split(b"\0", 1)[0].decode("utf-8", errors="replace")
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", buf, off + 24)
            segs.append(
                {
                    "name": name,
                    "vmaddr": vmaddr,
                    "vmsize": vmsize,
                    "fileoff": fileoff,
                    "filesize": filesize,
                }
            )
        elif cmd == LC_UNIXTHREAD:
            # arm_thread_state64: x0..x28, fp, lr, sp, pc (index 32)
            entry, = struct.unpack_from("<Q", buf, off + 16 + 32 * 8)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from("<II", buf, off + 8)
        off += size
    if not segs or entry is None:
        raise ValueError("Mach-O missing segments or LC_UNIXTHREAD entry")
    return {"cputype": cputype, "cpusub": cpusub & 0xFF, "segs": segs, "entry": entry, "fixups": fixups}


def _decode_chained_ptr(raw: int, fmt: int, image_base: int) -> Tuple[int, int, int]:
    """Return (target_va, next_step, stride_bytes) for one chained pointer."""
    if fmt in (2, 6):  # DYLD_CHAINED_PTR_64 / 64_OFFSET
        is_auth = (raw >> 63) & 1
        is_bind = (raw >> 62) & 1
        if is_auth or is_bind:
            raise ValueError(f"unsupported auth/bind chained pointer: {raw:#x}")
        nxt = (raw >> 51) & 0xFFF
        target = raw & 0xFFFFFFFFF
        high8 = (raw >> 36) & 0xFF
        target |= high8 << 56
        if fmt == 6:
            target += image_base
        return target, nxt, 4
    if fmt == 1:  # DYLD_CHAINED_PTR_ARM64E
        is_auth = (raw >> 63) & 1
        is_bind = (raw >> 62) & 1
        if is_bind:
            raise ValueError("kernel chained fixup contains external bind")
        nxt = (raw >> 51) & 0x7FF
        if is_auth:
            target = image_base + (raw & 0xFFFFFFFF)
        else:
            target = (raw & 0x7FFFFFFFFFF) | (((raw >> 43) & 0xFF) << 56)
            if target < image_base:
                target += image_base
        return target, nxt, 8
    raise ValueError(f"unsupported chained fixup pointer format {fmt}")


def apply_chained_fixups(buf: bytearray, macho: Dict[str, Any]) -> int:
    """Rewrite LC_DYLD_CHAINED_FIXUPS rebases in-place to plain target virtual addresses."""
    if not macho["fixups"]:
        return 0
    data_off, _ = macho["fixups"]
    _, starts_off, _, _, imports_count, _, _ = struct.unpack_from("<7I", buf, data_off)
    if imports_count != 0:
        raise ValueError("kernel chained fixups have imports; only rebases are supported")
    image_base = macho["segs"][0]["vmaddr"]
    seg_count, = struct.unpack_from("<I", buf, data_off + starts_off)
    nfix = 0
    for idx in range(seg_count):
        seg_info, = struct.unpack_from("<I", buf, data_off + starts_off + 4 + 4 * idx)
        if not seg_info:
            continue
        base = data_off + starts_off + seg_info
        _, page_size, fmt, _, _, page_count = struct.unpack_from("<IHHQIH", buf, base)
        starts = struct.unpack_from(f"<{page_count}H", buf, base + 22)
        seg = macho["segs"][idx]
        for page_idx, start in enumerate(starts):
            if start == 0xFFFF:
                continue
            if start & 0x8000:
                raise ValueError("DYLD_CHAINED_PTR_START_MULTI is not supported")
            loc = seg["fileoff"] + page_idx * page_size + start
            while True:
                raw, = struct.unpack_from("<Q", buf, loc)
                target, nxt, stride = _decode_chained_ptr(raw, fmt, image_base)
                struct.pack_into("<Q", buf, loc, target)
                nfix += 1
                if not nxt:
                    break
                loc += nxt * stride
    return nfix


def _raw_deflate(data: bytes) -> bytes:
    comp = zlib.compressobj(level=6, wbits=-15)
    return comp.compress(data) + comp.flush()


def build_payload(
    kernel_bytes: bytes,
    adt_bytes: bytes,
    cmdline: str = "debug=0x8 serial=3 -v cpus=1 sweetarm64e=1",
    ramdisk_bytes: Optional[bytes] = None,
    compress: bool = True,
) -> bytes:
    """Build a SWEETXNU v1 or v2 payload from a Mach-O kernel and begonia ADT."""
    kbuf = bytearray(kernel_bytes)
    macho = parse_macho(kbuf)
    apply_chained_fixups(kbuf, macho)

    root, _ = adt.parse(adt_bytes)
    chosen = root.child("chosen")
    if chosen is None:
        raise ValueError("ADT missing /chosen")
    phys_base = int.from_bytes(chosen.prop("dram-base"), "little")
    mem_size = int.from_bytes(chosen.prop("dram-size"), "little")

    load_segs = [s for s in macho["segs"] if s["vmsize"] > 0 and s["name"] != "__PAGEZERO"]
    virt_base = min(s["vmaddr"] for s in load_segs)
    kernel_end = max((s["vmaddr"] + s["vmsize"] + XNU_PAGE - 1) & ~(XNU_PAGE - 1) for s in load_segs)

    blobs: List[Tuple[int, int, bytes, int]] = []
    for s in load_segs:
        raw = bytes(kbuf[s["fileoff"] : s["fileoff"] + s["filesize"]])
        raw_sz = 0
        stored = raw
        if compress and len(raw) >= COMPRESS_THRESHOLD:
            deflated = _raw_deflate(raw)
            if len(deflated) < len(raw):
                stored = deflated
                raw_sz = len(raw)
        blobs.append((s["vmaddr"], s["vmsize"], stored, raw_sz))

    if ramdisk_bytes:
        rd_va = kernel_end
        rd_pa = phys_base + (rd_va - virt_base)
        mem_map = chosen.child("memory-map")
        if mem_map is None:
            raise ValueError("ADT missing /chosen/memory-map")
        mem_map.props.append(("RAMDisk", 0, struct.pack("<QQ", rd_pa, len(ramdisk_bytes))))
        adt_bytes = adt.serialize(root)
        stored_rd = ramdisk_bytes
        raw_rd_sz = 0
        if compress and len(ramdisk_bytes) >= COMPRESS_THRESHOLD:
            deflated = _raw_deflate(ramdisk_bytes)
            if len(deflated) < len(ramdisk_bytes):
                stored_rd = deflated
                raw_rd_sz = len(ramdisk_bytes)
        blobs.append((rd_va, (len(ramdisk_bytes) + XNU_PAGE - 1) & ~(XNU_PAGE - 1), stored_rd, raw_rd_sz))
        kernel_end = (rd_va + len(ramdisk_bytes) + XNU_PAGE - 1) & ~(XNU_PAGE - 1)

    version = 2 if compress else 1
    stride = 40 if version == 2 else 32
    table_end = PAYLOAD_HDR_SIZE + len(blobs) * stride
    adt_off = (table_end + 7) & ~7
    data_off = (adt_off + len(adt_bytes) + 15) & ~15

    out = bytearray(data_off)
    cmd_raw = cmdline.encode("utf-8")[: BOOT_LINE_LENGTH - 1].ljust(BOOT_LINE_LENGTH, b"\0")
    struct.pack_into(
        "<8sII7Q",
        out,
        0,
        b"SWEETXNU",
        version,
        len(blobs),
        virt_base,
        macho["entry"],
        adt_off,
        len(adt_bytes),
        kernel_end,
        phys_base,
        mem_size,
    )
    out[0x48 : 0x48 + BOOT_LINE_LENGTH] = cmd_raw
    out[adt_off : adt_off + len(adt_bytes)] = adt_bytes

    cur = data_off
    for idx, (vmaddr, vmsize, stored, raw_sz) in enumerate(blobs):
        if version == 2:
            struct.pack_into("<5Q", out, PAYLOAD_HDR_SIZE + idx * stride, vmaddr, vmsize, cur, len(stored), raw_sz)
        else:
            struct.pack_into("<4Q", out, PAYLOAD_HDR_SIZE + idx * stride, vmaddr, vmsize, cur, len(stored))
        out += stored
        pad = (-len(stored)) & 15
        if pad:
            out += b"\0" * pad
        cur += len(stored) + pad

    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build SWEETXNU payload from XNU Mach-O and begonia ADT")
    ap.add_argument("kernel", help="Input Mach-O kernel")
    ap.add_argument("adt", help="Input begonia ADT")
    ap.add_argument("out", help="Output payload binary")
    ap.add_argument("--cmdline", default="debug=0x8 serial=3 -v cpus=1 sweetarm64e=1")
    ap.add_argument("--ramdisk", default=None)
    ap.add_argument("--compress", action="store_true")
    args = ap.parse_args()

    with open(args.kernel, "rb") as fp:
        kbytes = fp.read()
    with open(args.adt, "rb") as fp:
        abytes = fp.read()
    if args.ramdisk:
        with open(args.ramdisk, "rb") as fp:
            rd = fp.read()
    else:
        rd = None
    payload = build_payload(kbytes, abytes, cmdline=args.cmdline, ramdisk_bytes=rd, compress=args.compress)
    with open(args.out, "wb") as fp:
        fp.write(payload)
    print(f"{args.out}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
