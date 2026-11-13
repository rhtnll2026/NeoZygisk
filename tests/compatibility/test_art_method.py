import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ArtMethodInitializationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        if os.environ.get("JNI_INCLUDE_DIR"):
            include = Path(os.environ["JNI_INCLUDE_DIR"])
        elif os.environ.get("JAVA_HOME"):
            include = Path(os.environ["JAVA_HOME"]) / "include"
        else:
            javac = shutil.which("javac")
            if not javac:
                raise RuntimeError("A JDK with JNI headers is required (set JAVA_HOME)")
            include = Path(javac).resolve().parents[1] / "include"
        cls.binary = Path(cls.build.name) / "art_method_test"
        # This is a JNI mock, not an Android/ART VM or device test.
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=undefined", "-fno-sanitize-recover=all", "-g",
            "-I" + str(include), "-I" + str(include / ("darwin" if sys.platform == "darwin" else "linux")),
            "-I" + str(ROOT / "tests/host/include"),
            "-I" + str(ROOT / "loader/src/include"),
            "-I" + str(ROOT / "loader/src/injector"),
            str(ROOT / "tests/compatibility/art_method_test.cpp"), "-o", str(cls.binary),
        ], check=True)

    def check_scenario(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True,
                                timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _scenario in (
    "success", "missing_executable", "missing_field", "missing_throwable",
    "missing_class", "missing_method", "missing_array", "array_exception",
    "short_array", "length_exception", "missing_element", "element_exception",
    "pointer_exception", "null_pointer", "reversed_methods", "short_method", "unaligned_method",
    "distant_methods",
):
    setattr(ArtMethodInitializationTests, "test_" + _scenario,
            lambda self, scenario=_scenario: self.check_scenario(scenario))


if __name__ == "__main__":
    unittest.main()
