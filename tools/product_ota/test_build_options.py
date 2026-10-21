"""Exercise the real product CMake module without configuring ESP-IDF or compiling."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec


REPOSITORY = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake is not installed")
class BuildOptionsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="product-ota-cmake-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.child = self.source / "esp32"
        self.child.mkdir()
        self.managed = self.root / "application.bin"
        self.managed.write_bytes(b"MSSpot1\0" + bytes(range(256)))
        self.public_key = self.root / "test-public.pem"
        # No private key is persisted, and this temporary public key is never a release input.
        key = ec.generate_private_key(ec.SECP256R1())
        self.public_key.write_bytes(key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        ))
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(ProductOtaBuildOptions NONE)\n'
            '# The real module resolves its tools and partition CSV from the repository.\n'
            'set(CMAKE_SOURCE_DIR "${OTA_REPOSITORY_ROOT}")\n'
            '# Production loads this module in a child but compiles the engine in the root.\n'
            'add_subdirectory(esp32)\n'
            'get_source_file_property(_definitions\n'
            '    "${OTA_REPOSITORY_ROOT}/targets/ESP32/_common/ProductFirmwareUpdate.cpp"\n'
            '    COMPILE_DEFINITIONS)\n'
            'file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/evaluated-definitions.txt"\n'
            '    CONTENT "$<JOIN:${_definitions},\\n>\\n")\n',
            encoding="utf-8",
        )
        (self.child / "CMakeLists.txt").write_text(
            'set(CMAKE_SOURCE_DIR "${OTA_REPOSITORY_ROOT}")\n'
            'include("${OTA_REPOSITORY_ROOT}/CMake/Modules/NF_ProductOta.cmake")\n',
            encoding="utf-8",
        )

    def configure(self, *, allow=None, debugger=True, rtm=False):
        command = [
            CMAKE, "-S", str(self.source), "-B", str(self.build),
            f"-DOTA_REPOSITORY_ROOT:PATH={REPOSITORY.as_posix()}",
            f"-DPython3_EXECUTABLE:FILEPATH={Path(sys.executable).as_posix()}",
            f"-DNF_PRODUCT_MANAGED_IMAGE:FILEPATH={self.managed.as_posix()}",
            f"-DNF_PRODUCT_PUBLIC_KEY:FILEPATH={self.public_key.as_posix()}",
            "-DNF_PRODUCT_RELEASE_ID:STRING=cmake-test",
            "-DNF_INTEROP_ASSEMBLIES:STRING=Product.Firmware",
            "-DNF_PRODUCT_OTA_REQUIRE_SIGNATURE:BOOL=ON",
            f"-DNF_FEATURE_DEBUGGER:BOOL={'ON' if debugger else 'OFF'}",
            f"-DNF_BUILD_RTM:BOOL={'ON' if rtm else 'OFF'}",
        ]
        if allow is not None:
            command.append(f"-DNF_PRODUCT_OTA_ALLOW_VS_DEPLOY:BOOL={'ON' if allow else 'OFF'}")
        return subprocess.run(command, capture_output=True, text=True, timeout=60)

    def assert_configured(self, result, expected):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        definitions = (self.build / "evaluated-definitions.txt").read_text().splitlines()
        self.assertEqual(definitions, [f"NF_PRODUCT_OTA_ALLOW_VS_DEPLOY={expected}"])
        self.assertIn(
            "NF_PRODUCT_OTA_REQUIRE_SIGNATURE:BOOL=ON",
            (self.build / "CMakeCache.txt").read_text(),
        )
        self.assertTrue((self.build / "ProductReleaseDescriptor.cpp").is_file())

    def assert_rejected(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        message = " ".join((result.stdout + result.stderr).split())
        self.assertIn(
            "NF_PRODUCT_OTA_ALLOW_VS_DEPLOY requires the managed debugger and a non-RTM build.",
            message,
        )
        self.assertFalse((self.build / "evaluated-definitions.txt").exists())

    def test_default_disables_visual_studio_deployment(self):
        self.assert_configured(self.configure(), 0)

    def test_explicit_enable_with_debugger_non_rtm(self):
        self.assert_configured(self.configure(allow=True), 1)

    def test_explicit_disable_without_debugger(self):
        self.assert_configured(self.configure(allow=False, debugger=False), 0)

    def test_reconfigure_same_build_directory_disables_development_mode(self):
        self.assert_configured(self.configure(allow=True), 1)
        self.assert_configured(self.configure(allow=False), 0)

    def test_enable_without_debugger_is_rejected(self):
        self.assert_rejected(self.configure(allow=True, debugger=False))

    def test_enable_in_rtm_is_rejected(self):
        self.assert_rejected(self.configure(allow=True, rtm=True))


if __name__ == "__main__":
    unittest.main()
