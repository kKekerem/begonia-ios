#!/usr/bin/env python3
"""Unit tests for Apple Device Tree (ADT) serialization and begonia board generation."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src", "tools"))
import adt  # noqa: E402
import mkadt_begonia  # noqa: E402


class TestBegoniaADT(unittest.TestCase):
    def test_roundtrip_begonia_adt(self) -> None:
        tree = mkadt_begonia.build_begonia_adt(
            dram_base=0x80000000,
            dram_size=0x40000000,
            cpus_count=8,
            identity="n841ap",
            seed=b"\xAB" * 256,
        )
        blob = adt.serialize(tree)
        parsed, consumed = adt.parse(blob)
        self.assertEqual(consumed, len(blob))
        self.assertEqual(parsed.name, "device-tree")

        compat = parsed.prop("compatible")
        self.assertIsNotNone(compat)
        self.assertIn(b"xiaomi,begonia\0", compat)
        self.assertIn(b"mediatek,MT6785\0", compat)
        self.assertIn(b"N841AP\0", compat)

        chosen = parsed.child("chosen")
        self.assertIsNotNone(chosen)
        self.assertEqual(struct.unpack("<Q", chosen.prop("dram-base"))[0], 0x80000000)
        self.assertEqual(struct.unpack("<Q", chosen.prop("dram-size"))[0], 0x40000000)
        self.assertEqual(len(chosen.prop("random-seed")), 256)

        cpus = parsed.child("cpus")
        self.assertIsNotNone(cpus)
        self.assertEqual(len(cpus.children), 8)
        for i, cpu in enumerate(cpus.children):
            self.assertEqual(struct.unpack("<I", cpu.prop("reg"))[0], i << 8)
            self.assertEqual(struct.unpack("<I", cpu.prop("timebase-frequency"))[0], 13_000_000)

        arm_io = parsed.child("arm-io")
        self.assertIsNotNone(arm_io)
        gic = arm_io.child("gic")
        self.assertIsNotNone(gic)
        self.assertEqual(struct.unpack("<I", gic.prop("virtual-timer-intid"))[0], 27)
        gic_regs = struct.unpack("<4Q", gic.prop("reg"))
        self.assertEqual(gic_regs, (0x0, 0x40000, 0x40000, 0x200000))
        intpol = struct.unpack("<2Q", gic.prop("mtk,intpol-reg"))
        self.assertEqual(intpol, (0x0C53A650, 0x50))

        fbcon = parsed.child("sweet-fbcon")
        self.assertIsNotNone(fbcon)
        self.assertEqual(struct.unpack("<I", fbcon.prop("width"))[0], 1080)
        self.assertEqual(struct.unpack("<I", fbcon.prop("height"))[0], 2340)

        mali = arm_io.child("mali")
        self.assertIsNotNone(mali)
        self.assertEqual(struct.unpack("<2Q", mali.prop("reg")), (0x13040000 - 0x0C000000, 0x10000))

        sweet_ufs = parsed.child("sweet-ufs")
        self.assertIsNotNone(sweet_ufs)
        self.assertEqual(struct.unpack("<2Q", sweet_ufs.prop("reg")), (0x11270000, 0x2300))

    def test_default_dram_matches_xnu_l2_alignment(self) -> None:
        self.assertEqual(mkadt_begonia.DEFAULT_DRAM_BASE & 0x1FFFFFF, 0x01004000)
        self.assertEqual((mkadt_begonia.DEFAULT_DRAM_BASE + mkadt_begonia.DEFAULT_DRAM_SIZE) & 0x3FFF, 0)


if __name__ == "__main__":
    unittest.main()
