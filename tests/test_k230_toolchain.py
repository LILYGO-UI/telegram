"""Exercise clean K230 configuration without downloads or a cross compiler."""

import hashlib
import io
import tarfile
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake is required for the toolchain regression tests")
class K230ToolchainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="telegram-k230-toolchain-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.project = self.root / "telegram"
        self.build = self.project / "build"
        cmake = self.project / "cmake"
        cmake.mkdir(parents=True)
        for filename in ("k230-debian-toolchain.cmake", "K230DebianSysroot.cmake", "k230-bsp.cmake"):
            shutil.copy2(PROJECT / "cmake" / filename, cmake / filename)
        (self.project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(K230SysrootFixture LANGUAGES NONE)\n"
            'file(WRITE "${CMAKE_BINARY_DIR}/selection.txt"\n'
            '    "${LilyGoUI_DIR}\\n${LILYGO_UI_DEBIAN_SYSROOT}\\n")\n')
        self.vendor = self.make_sdk(self.project / "third_party/cm0-appkit")
        self.sibling = self.root / "cm0-appkit/.cache/k230-debian/sysroot"
        self.archive = self.root / "release.tar.gz"
        with tarfile.open(self.archive, "w:gz") as bundle:
            for name in ("usr/include/stdio.h", "usr/lib/riscv64-linux-gnu/crt1.o",
                         "usr/lib/riscv64-linux-gnu/pkgconfig/libdrm.pc", "var/lib/dpkg/status",
                         "usr/share/canaan-sdk/manifest.json"):
                member = tarfile.TarInfo("sysroot/" + name)
                member.size = 1
                bundle.addfile(member, io.BytesIO(b"x"))
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.managed = self.project / ".cache/k230-bsp/20261116T104343Z/sysroot"
        self.environment = dict(os.environ)
        for name in ("LILYGO_UI_APPKIT_DIR", "LILYGO_UI_DEBIAN_SYSROOT",
                     "LILYGO_UI_DEBIAN_CROSS_PREFIX",
                     "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR"):
            self.environment.pop(name, None)

    def make_sdk(self, sdk):
        toolchain = sdk / "cmake/k230-debian-toolchain.cmake"
        toolchain.parent.mkdir(parents=True)
        # Custom sysroots are validated by the selected SDK, independently of
        # the stronger readiness check used to choose automatic cache paths.
        toolchain.write_text(
            'set(CMAKE_SYSTEM_NAME Linux)\n'
            'if(NOT IS_DIRECTORY "${LILYGO_UI_DEBIAN_SYSROOT}/usr/include" OR\n'
            '   NOT IS_DIRECTORY "${LILYGO_UI_DEBIAN_SYSROOT}/usr/lib")\n'
            '    message(FATAL_ERROR "Stub SDK: incomplete explicit sysroot: '
            '${LILYGO_UI_DEBIAN_SYSROOT}")\n'
            'endif()\n'
            'list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES '
            'LILYGO_UI_DEBIAN_SYSROOT)\n')
        return sdk

    @staticmethod
    def make_sysroot(path, *, managed=True):
        (path / "usr/include").mkdir(parents=True, exist_ok=True)
        (path / "usr/lib").mkdir(parents=True, exist_ok=True)
        if managed:
            (path / "usr/lib/riscv64-linux-gnu").mkdir(exist_ok=True)
            (path / "var/lib/dpkg").mkdir(parents=True, exist_ok=True)
            (path / "var/lib/dpkg/status").write_text("Package: fixture\n")
        return path

    def configure(self, *definitions, build=None, success=True):
        build = build or self.build
        result = subprocess.run(
            [CMAKE, "-S", str(self.project), "-B", str(build),
             "-G", "Unix Makefiles",
             f"-DCMAKE_TOOLCHAIN_FILE={self.project / 'cmake/k230-debian-toolchain.cmake'}",
             f"-DK230_BSP_URL={self.archive.as_uri()}", f"-DK230_BSP_SHA256={self.digest}",
             *definitions], cwd=self.root, env=self.environment,
            capture_output=True, text=True, timeout=30)
        diagnostics = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, diagnostics)
            selection = (build / "selection.txt").read_text().splitlines()
            self.assertEqual(len(selection), 2, selection)
            return tuple(map(Path, selection))
        self.assertNotEqual(result.returncode, 0, diagnostics)
        return diagnostics

    def test_project_release_survives_clean_build_and_ignores_sibling(self):
        self.make_sysroot(self.sibling)
        expected = (self.vendor, self.managed)
        self.assertEqual(self.configure(), expected)
        self.archive.unlink()
        shutil.rmtree(self.build)
        self.assertEqual(self.configure(), expected)

    def test_corrupt_download_fails_without_publishing(self):
        self.archive.write_bytes(b'corrupt')
        self.assertIn('SHA-256 mismatch', self.configure(success=False))
        self.assertFalse(self.managed.exists())
        self.assertFalse(list(self.managed.parent.glob('*.part')))
        self.assertFalse(list(self.managed.parent.glob('*.tar.gz')))

    def test_explicit_sysroot_overrides_environment_and_survives_reconfigure(self):
        explicit = self.make_sysroot(self.root / "explicit sysroot", managed=False)
        environment = self.make_sysroot(self.root / "environment sysroot")
        self.make_sysroot(self.sibling)
        self.environment["LILYGO_UI_DEBIAN_SYSROOT"] = str(environment)
        expected = (self.vendor, explicit)
        self.assertEqual(self.configure(f"-DLILYGO_UI_DEBIAN_SYSROOT={explicit}"), expected)
        self.environment["LILYGO_UI_DEBIAN_SYSROOT"] = str(self.root / "missing env")
        self.assertEqual(self.configure(), expected)
        self.assertFalse(self.managed.exists())

    def test_environment_sysroot_overrides_automatic_cache(self):
        selected = self.make_sysroot(self.root / "environment sysroot", managed=False)
        self.make_sysroot(self.sibling)
        self.environment["LILYGO_UI_DEBIAN_SYSROOT"] = str(selected)
        self.assertEqual(self.configure(), (self.vendor, selected))
        self.assertFalse(self.managed.exists())

    def test_relative_sysroot_named_off_is_not_treated_as_a_boolean(self):
        selected = self.make_sysroot(self.project / "OFF", managed=False)
        self.make_sysroot(self.sibling)
        self.assertEqual(self.configure("-DLILYGO_UI_DEBIAN_SYSROOT=OFF"),
                         (self.vendor, selected))
        self.assertFalse(self.managed.exists())

    def test_missing_custom_sysroot_does_not_prepare_or_fall_back(self):
        self.make_sysroot(self.sibling)
        missing = self.root / "missing custom sysroot"
        for source in ("explicit", "environment"):
            with self.subTest(source=source):
                definitions = []
                if source == "explicit":
                    definitions.append(f"-DLILYGO_UI_DEBIAN_SYSROOT={missing}")
                else:
                    self.environment["LILYGO_UI_DEBIAN_SYSROOT"] = str(missing)
                diagnostics = self.configure(
                    *definitions, build=self.project / f"build-{source}", success=False)
                self.assertIn("Stub SDK: incomplete explicit sysroot", diagnostics)
                self.assertIn(str(missing), " ".join(diagnostics.split()))
        self.assertFalse(missing.exists())
        self.assertFalse(self.managed.exists())

    def test_selecting_different_appkit_keeps_project_bsp(self):
        environment_sdk = self.make_sdk(self.root / "environment SDK")
        explicit_sdk = self.make_sdk(self.root / "explicit SDK")
        self.environment["LILYGO_UI_APPKIT_DIR"] = str(environment_sdk)
        self.assertEqual(self.configure(), (environment_sdk, self.managed))
        self.archive.unlink()
        self.assertEqual(self.configure(f"-DLilyGoUI_DIR={explicit_sdk}", build=self.project / "other"),
                         (explicit_sdk, self.managed))


if __name__ == "__main__":
    unittest.main()
