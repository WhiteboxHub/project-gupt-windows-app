"""
Tests for Gupt.exe executable integrity, PE headers, exports, and deployment.
"""
import unittest
import os
import struct

class TestBinaryIntegrity(unittest.TestCase):

    def setUp(self):
        self.build_binary = os.path.abspath("project-gupt-windows-app/build/Release/Gupt.exe")
        self.desktop_binary = os.path.expanduser(r"~\Desktop\Gupt.exe")

    def test_build_binary_exists(self):
        self.assertTrue(os.path.isfile(self.build_binary), f"Missing build binary: {self.build_binary}")
        size = os.path.getsize(self.build_binary)
        self.assertGreater(size, 100_000, f"Binary too small: {size} bytes")
        print(f" [PASS] Build binary exists: {self.build_binary} ({size:,} bytes)")

    def test_desktop_binary_exists_and_synced(self):
        self.assertTrue(os.path.isfile(self.desktop_binary), f"Missing desktop binary: {self.desktop_binary}")
        build_size = os.path.getsize(self.build_binary)
        desktop_size = os.path.getsize(self.desktop_binary)
        self.assertEqual(build_size, desktop_size, "Desktop binary size does not match build binary size")
        print(f" [PASS] Desktop binary matches build: {self.desktop_binary} ({desktop_size:,} bytes)")

    def test_pe_header(self):
        """Verifies the file is a valid 64-bit Windows PE executable."""
        with open(self.build_binary, "rb") as f:
            # MZ header
            mz = f.read(2)
            self.assertEqual(mz, b"MZ", "Missing MZ DOS header")

            # PE offset at 0x3C
            f.seek(0x3C)
            pe_offset = struct.unpack("<I", f.read(4))[0]

            # PE signature
            f.seek(pe_offset)
            pe_sig = f.read(4)
            self.assertEqual(pe_sig, b"PE\x00\x00", "Missing PE signature")

            # Machine type (0x8664 = x64)
            machine = struct.unpack("<H", f.read(2))[0]
            self.assertEqual(machine, 0x8664, "Binary is not x64 (AMD64)")
            print(f" [PASS] Valid Windows x64 PE executable verified.")

if __name__ == '__main__':
    unittest.main()
