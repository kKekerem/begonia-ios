#!/usr/bin/env python3
"""Unit tests for SWEETXNU v1/v2 Mach-O payload generation and chained fixups."""
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src", "tools"))
import adt  # noqa: E402
import mkadt_begonia  # noqa: E402
import mkpayload  # noqa: E402


def build_synthetic_macho(text_bytes: bytes, ptr_off_in_text: int = 0x100) -> bytes:
    """Construct a minimal 64-bit Mach-O with __TEXT segment, LC_UNIXTHREAD, and a chained rebase."""
    vmaddr = 0xFFFFFFF007004000
    entry = vmaddr + 0x200
    text_off = 0x4000
    fixup_off = text_off + len(text_bytes)

    seg_cmd = struct.pack(
        "<II16sQQQQIIII",
        mkpayload.LC_SEGMENT_64,
        72,
        b"__TEXT".ljust(16, b"\0"),
        vmaddr,
        0x4000,
        text_off,
        len(text_bytes),
        5,
        5,
        0,
        0,
    )
    # LC_UNIXTHREAD: cmd, cmdsize, flavor(6=ARM_THREAD_STATE64), count(68), 34*u64
    regs = [0] * 34
    regs[32] = entry
    thread_cmd = struct.pack("<IIII", mkpayload.LC_UNIXTHREAD, 16 + 34 * 8, 6, 68) + struct.pack("<34Q", *regs)
    # Minimal LC_DYLD_CHAINED_FIXUPS with 1 segment and 1 page start using DYLD_CHAINED_PTR_64_OFFSET (fmt=6)
    fixup_cmd = struct.pack("<IIII", mkpayload.LC_DYLD_CHAINED_FIXUPS, 16, fixup_off, 128)
    cmds = seg_cmd + thread_cmd + fixup_cmd
    hdr = struct.pack("<8I", mkpayload.MH_MAGIC_64, 0x0100000C, 0, 2, 3, len(cmds), 0, 0)

    buf = bytearray(fixup_off + 128)
    buf[:32] = hdr
    buf[32 : 32 + len(cmds)] = cmds
    buf[text_off : text_off + len(text_bytes)] = text_bytes

    # Encode a chained rebase pointing to vmaddr + 0x340 (offset 0x340, next=0, fmt=6)
    encoded_ptr = 0x340
    struct.pack_into("<Q", buf, text_off + ptr_off_in_text, encoded_ptr)

    # Chained fixups header + starts_in_image + starts_in_segment
    starts_off = 28
    seg_info_off = 8
    struct.pack_into("<7I", buf, fixup_off, 0, starts_off, 0, 0, 0, 0, 6)
    struct.pack_into("<II", buf, fixup_off + starts_off, 1, seg_info_off)
    struct.pack_into("<IHHQIH", buf, fixup_off + starts_off + seg_info_off, 24, 0x4000, 6, 0, 0, 1)
    struct.pack_into("<H", buf, fixup_off + starts_off + seg_info_off + 22, ptr_off_in_text)
    return bytes(buf)


class TestMkPayload(unittest.TestCase):
    def test_v1_and_v2_payload_with_chained_fixups(self) -> None:
        raw_text = (b"BEGONIA_KERNEL_DATA_" * 60000)[: (1 << 20) + 4096]
        macho = build_synthetic_macho(raw_text, ptr_off_in_text=0x100)
        tree = mkadt_begonia.build_begonia_adt(dram_base=0x80000000, dram_size=0x40000000, seed=b"\x11" * 256)
        adt_blob = adt.serialize(tree)

        p2 = mkpayload.build_payload(macho, adt_blob, cmdline="debug=0x8 -v", compress=True)
        magic, ver, nsegs, virt_base, entry_va, adt_off, adt_sz, k_end, phys, memsz = struct.unpack_from(
            "<8sII7Q", p2, 0
        )
        self.assertEqual(magic, b"SWEETXNU")
        self.assertEqual(ver, 2)
        self.assertEqual(nsegs, 1)
        self.assertEqual(phys, 0x80000000)
        self.assertEqual(memsz, 0x40000000)
        self.assertEqual(adt_sz, len(adt_blob))
        self.assertEqual(p2[adt_off : adt_off + adt_sz], adt_blob)
        self.assertEqual(virt_base, 0xFFFFFFF007004000)
        self.assertEqual(entry_va, virt_base + 0x200)
        self.assertGreaterEqual(k_end, virt_base + 0x4000)

        vmaddr, vmsize, foff, fsz, raw_sz = struct.unpack_from("<5Q", p2, mkpayload.PAYLOAD_HDR_SIZE)
        self.assertEqual(vmaddr, virt_base)
        self.assertEqual(raw_sz, len(raw_text))
        inflated = zlib.decompress(p2[foff : foff + fsz], wbits=-15)
        self.assertEqual(len(inflated), len(raw_text))
        rebased_ptr, = struct.unpack_from("<Q", inflated, 0x100)
        self.assertEqual(rebased_ptr, virt_base + 0x340)


if __name__ == "__main__":
    unittest.main()
