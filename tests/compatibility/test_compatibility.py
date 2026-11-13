import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from check_elf import ABIS, PT_GNU_RELRO, check_elf, check_zip

ROOT = Path(__file__).resolve().parents[2]


def elf64(align=16384, address=0, relro_size=None, mem_size=32768, machine=183):
    count = 1 + (relro_size is not None)
    data = bytearray(256)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, machine, 1, 0, 64, 0, 0,
                     64, 56, count, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 6, 0, address, 0, 256, mem_size, align)
    if relro_size is not None:
        struct.pack_into("<IIQQQQQQ", data, 120, PT_GNU_RELRO, 4, 0, address,
                         0, 256, relro_size, 1)
    return data


class ElfCompatibilityTests(unittest.TestCase):
    def test_16k_and_64k_alignment(self):
        for align in (16384, 65536):
            check_elf(elf64(align), "arm64-v8a")

    def test_reject_4k_alignment(self):
        with self.assertRaisesRegex(ValueError, "PT_LOAD"):
            check_elf(elf64(4096), "arm64-v8a")

    def test_reject_noncongruent_load(self):
        with self.assertRaisesRegex(ValueError, "PT_LOAD"):
            check_elf(elf64(address=4096), "arm64-v8a")

    def test_reject_non_power_of_two(self):
        with self.assertRaisesRegex(ValueError, "PT_LOAD"):
            check_elf(elf64(24576), "arm64-v8a")

    def test_reject_relro_with_writable_tail(self):
        with self.assertRaisesRegex(ValueError, "RELRO"):
            check_elf(elf64(relro_size=4096), "arm64-v8a")

    def test_aligned_relro_with_writable_tail(self):
        check_elf(elf64(relro_size=16384), "arm64-v8a")

    def test_relro_occupies_entire_load(self):
        check_elf(elf64(relro_size=4096, mem_size=4096), "arm64-v8a")

    def test_wrong_machine(self):
        with self.assertRaisesRegex(ValueError, "expected an Android"):
            check_elf(elf64(), "x86_64")
        check_elf(elf64(machine=62), "x86_64")

    def test_truncated_and_non_elf(self):
        for data in (b"", b"not ELF", elf64()[:20], elf64()[:100]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                check_elf(data, "arm64-v8a")

    def test_reject_bad_segment_bounds(self):
        data = elf64()
        struct.pack_into("<Q", data, 64 + 32, 100000)
        with self.assertRaisesRegex(ValueError, "bounds"):
            check_elf(data, "arm64-v8a")

    def test_missing_packaged_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "empty.zip")
            with zipfile.ZipFile(path, "w"):
                pass
            with self.assertRaisesRegex(ValueError, "expected exactly one"):
                check_zip(path)

    def test_32bit_4k_binary(self):
        for abi, (elf_class, machine) in ABIS.items():
            if elf_class != 1:
                continue
            data = bytearray(128)
            data[:7] = b"\x7fELF\x01\x01\x01"
            struct.pack_into("<HHIIIIIHHHHHH", data, 16, 3, machine, 1, 0, 52,
                             0, 0, 52, 32, 1, 0, 0, 0)
            struct.pack_into("<IIIIIIII", data, 52, 1, 0, 0, 0, 128, 128, 5, 4096)
            check_elf(data, abi)


class JniCompatibilityTests(unittest.TestCase):
    def assert_android16_signatures(self, has_fifo_ui):
        # Pinned AOSP release/QPR revisions and parameter changes are recorded in
        # ANDROID16.md. QPR2 adds useFifoUi; the other two entry points are unchanged.
        top_app = "ZZ" if has_fifo_ui else "Z"
        expected = {
            "nativeForkAndSpecialize": "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;Ljava/lang/String;" + top_app + "[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
            "nativeSpecializeAppProcess": "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)V",
            "nativeForkSystemServer": "(II[II[[IJJ)I",
        }
        header = (ROOT / "loader/src/injector/jni_hooks.hpp").read_text()
        entries = set(re.findall(r'"(native\w+)",\s*"([^"]+)"', header))
        for name, signature in expected.items():
            with self.subTest(method=name):
                self.assertIn((name, signature), entries)

    def test_android16_release_signatures(self):
        self.assert_android16_signatures(has_fifo_ui=False)

    def test_android16_qpr1_signatures(self):
        self.assert_android16_signatures(has_fifo_ui=False)

    def test_android16_qpr2_signatures(self):
        self.assert_android16_signatures(has_fifo_ui=True)

    def test_aosp_android17_signatures(self):
        # Derived from native declarations in AOSP android17-release Zygote.java.
        # These check signature coverage, not successful hooks on a running device.
        expected = {
            "nativeForkAndSpecialize": "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;Ljava/lang/String;ZZ[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
            "nativeSpecializeAppProcess": "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)V",
            "nativeForkSystemServer": "(II[II[[IJJ)I",
        }
        header = (ROOT / "loader/src/injector/jni_hooks.hpp").read_text()
        entries = set(re.findall(r'"(native\w+)",\s*"([^"]+)"', header))
        for name, signature in expected.items():
            with self.subTest(method=name):
                self.assertIn((name, signature), entries)

    def test_generated_header_is_current(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.executable, str(ROOT / "loader/src/injector/gen_jni_hooks.py")],
                           cwd=directory, check=True)
            self.assertEqual(Path(directory, "jni_hooks.hpp").read_bytes(),
                             (ROOT / "loader/src/injector/jni_hooks.hpp").read_bytes())


if __name__ == "__main__":
    unittest.main()
