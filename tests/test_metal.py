import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class MetalUserspaceTests(unittest.TestCase):
    def test_line_limits_and_files_exist(self):
        metal_dir = ROOT / "src" / "metal"
        expected = [
            metal_dir / "sweet_mali_uapi.h",
            metal_dir / "sweet_drm_mali.c",
            metal_dir / "sweet_drm_syncobj.c",
            metal_dir / "build-begonia-metal.sh",
            metal_dir / "patches" / "begonia-mesa-25.0.7-panfrost.patch",
            metal_dir / "patches" / "chlorophyll-panfrost.patch",
        ]
        for p in expected:
            self.assertTrue(p.is_file(), f"missing {p}")
        for p in expected[:4]:
            lines = p.read_text().splitlines()
            self.assertLess(len(lines), 500, f"{p.name} exceeds 500 lines ({len(lines)})")

    def test_abi_parity_between_xnu_patch_and_uapi(self):
        xnu_patch = (ROOT / "src" / "xnu" / "patches-11215" / "910-begonia-hw-drivers.patch").read_text()
        uapi = (ROOT / "src" / "metal" / "sweet_mali_uapi.h").read_text()
        for sym in (
            "SWEET_MALI_SEL_GEM_CLOSE",
            "SWEET_MALI_SEL_SUBMIT",
            "SWEET_MALI_SEL_WAIT_BO",
            "SWEET_MALI_SEL_CREATE_BO",
            "SWEET_MALI_SEL_MMAP_BO",
            "SWEET_MALI_SEL_GET_PARAM",
            "SWEET_MALI_SEL_GET_BO_OFF",
            "SWEET_MALI_SEL_INFO",
            "SWEET_MALI_SEL_SURFACE_BO",
            "SWEET_MALI_SEL_BO_SYNC",
            "SWEET_MALI_SEL_WAIT_SEQ",
        ):
            m_xnu = re.search(rf"{sym}\s*=\s*(0x[0-9a-fA-F]+)", xnu_patch)
            m_usr = re.search(rf"{sym}\s*=\s*(0x[0-9a-fA-F]+)", uapi)
            self.assertIsNotNone(m_xnu, f"{sym} missing in XNU patch")
            self.assertIsNotNone(m_usr, f"{sym} missing in sweet_mali_uapi.h")
            self.assertEqual(m_xnu.group(1), m_usr.group(1), f"{sym} mismatch")

    def test_patches_contain_panfrost_and_air_hooks(self):
        mesa_patch = (ROOT / "src" / "metal" / "patches" / "begonia-mesa-25.0.7-panfrost.patch").read_text()
        chl_patch = (ROOT / "src" / "metal" / "patches" / "chlorophyll-panfrost.patch").read_text()
        self.assertIn("pan_kmod_bo_import_kms", mesa_patch)
        self.assertIn("panfrost_bo_import_kms", mesa_patch)
        self.assertIn("libbgair", mesa_patch)
        self.assertIn("sweet_hw.c", mesa_patch)
        self.assertIn("GALLIUM_PANFROST", chl_patch)
        self.assertIn("panfrost_create_screen", chl_patch)

    def test_ios18_workflow_cmdline_and_metal_wiring(self):
        wf = (ROOT / ".github" / "workflows" / "ios18-ipsw.yml").read_text()
        self.assertIn("chlorophyll=1", wf)
        self.assertIn("begoniamali=1", wf)
        self.assertIn("begonia-metal-userspace", wf)
        self.assertIn("Chlorophyll.bundle", wf)
        self.assertIn("bgmtl-bb.arm", wf)
        m = re.search(r'--cmdline\s+"([^"]+ufsroot=iosroot[^"]+)"', wf)
        self.assertIsNotNone(m)
        self.assertLessEqual(len(m.group(1)), 608)


if __name__ == "__main__":
    unittest.main()
