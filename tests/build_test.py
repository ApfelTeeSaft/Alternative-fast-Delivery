"""Exercise CI toolchain setup without downloading SDKs or building dependencies."""

import importlib.util
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch


path = Path(__file__).resolve().parents[1] / "scripts/build.py"
spec = importlib.util.spec_from_file_location("afd_build", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Configured(Exception):
    """Stop once main has produced the real CMake configure command."""


class BuildTests(unittest.TestCase):
    def test_macos_package_uses_its_bundled_platform_plugin(self):
        for arch in ("x64", "arm64"):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vp = root / "vcpkg"
                vp.mkdir()
                (vp / ("vcpkg.exe" if os.name == "nt" else "vcpkg")).touch()
                stage = root / "build" / ("macos-" + arch) / "stage"
                app = stage / "alternativefastdelivery.app"
                binary = app / "Contents/MacOS/alternativefastdelivery"
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b"fixture" * 8000)
                plugins = app / "Contents/PlugIns/platforms"
                plugins.mkdir(parents=True)
                (plugins / "libqcocoa.dylib").write_bytes(b"fixture")

                def start(command, **kwargs):
                    self.assertEqual(command, [str(binary), "--interaction-smoke-test"])
                    environment = kwargs["env"]
                    # Reproduce the packaged Cocoa-only plugin set from CI.
                    backend = environment["QT_QPA_PLATFORM"]
                    self.assertTrue((plugins / ("libq" + backend + ".dylib")).is_file())
                    self.assertEqual(backend, "cocoa")
                    self.assertEqual(environment["QT_QUICK_BACKEND"], "software")
                    for variable in (
                        "QT_PLUGIN_PATH",
                        "QML_IMPORT_PATH",
                        "QML2_IMPORT_PATH",
                    ):
                        self.assertNotIn(variable, environment)

                with patch.object(module, "ROOT", root), patch.object(
                    module, "run"
                ), patch.object(module, "install_qt"), patch.object(
                    module.platform, "system", return_value="Darwin"
                ), patch.object(
                    module.subprocess, "run", side_effect=start
                ) as startup, patch.dict(
                    os.environ,
                    {
                        "VCPKG_ROOT": str(vp),
                        "QT_PLUGIN_PATH": "/sdk/plugins",
                        "QML_IMPORT_PATH": "/sdk/qml",
                        "QML2_IMPORT_PATH": "/sdk/qml",
                    },
                ), patch.object(
                    sys, "argv", ["build.py", "--platform", "macos", "--arch", arch]
                ):
                    module.main()
                    startup.assert_called_once()
                with zipfile.ZipFile(
                    root
                    / "dist"
                    / ("AlternativeFastDelivery-development-macos-" + arch + ".zip")
                ) as archive:
                    self.assertIn(
                        "alternativefastdelivery.app/Contents/PlugIns/platforms/libqcocoa.dylib",
                        archive.namelist(),
                    )

    def test_simulator_packages_xcode_product_in_either_release_layout(self):
        for layout in ("Release", "Release-iphonesimulator"):
            with self.subTest(
                layout=layout
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vp = root / "vcpkg"
                vp.mkdir()
                (vp / ("vcpkg.exe" if os.name == "nt" else "vcpkg")).touch()
                build = root / "build/ios-simulator-x64"
                app = build / layout / "alternativefastdelivery.app"
                app.mkdir(parents=True)
                (app / "alternativefastdelivery").write_bytes(b"fixture")
                with (app / "Info.plist").open("wb") as stream:
                    plistlib.dump(
                        {
                            "CFBundleExecutable": "alternativefastdelivery",
                            "CFBundleSupportedPlatforms": ["iPhoneSimulator"],
                        },
                        stream,
                    )
                products = [
                    {"target": "ALL_BUILD", "buildSettings": {}},
                    {
                        "target": "alternativefastdelivery",
                        "buildSettings": {
                            "PLATFORM_NAME": "iphonesimulator",
                            "TARGET_BUILD_DIR": str(app.parent),
                            "FULL_PRODUCT_NAME": app.name,
                        },
                    },
                ]

                def output(command, **kwargs):
                    if command[0] == "xcrun":
                        return "26.2\n"
                    self.assertEqual(command[0], "xcodebuild")
                    self.assertIn("-showBuildSettings", command)
                    self.assertEqual(
                        command[command.index("-sdk") + 1], "iphonesimulator"
                    )
                    self.assertIn("generic/platform=iOS Simulator", command)
                    return json.dumps(products)

                with patch.object(module, "ROOT", root), patch.object(
                    module, "run"
                ), patch.object(module, "install_qt"), patch.object(
                    module.platform, "system", return_value="Darwin"
                ), patch.object(
                    module.subprocess, "check_output", side_effect=output
                ), patch.dict(
                    os.environ, {"VCPKG_ROOT": str(vp)}
                ), patch.object(
                    sys,
                    "argv",
                    ["build.py", "--platform", "ios-simulator", "--arch", "x64"],
                ):
                    module.main()
                with zipfile.ZipFile(
                    root
                    / "dist/AlternativeFastDelivery-development-ios-simulator-x64-simulator.zip"
                ) as archive:
                    self.assertIn(
                        "alternativefastdelivery.app/Info.plist", archive.namelist()
                    )
                    self.assertIn(
                        "alternativefastdelivery.app/alternativefastdelivery",
                        archive.namelist(),
                    )

    def test_simulator_does_not_package_device_output(self):
        for platform, supported in (
            ("iphoneos", "iPhoneOS"),
            ("iphonesimulator", "iPhoneOS"),
        ):
            with self.subTest(
                platform=platform
            ), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                app = build / "Release/alternativefastdelivery.app"
                app.mkdir(parents=True)
                (app / "alternativefastdelivery").write_bytes(b"fixture")
                with (app / "Info.plist").open("wb") as stream:
                    plistlib.dump(
                        {
                            "CFBundleExecutable": "alternativefastdelivery",
                            "CFBundleSupportedPlatforms": [supported],
                        },
                        stream,
                    )
                products = [
                    {
                        "target": "alternativefastdelivery",
                        "buildSettings": {
                            "PLATFORM_NAME": platform,
                            "TARGET_BUILD_DIR": str(app.parent),
                            "FULL_PRODUCT_NAME": app.name,
                        },
                    }
                ]
                with patch.object(
                    module.subprocess, "check_output", return_value=json.dumps(products)
                ):
                    with self.assertRaisesRegex(RuntimeError, "simulator"):
                        module.simulator_bundle(build)

    def test_simulator_requires_one_application_target(self):
        product = {"target": "alternativefastdelivery", "buildSettings": {}}
        for products in ([], [product, product]):
            with self.subTest(products=products), patch.object(
                module.subprocess, "check_output", return_value=json.dumps(products)
            ):
                with self.assertRaisesRegex(
                    RuntimeError, "one simulator application target"
                ):
                    module.simulator_bundle(Path("build"))

    def test_linux_desktop_includes_icu_and_wayland(self):
        for host, arch in (
            ("linux", "linux_gcc_64"),
            ("linux_arm64", "linux_gcc_arm64"),
        ):
            with self.subTest(host=host), patch.object(module, "run") as run:
                module.install_qt(host, "desktop", arch, Path("sdk"))
                args = run.call_args.args
                archives = args[args.index("--archives") + 1 :]
                self.assertIn("icu", archives)
                self.assertIn("qtwayland", archives)
                self.assertIn("qttools", archives)

    def test_android_installs_linux_host_before_target(self):
        with patch.object(module, "run") as run:
            module.install_qt("linux", "android", "android_arm64_v8a", Path("sdk"))
            host, target = [call.args for call in run.call_args_list]
            self.assertEqual(host[4:8], ("linux", "desktop", "6.10.3", "linux_gcc_64"))
            self.assertIn("icu", host[host.index("--archives") + 1 :])
            self.assertEqual(
                target[4:8], ("linux", "android", "6.10.3", "android_arm64_v8a")
            )
            self.assertNotIn("icu", target[target.index("--archives") + 1 :])

    def test_non_linux_install_does_not_request_linux_archives(self):
        for host, target, arch in (
            ("windows", "desktop", "win64_msvc2022_64"),
            ("mac", "ios", "ios"),
        ):
            with self.subTest(host=host), patch.object(module, "run") as run:
                module.install_qt(host, target, arch, Path("sdk"))
                self.assertEqual(run.call_count, 1)
                archives = run.call_args.args[
                    run.call_args.args.index("--archives") + 1 :
                ]
                self.assertNotIn("icu", archives)
                self.assertNotIn("qtwayland", archives)

    def test_windows_host_path_uses_installed_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            self.assertEqual(
                module.host_qt_path(sdk, "Windows"), sdk / "6.10.3/mingw_64"
            )
            msvc = sdk / "6.10.3/msvc2022_64"
            msvc.mkdir(parents=True)
            self.assertEqual(module.host_qt_path(sdk, "Windows"), msvc)

    def test_mobile_cmake_configuration_has_matching_host_tools(self):
        cases = [
            ("ios", "arm64", "Darwin", "macos", "Xcode"),
            ("ios-simulator", "x64", "Darwin", "macos", "Xcode"),
        ]
        cases += [
            ("android", abi, "Linux", "gcc_64", "Ninja")
            for abi in ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        ]
        for target, arch, system, host_dir, generator in cases:
            with self.subTest(
                target=target, arch=arch
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                vp = root / "vcpkg"
                vp.mkdir()
                (vp / ("vcpkg.exe" if os.name == "nt" else "vcpkg")).touch()
                commands = []

                def run(*args, **kwargs):
                    commands.append(tuple(str(arg) for arg in args))
                    if args[0] == "cmake":
                        raise Configured()

                env = dict(
                    VCPKG_ROOT=str(vp),
                    ANDROID_NDK_HOME=str(root / "ndk"),
                    ANDROID_HOME=str(root / "android"),
                )
                with patch.object(module, "ROOT", root), patch.object(
                    module, "run", run
                ), patch.object(module, "install_qt"), patch.object(
                    module.platform, "system", return_value=system
                ), patch.object(
                    module.subprocess, "check_output", return_value="26.2\n"
                ), patch.dict(
                    os.environ, env
                ), patch.object(
                    sys, "argv", ["build.py", "--platform", target, "--arch", arch]
                ):
                    with self.assertRaises(Configured):
                        module.main()
                args = commands[-1]
                host = str(root / ".sdk/Qt/6.10.3" / host_dir).replace("\\", "/")
                self.assertIn("-DQT_HOST_PATH=" + host, args)
                self.assertIn(generator, args)
                self.assertIn("-DBUILD_TESTING=OFF", args)
                self.assertTrue(
                    any(
                        arg.startswith("-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=")
                        for arg in args
                    )
                )

    def test_old_ios_sdk_still_fails_before_downloads(self):
        with patch.object(
            module.platform, "system", return_value="Darwin"
        ), patch.object(
            module.subprocess, "check_output", return_value="18.5\n"
        ), patch.object(
            module, "install_qt"
        ) as install, patch.object(
            sys, "argv", ["build.py", "--platform", "ios-simulator", "--arch", "x64"]
        ):
            with self.assertRaisesRegex(RuntimeError, "iOS 26 SDK"):
                module.main()
            install.assert_not_called()


if __name__ == "__main__":
    unittest.main()
