"""Regress the factory image that failed to mount before USB debugger startup."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake is not installed")
class LittlefsImageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="product-ota-littlefs-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.component = self.root / "littlefs-component"
        self.header = self.component / "src" / "littlefs" / "lfs.h"
        self.header.parent.mkdir(parents=True)
        self.seed = self.root / "seed"
        self.seed.mkdir()
        (self.seed / "settings.txt").write_text("factory settings\n", encoding="utf-8")
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(ProductOtaLittlefs NONE)\n'
            'function(idf_component_get_property output component property)\n'
            '    if(NOT component STREQUAL "littlefs" OR NOT property STREQUAL "COMPONENT_DIR")\n'
            '        message(FATAL_ERROR "Unexpected SDK property request")\n'
            '    endif()\n'
            '    set(${output} "${TEST_COMPONENT_DIRECTORY}" PARENT_SCOPE)\n'
            'endfunction()\n'
            'function(littlefs_create_partition_image partition directory)\n'
            '    if(NOT partition STREQUAL "config" OR NOT ARGN STREQUAL "")\n'
            '        message(FATAL_ERROR "Must generate config without automatic flashing")\n'
            '    endif()\n'
            '    file(WRITE "${CMAKE_BINARY_DIR}/image-name-max.txt" "${CONFIG_LITTLEFS_OBJ_NAME_LEN}")\n'
            'endfunction()\n'
            'include("${OTA_REPOSITORY_ROOT}/CMake/Modules/NF_ProductOtaLittlefs.cmake")\n'
            'nf_product_ota_create_config_image("${TEST_SEED_DIRECTORY}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/runtime-name-length.txt" "${CONFIG_LITTLEFS_OBJ_NAME_LEN}")\n',
            encoding="utf-8",
        )

    def configure(self, buffer_length=256, runtime_limit=255):
        self.header.write_text(
            f"#ifndef LFS_NAME_MAX\n#define LFS_NAME_MAX {runtime_limit}\n#endif\n",
            encoding="utf-8",
        )
        result = subprocess.run([
            CMAKE, "-S", str(self.source), "-B", str(self.build),
            f"-DOTA_REPOSITORY_ROOT={REPOSITORY.as_posix()}",
            f"-DTEST_COMPONENT_DIRECTORY={self.component.as_posix()}",
            f"-DTEST_SEED_DIRECTORY={self.seed.as_posix()}",
            f"-DCONFIG_LITTLEFS_OBJ_NAME_LEN={buffer_length}",
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.build / "runtime-name-length.txt").read_text(), str(buffer_length))
        return int((self.build / "image-name-max.txt").read_text())

    def littlefs(self, *arguments, succeeds=True):
        result = subprocess.run(
            [sys.executable, "-m", "littlefs", *map(str, arguments)],
            capture_output=True, text=True, timeout=30,
        )
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def create_image(self, name_max):
        image = self.root / "config.bin"
        self.littlefs("create", self.seed, image, "--fs-size=65536",
                      "--block-size=4096", f"--name-max={name_max}")
        return image

    def test_product_buffer_excludes_nul_from_image_limit(self):
        self.assertEqual(self.configure(), 255)

    def test_smaller_buffers_reserve_the_nul_terminator(self):
        self.assertEqual(self.configure(buffer_length=64), 63)

    def test_large_buffers_do_not_exceed_runtime_limit(self):
        self.assertEqual(self.configure(buffer_length=1022), 255)

    def test_runtime_limit_is_read_from_component(self):
        self.assertEqual(self.configure(runtime_limit=127), 127)

    def test_old_image_is_rejected_by_runtime_filename_limit(self):
        image = self.create_image(256)
        result = self.littlefs("list", image, "--block-size=4096", "--name-max=255", succeeds=False)
        self.assertIn("LFS_ERR_INVAL", result.stderr)

    def test_generated_limit_mounts_and_preserves_seed_contents(self):
        image = self.create_image(self.configure())
        result = self.littlefs("list", image, "--block-size=4096", "--name-max=255")
        self.assertIn("settings.txt", result.stdout)
        extracted = self.root / "extracted"
        self.littlefs("extract", image, extracted, "--block-size=4096", "--name-max=255")
        self.assertEqual((extracted / "settings.txt").read_bytes(), (self.seed / "settings.txt").read_bytes())


if __name__ == "__main__":
    unittest.main()
