#!/usr/bin/env python3
"""
Tests for the compiled bare-metal mtkloader.bin and host-compiled C units
(RFC-1951 inflate_raw and FDT parser).
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "src", "tools"))
import pack_bootimg  # noqa: E402


class TestLoaderAndCUnits(unittest.TestCase):
    def test_compiled_mtkloader_binary_if_present(self) -> None:
        bin_path = os.path.join(ROOT, "build", "mtkloader.bin")
        if not os.path.exists(bin_path):
            self.skipTest("build/mtkloader.bin not built yet")
        with open(bin_path, "rb") as fp:
            data = fp.read()
        self.assertGreater(len(data), 1024)
        self.assertLess(len(data), pack_bootimg.CHAIN_OFFSET)
        self.assertEqual(data[0x38:0x3C], b"ARM\x64")
        text_off, = struct.unpack_from("<Q", data, 0x08)
        self.assertEqual(text_off, 0x8000)

    def test_c_inflate_and_fdt_on_host_compiler(self) -> None:
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no host C compiler available")

        src_dir = os.path.join(ROOT, "src", "mtkloader")
        inc_dir = os.path.join(src_dir, "include")
        harness_c = """
#include <stdint.h>
#include "inflate.h"
#include "fdt.h"

/* Minimal stubs required by fdt.c debug output */
void con_putc(char c) { (void)c; }
void con_puts(const char *s) { (void)s; }
void con_hex(uint64_t v) { (void)v; }

int test_inflate_wrapper(const uint8_t *in, uint64_t in_len, uint8_t *out, uint64_t *out_len) {
    return inflate_raw(in, in_len, out, out_len);
}

int test_fdt_model(const uint8_t *dtb, char *buf, uint32_t cap) {
    return fdt_root_model((uintptr_t)dtb, buf, cap);
}

uintptr_t test_fdt_chosen_prop(const uint8_t *dtb, const char *prop, uint32_t *len) {
    return fdt_chosen_prop((uintptr_t)dtb, prop, len);
}
"""
        with tempfile.TemporaryDirectory() as td:
            h_path = os.path.join(td, "harness.c")
            so_path = os.path.join(td, "libloader_test.so")
            with open(h_path, "w", encoding="utf-8") as fp:
                fp.write(harness_c)
            subprocess.check_call(
                [
                    cc,
                    "-shared",
                    "-fPIC",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{inc_dir}",
                    h_path,
                    os.path.join(src_dir, "inflate.c"),
                    os.path.join(src_dir, "fdt.c"),
                    "-o",
                    so_path,
                ]
            )
            lib = ctypes.CDLL(so_path)
            lib.test_inflate_wrapper.argtypes = [
                ctypes.c_char_p,
                ctypes.c_uint64,
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_uint64),
            ]
            lib.test_inflate_wrapper.restype = ctypes.c_int

            # Verify stored, fixed Huffman, and dynamic Huffman DEFLATE streams
            for raw in [
                b"A" * 512,
                bytes(range(256)) * 64,
                (b"begonia-mt6785-xnu11215-" * 2000) + os.urandom(4096),
            ]:
                comp = zlib.compressobj(level=6, wbits=-15)
                zdata = comp.compress(raw) + comp.flush()
                out_buf = ctypes.create_string_buffer(len(raw) + 64)
                out_len = ctypes.c_uint64(len(raw) + 64)
                rc = lib.test_inflate_wrapper(zdata, len(zdata), out_buf, ctypes.byref(out_len))
                self.assertEqual(rc, 0)
                self.assertEqual(out_len.value, len(raw))
                self.assertEqual(out_buf.raw[: out_len.value], raw)

            # Verify FDT parser against build_minimal_fdt()
            dtb = pack_bootimg.build_minimal_fdt("Redmi Note 8 Pro")
            lib.test_fdt_model.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
            lib.test_fdt_model.restype = ctypes.c_int
            model_buf = ctypes.create_string_buffer(64)
            self.assertEqual(lib.test_fdt_model(dtb, model_buf, 64), 1)
            self.assertEqual(model_buf.value, b"Redmi Note 8 Pro")

            lib.test_fdt_chosen_prop.argtypes = [
                ctypes.c_char_p,
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_uint32),
            ]
            lib.test_fdt_chosen_prop.restype = ctypes.c_uint64
            plen = ctypes.c_uint32(0)
            ptr = lib.test_fdt_chosen_prop(dtb, b"bootargs", ctypes.byref(plen))
            self.assertNotEqual(ptr, 0)
            self.assertEqual(plen.value, len(b"begonia.diag=1\0"))


if __name__ == "__main__":
    unittest.main()
