#!/usr/bin/env python3
"""
Pack mtkloader.bin into an Android boot image v2 for Redmi Note 8 Pro (begonia).

Supports:
  --standalone: creates a fastboot-bootable diagnostic or ramdisk-payload boot.img
  --chainboot:  prepends mtkloader before stock Android's Image for dual-boot
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import struct
from typing import Optional

# begonia BoardConfig.mk constants
BEGONIA_PAGE_SIZE = 2048
BEGONIA_KERNEL_ADDR = 0x40080000
BEGONIA_RAMDISK_ADDR = 0x47C80000
BEGONIA_SECOND_ADDR = 0x40F00000
BEGONIA_TAGS_ADDR = 0x4BC80000
BEGONIA_DTB_ADDR = 0x4BC80000
BEGONIA_BOOT_PARTITION = 64 << 20
CHAIN_OFFSET = 0x40000


def _pad(buf: bytes, align: int) -> bytes:
    rem = len(buf) % align
    return buf if rem == 0 else buf + b"\0" * (align - rem)


def build_minimal_fdt(model: str = "begonia-diag") -> bytes:
    """
    Create a tiny valid FDT v17 containing /model and /chosen so standalone test
    boot images have a valid DTB even before extracting the phone's stock DTB.
    """
    strings = b"model\0compatible\0bootargs\0"
    struct_blk = bytearray()
    # Root node
    struct_blk += struct.pack(">I", 1) + b"\0\0\0\0"
    model_b = model.encode("utf-8") + b"\0"
    compat_b = b"xiaomi,begonia\0mediatek,MT6785\0"
    struct_blk += struct.pack(">III", 3, len(model_b), 0) + _pad(model_b, 4)
    struct_blk += struct.pack(">III", 3, len(compat_b), 6) + _pad(compat_b, 4)
    # /chosen
    struct_blk += struct.pack(">I", 1) + _pad(b"chosen\0", 4)
    bootargs_b = b"begonia.diag=1\0"
    struct_blk += struct.pack(">III", 3, len(bootargs_b), 17) + _pad(bootargs_b, 4)
    struct_blk += struct.pack(">I", 2)  # end /chosen
    struct_blk += struct.pack(">I", 2)  # end root
    struct_blk += struct.pack(">I", 9)  # FDT_END

    hdr_size = 40
    rsv = b"\0" * 16
    off_struct = hdr_size + len(rsv)
    off_strings = off_struct + len(struct_blk)
    total_size = off_strings + len(strings)
    hdr = struct.pack(
        ">10I",
        0xD00DFEED,
        total_size,
        off_struct,
        off_strings,
        hdr_size,
        17,
        16,
        0,
        len(strings),
        len(struct_blk),
    )
    return hdr + rsv + bytes(struct_blk) + strings


def _patch_loader_header(
    loader: bytes,
    span_size: int,
    android_len: int,
    payload_off: int,
    payload_len: int,
) -> bytes:
    """Patch ARM64 Image header size and custom chainboot/payload fields (res2..res4)."""
    if loader[0x38:0x3C] != b"ARM\x64":
        raise ValueError("loader missing ARM64 Image magic")
    buf = bytearray(loader)
    struct.pack_into("<Q", buf, 0x10, (span_size + 0xFFFFF) & ~0xFFFFF)
    struct.pack_into("<Q", buf, 0x20, android_len)   # res2: hdr[4]
    struct.pack_into("<Q", buf, 0x28, payload_off)   # res3: hdr[5]
    struct.pack_into("<Q", buf, 0x30, payload_len)   # res4: hdr[6]
    return bytes(buf)


def build_bootimg_v2(
    kernel_gz: bytes,
    ramdisk: bytes,
    second: bytes,
    dtb: bytes,
    recovery_dtbo: bytes = b"",
    cmdline: str = "bootopt=64S3,32N2,64N2",
    page_size: int = BEGONIA_PAGE_SIZE,
    header_template: Optional[bytes] = None,
) -> bytes:
    def pages(n: int) -> int:
        return ((n + page_size - 1) // page_size) * page_size

    rdo_offset = (
        page_size + pages(len(kernel_gz)) + pages(len(ramdisk)) + pages(len(second))
        if recovery_dtbo
        else 0
    )

    if header_template is not None:
        hdr = bytearray(header_template[:page_size])
        struct.pack_into("<I", hdr, 8, len(kernel_gz))
        struct.pack_into("<I", hdr, 16, len(ramdisk))
        struct.pack_into("<I", hdr, 24, len(second))
        struct.pack_into("<IQ", hdr, 1632, len(recovery_dtbo), rdo_offset)
        struct.pack_into("<I", hdr, 1648, len(dtb))
    else:
        hdr = bytearray(page_size)
        struct.pack_into(
            "<8s10I16s512s",
            hdr,
            0,
            b"ANDROID!",
            len(kernel_gz),
            BEGONIA_KERNEL_ADDR,
            len(ramdisk),
            BEGONIA_RAMDISK_ADDR,
            len(second),
            BEGONIA_SECOND_ADDR,
            BEGONIA_TAGS_ADDR,
            page_size,
            2,  # header_version = 2
            0,  # os_version
            b"begonia-ios".ljust(16, b"\0"),
            cmdline.encode("utf-8")[:511].ljust(512, b"\0"),
        )
        struct.pack_into("<IQI", hdr, 1632, len(recovery_dtbo), rdo_offset, 1660)
        struct.pack_into("<IQ", hdr, 1648, len(dtb), BEGONIA_DTB_ADDR)

    sha = hashlib.sha1()
    for part in (kernel_gz, ramdisk, second, recovery_dtbo, dtb):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    hdr[576:608] = sha.digest().ljust(32, b"\0")

    out = (
        bytes(hdr)
        + _pad(kernel_gz, page_size)
        + _pad(ramdisk, page_size)
        + _pad(second, page_size)
        + _pad(recovery_dtbo, page_size)
        + _pad(dtb, page_size)
    )
    if len(out) > BEGONIA_BOOT_PARTITION:
        raise ValueError(f"boot image ({len(out)} B) exceeds begonia 64 MiB boot partition")
    return out


def pack_standalone(
    loader_bin: bytes,
    out_path: str,
    payload_bin: Optional[bytes] = None,
    dtb_bin: Optional[bytes] = None,
    recovery_dtbo: bytes = b"",
) -> bytes:
    dtb = dtb_bin if dtb_bin is not None else build_minimal_fdt()
    ramdisk = payload_bin if payload_bin is not None else b""
    patched = _patch_loader_header(loader_bin, len(loader_bin), 0, 0, 0)
    kernel_gz = gzip.compress(patched, compresslevel=9, mtime=0)
    img = build_bootimg_v2(kernel_gz=kernel_gz, ramdisk=ramdisk, second=b"", dtb=dtb, recovery_dtbo=recovery_dtbo)
    with open(out_path, "wb") as fp:
        fp.write(img)
    return img


def extract_recovery_dtbo(recovery_img: bytes) -> bytes:
    """Extract recovery_dtbo section from an existing v1/v2 Android recovery image."""
    if recovery_img[:8] != b"ANDROID!":
        raise ValueError("recovery image is not an Android boot image")
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from("<9I", recovery_img, 8)
    if hver < 1:
        return b""
    rdo_size, = struct.unpack_from("<I", recovery_img, 1632)
    if rdo_size == 0:
        return b""

    def pages(n: int) -> int:
        return ((n + page - 1) // page) * page

    off = page + pages(ksize) + pages(rsize) + pages(ssize)
    return recovery_img[off : off + rdo_size]


def pack_chainboot(
    stock_boot_img: bytes,
    loader_bin: bytes,
    out_path: str,
    payload_bin: Optional[bytes] = None,
    recovery_dtbo: bytes = b"",
) -> bytes:
    if stock_boot_img[:8] != b"ANDROID!":
        raise ValueError("stock image is not an Android boot image")
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from("<9I", stock_boot_img, 8)
    if hver != 2:
        raise ValueError(f"expected boot image header v2, got v{hver}")
    rdo_size, = struct.unpack_from("<I", stock_boot_img, 1632)
    dtb_size, = struct.unpack_from("<I", stock_boot_img, 1648)

    def pages(n: int) -> int:
        return ((n + page - 1) // page) * page

    off = page
    kernel = stock_boot_img[off : off + ksize]; off += pages(ksize)
    ramdisk = stock_boot_img[off : off + rsize]; off += pages(rsize)
    second = stock_boot_img[off : off + ssize]; off += pages(ssize)
    rdo = stock_boot_img[off : off + rdo_size]; off += pages(rdo_size)
    dtb = stock_boot_img[off : off + dtb_size]

    if recovery_dtbo:
        rdo = recovery_dtbo

    android_img = gzip.decompress(kernel) if kernel[:2] == b"\x1f\x8b" else kernel
    if android_img[0x38:0x3C] != b"ARM\x64":
        raise ValueError("stock Android kernel is not an ARM64 Image")
    if len(loader_bin) > CHAIN_OFFSET:
        raise ValueError(f"loader ({len(loader_bin)} B) exceeds CHAIN_OFFSET ({CHAIN_OFFSET} B)")

    android_image_size, = struct.unpack_from("<Q", android_img, 0x10)
    padded_android = _pad(android_img, 16)
    payload_off = CHAIN_OFFSET + len(padded_android) if payload_bin else 0
    payload_len = len(payload_bin) if payload_bin else 0
    total_span = max(CHAIN_OFFSET + android_image_size, payload_off + payload_len)

    prog = _patch_loader_header(loader_bin, total_span, len(android_img), payload_off, payload_len)

    blob = prog + b"\0" * (CHAIN_OFFSET - len(prog)) + padded_android
    if payload_bin:
        blob += payload_bin
    kernel_gz = gzip.compress(blob, compresslevel=9, mtime=0)
    img = build_bootimg_v2(
        kernel_gz=kernel_gz,
        ramdisk=ramdisk,
        second=second,
        dtb=dtb,
        recovery_dtbo=rdo,
        page_size=page,
        header_template=stock_boot_img,
    )
    with open(out_path, "wb") as fp:
        fp.write(img)
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description="Pack begonia Android boot v2 image")
    ap.add_argument("loader", help="Compiled mtkloader.bin")
    ap.add_argument("out", help="Output boot.img")
    ap.add_argument("--stock-boot", default=None, help="Stock begonia boot.img for chainboot dual-boot")
    ap.add_argument("--stock-recovery", default=None, help="Optional stock recovery.img to preserve recovery_dtbo")
    ap.add_argument("--payload", default=None, help="Optional SWEETXNU payload")
    ap.add_argument("--dtb", default=None, help="Optional DTB file for standalone mode")
    args = ap.parse_args()

    with open(args.loader, "rb") as fp:
        loader = fp.read()
    if args.payload:
        with open(args.payload, "rb") as fp:
            payload = fp.read()
    else:
        payload = None

    rec_dtbo = b""
    if args.stock_recovery:
        with open(args.stock_recovery, "rb") as fp:
            rec_dtbo = extract_recovery_dtbo(fp.read())

    if args.stock_boot:
        with open(args.stock_boot, "rb") as fp:
            stock = fp.read()
        img = pack_chainboot(stock, loader, args.out, payload_bin=payload, recovery_dtbo=rec_dtbo)
    else:
        if args.dtb:
            with open(args.dtb, "rb") as fp:
                dtb = fp.read()
        else:
            dtb = None
        img = pack_standalone(loader, args.out, payload_bin=payload, dtb_bin=dtb, recovery_dtbo=rec_dtbo)

    print(f"{args.out}: {len(img)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
