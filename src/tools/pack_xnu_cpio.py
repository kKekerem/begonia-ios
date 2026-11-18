#!/usr/bin/env python3
"""Boot image that LK on begonia accepts and that still carries a SWEETXNU payload.

begonia's LK crashes (lk_crash, cold PMIC reset, nothing on screen) when
  - the ramdisk is not a gzip Android ramdisk (a raw payload as ramdisk), or
  - the compressed kernel section passes ~16 MB (Android Image + payload in the kernel), or
  - the kernel section has no chained Android Image (tiny standalone loader).
So the kernel section stays mtkloader + the stock Android Image (as pack_bootimg.py chainboot
makes it), and the payload becomes the first file ("xnu.sweet") of the newc cpio ramdisk, in front
of the stock Android ramdisk, gzip'd again. mtkloader (find_gz_cpio_payload) inflates it.

Usage: pack_xnu_cpio.py <mtkloader.bin> <payload.bin> <stock boot.img> <out.img>
                        [--stock-recovery <img with recovery_dtbo>]
"""
import argparse
import gzip
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_bootimg as pb  # noqa: E402

CPIO_NAME = "xnu.sweet"  # 9 chars + NUL = 10: data at offset 120, 8-byte aligned


def newc_entry(name, data, mode=0o100644, ino=1):
    namesize = len(name) + 1
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, namesize, 0)
    hdr = b"070701" + b"".join(b"%08X" % f for f in fields)
    blob = hdr + name.encode() + b"\0"
    blob += b"\0" * (-len(blob) % 4)
    assert len(blob) % 8 == 0, "payload data must start 8-byte aligned"
    blob += data + b"\0" * (-len(data) % 4)
    return blob


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("loader")
    ap.add_argument("payload")
    ap.add_argument("stock_boot")
    ap.add_argument("out")
    ap.add_argument("--stock-recovery", default=None)
    a = ap.parse_args()

    stock = open(a.stock_boot, "rb").read()
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from("<9I", stock, 8)
    if stock[:8] != b"ANDROID!" or hver != 2:
        sys.exit("stock image must be an Android boot image header v2")
    off = page + -(-ksize // page) * page
    ramdisk = stock[off:off + rsize]
    if ramdisk[:2] != b"\x1f\x8b":
        sys.exit("stock ramdisk is not gzip")
    android_cpio = gzip.decompress(ramdisk)
    if android_cpio[:6] != b"070701":
        sys.exit("stock ramdisk is not a newc cpio")

    payload = open(a.payload, "rb").read()
    if payload[:8] != b"SWEETXNU":
        sys.exit("payload lacks SWEETXNU magic")
    new_ramdisk = gzip.compress(newc_entry(CPIO_NAME, payload) + android_cpio, compresslevel=9, mtime=0)

    rdo = b""
    if a.stock_recovery:
        rdo = pb.extract_recovery_dtbo(open(a.stock_recovery, "rb").read())
    loader = open(a.loader, "rb").read()
    # Chainboot image (loader + Android Image, stock ramdisk), then swap the ramdisk.
    tmp = a.out + ".chain.tmp"
    pb.pack_chainboot(stock, loader, tmp, payload_bin=None, recovery_dtbo=rdo)
    chain = open(tmp, "rb").read()
    os.remove(tmp)
    ck, _, _, _, _, _, _, cpage, _ = struct.unpack_from("<9I", chain, 8)
    koff = cpage
    kernel_gz = chain[koff:koff + ck]
    rdo_size, = struct.unpack_from("<I", chain, 1632)
    dtb_size, = struct.unpack_from("<I", chain, 1648)
    rk = -(-ck // cpage) * cpage
    rr = -(-struct.unpack_from("<I", chain, 16)[0] // cpage) * cpage
    o = cpage + rk + rr
    rdo_bytes = chain[o:o + rdo_size]
    o += -(-rdo_size // cpage) * cpage
    dtb = chain[o:o + dtb_size]
    img = pb.build_bootimg_v2(kernel_gz=kernel_gz, ramdisk=new_ramdisk, second=b"", dtb=dtb,
                              recovery_dtbo=rdo_bytes, page_size=cpage, header_template=chain)
    with open(a.out, "wb") as fp:
        fp.write(img)
    print("%s: %d bytes (kernel %d, ramdisk %d gzip with %s %d B)" % (
        a.out, len(img), len(kernel_gz), len(new_ramdisk), CPIO_NAME, len(payload)))


if __name__ == "__main__":
    main()
