#!/usr/bin/env python3
"""Unit tests for Android boot image v2 standalone and dual-boot chainboot packing."""
import gzip
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src", "tools"))
import pack_bootimg  # noqa: E402


def make_synthetic_loader(size: int = 16384) -> bytes:
    buf = bytearray(size)
    struct.pack_into("<I", buf, 0, 0x14000010)  # b +0x40
    buf[0x38:0x3C] = b"ARM\x64"
    return bytes(buf)


class TestPackBootImg(unittest.TestCase):
    def test_standalone_and_chainboot_v2(self) -> None:
        loader = make_synthetic_loader()
        fake_android_img = make_synthetic_loader(65536)
        struct.pack_into("<Q", bytearray(fake_android_img), 0x10, 0x100000)

        with tempfile.TemporaryDirectory() as td:
            standalone_path = os.path.join(td, "diag.img")
            img = pack_bootimg.pack_standalone(loader, standalone_path)
            self.assertEqual(img[:8], b"ANDROID!")
            ksize, kaddr, rsize, raddr, _, _, tags, page, hver = struct.unpack_from("<9I", img, 8)
            self.assertEqual(kaddr, pack_bootimg.BEGONIA_KERNEL_ADDR)
            self.assertEqual(raddr, pack_bootimg.BEGONIA_RAMDISK_ADDR)
            self.assertEqual(tags, pack_bootimg.BEGONIA_TAGS_ADDR)
            self.assertEqual(page, 2048)
            self.assertEqual(hver, 2)
            decompressed = gzip.decompress(img[page : page + ksize])
            self.assertEqual(decompressed[0x38:0x3C], b"ARM\x64")

            # Use the standalone image as a synthetic stock boot image for chainboot test
            stock_k = gzip.compress(fake_android_img, mtime=0)
            stock_img = pack_bootimg.build_bootimg_v2(
                kernel_gz=stock_k,
                ramdisk=b"RAMDISK",
                second=b"",
                dtb=pack_bootimg.build_minimal_fdt("stock-begonia"),
            )
            chain_path = os.path.join(td, "dualboot.img")
            fake_payload = b"SWEETXNU" + b"\x00" * 1024
            chained = pack_bootimg.pack_chainboot(stock_img, loader, chain_path, payload_bin=fake_payload)
            cksize = struct.unpack_from("<I", chained, 8)[0]
            blob = gzip.decompress(chained[page : page + cksize])
            res2, res3, res4 = struct.unpack_from("<3Q", blob, 0x20)
            self.assertEqual(res2, len(fake_android_img))
            self.assertEqual(res3, pack_bootimg.CHAIN_OFFSET + len(fake_android_img))
            self.assertEqual(res4, len(fake_payload))
            self.assertEqual(blob[res3 : res3 + res4], fake_payload)
            self.assertEqual(blob[pack_bootimg.CHAIN_OFFSET : pack_bootimg.CHAIN_OFFSET + res2], fake_android_img)

    def test_chainboot_preserves_ramdisk_and_dtb_without_payload(self) -> None:
        loader = make_synthetic_loader()
        fake_android_img = make_synthetic_loader(32768)
        stock_ramdisk = b"RAMDISK-DATA" * 100
        stock_dtb = pack_bootimg.build_minimal_fdt("begonia-stock-test")
        stock_img = pack_bootimg.build_bootimg_v2(
            kernel_gz=gzip.compress(fake_android_img, mtime=0),
            ramdisk=stock_ramdisk,
            second=b"",
            dtb=stock_dtb,
        )
        with tempfile.TemporaryDirectory() as td:
            out_path = os.path.join(td, "preview.img")
            out = pack_bootimg.pack_chainboot(stock_img, loader, out_path, payload_bin=None)
            ks, _, rs, _, ss, _, _, pg, hv = struct.unpack_from("<9I", out, 8)
            rdo, = struct.unpack_from("<I", out, 1632)
            dtb_sz, = struct.unpack_from("<I", out, 1648)
            self.assertEqual(hv, 2)
            self.assertEqual(rs, len(stock_ramdisk))
            self.assertEqual(rdo, 0)
            self.assertEqual(dtb_sz, len(stock_dtb))
            pgs = lambda n: ((n + pg - 1) // pg) * pg  # noqa: E731
            rd_off = pg + pgs(ks)
            self.assertEqual(out[rd_off : rd_off + rs], stock_ramdisk)
            dtb_off = rd_off + pgs(rs) + pgs(ss)
            self.assertEqual(out[dtb_off : dtb_off + dtb_sz], stock_dtb)

            # Also verify recovery_dtbo extraction, placement, offset update, and v2 SHA1
            fake_dtbo = b"\xd7\xb7\xab\x1e" + b"\xaa" * 5000
            rec_src = pack_bootimg.build_bootimg_v2(
                kernel_gz=gzip.compress(fake_android_img, mtime=0),
                ramdisk=stock_ramdisk,
                second=b"",
                dtb=stock_dtb,
                recovery_dtbo=fake_dtbo,
            )
            extracted = pack_bootimg.extract_recovery_dtbo(rec_src)
            self.assertEqual(extracted, fake_dtbo)
            rec_out_path = os.path.join(td, "rec-preview.img")
            rec_out = pack_bootimg.pack_chainboot(
                stock_img, loader, rec_out_path, payload_bin=None, recovery_dtbo=extracted
            )
            rks, _, rrs, _, rss, _, _, rpg, _ = struct.unpack_from("<9I", rec_out, 8)
            rrdo_sz, rrdo_addr = struct.unpack_from("<IQ", rec_out, 1632)
            rdtb_sz, = struct.unpack_from("<I", rec_out, 1648)
            rpgs = lambda n: ((n + rpg - 1) // rpg) * rpg  # noqa: E731
            expected_rdo_off = rpg + rpgs(rks) + rpgs(rrs) + rpgs(rss)
            self.assertEqual(rrdo_sz, len(fake_dtbo))
            self.assertEqual(rrdo_addr, expected_rdo_off)
            self.assertEqual(rec_out[rrdo_addr : rrdo_addr + rrdo_sz], fake_dtbo)
            rec_dtb_off = expected_rdo_off + rpgs(rrdo_sz)
            self.assertEqual(rec_out[rec_dtb_off : rec_dtb_off + rdtb_sz], stock_dtb)


if __name__ == "__main__":
    unittest.main()
