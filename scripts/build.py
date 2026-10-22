#!/usr/bin/env python3
"""Configure, test and package one native target. No remote mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys
import tarfile
import zipfile
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
AQT_CONFIG = Path(__file__).with_name("aqt.ini")


def run(*args, env=None):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True, env=env)


def install_qt(host, target, arch, sdk):
    # Android's Linux host tools need Qt's bundled ICU, not Ubuntu's ICU ABI.
    # Install the host separately so the target's archive filter cannot omit it.
    if host.startswith("linux") and target == "android":
        install_qt(
            host,
            "desktop",
            "linux_gcc_64" if host == "linux" else "linux_gcc_arm64",
            sdk,
        )
    archives = ["qtbase", "qtdeclarative", "qtsvg", "qttools", "qttranslations"]
    if host.startswith("linux") and target == "desktop":
        archives += ["icu", "qtwayland"]
    command = [
        sys.executable,
        "-m",
        "aqt",
        "--config",
        AQT_CONFIG,
        "install-qt",
        host,
        target,
        "6.10.3",
        arch,
        "--autodesktop",
        "-O",
        sdk,
        "--archives",
        *archives,
    ]
    # A streamed read timeout can escape aqt's per-request retries. Retry the
    # installation with another Qt mirror, retaining checksum verification.
    mirrors = (
        None,
        "https://ftp.fau.de/qtproject",
        "https://ftp.jaist.ac.jp/pub/qtproject",
    )
    for attempt, mirror in enumerate(mirrors):
        args = command + (["--base", mirror] if mirror else [])
        try:
            run(*args)
            return
        except subprocess.CalledProcessError:
            if attempt == len(mirrors) - 1:
                raise
            delay = (5, 15)[attempt]
            print(
                f"Qt installation failed for {host}/{target}/{arch}. "
                f"Retrying with another mirror in {delay}s "
                f"(attempt {attempt + 2}/{len(mirrors)}).",
                flush=True,
            )
            time.sleep(delay)


def host_qt_path(sdk, system):
    host = (
        sdk
        / "6.10.3"
        / {"Windows": "msvc2022_64", "Darwin": "macos"}.get(system, "gcc_64")
    )
    if system == "Windows" and not host.exists():
        host = sdk / "6.10.3/mingw_64"
    return host


def simulator_bundle(build):
    # CMake's RUNTIME_OUTPUT_DIRECTORY can put the app in Release/, without
    # Xcode's usual -iphonesimulator suffix. Ask Xcode for the actual product.
    products = json.loads(
        subprocess.check_output(
            [
                "xcodebuild",
                "-project",
                str(build / "AlternativeFastDelivery.xcodeproj"),
                "-scheme",
                "alternativefastdelivery",
                "-configuration",
                "Release",
                "-sdk",
                "iphonesimulator",
                "-destination",
                "generic/platform=iOS Simulator",
                "-showBuildSettings",
                "-json",
            ],
            cwd=ROOT,
            text=True,
        )
    )
    matches = [
        p["buildSettings"]
        for p in products
        if p.get("target") == "alternativefastdelivery"
    ]
    if len(matches) != 1:
        raise RuntimeError(
            "Expected one simulator application target in Xcode build settings"
        )
    settings = matches[0]
    if settings.get("PLATFORM_NAME") != "iphonesimulator":
        raise RuntimeError(
            "Xcode selected a device target instead of the iOS simulator"
        )
    product = settings.get("FULL_PRODUCT_NAME", "")
    if not product.endswith(".app") or Path(product).name != product:
        raise RuntimeError("Xcode did not report an application bundle")
    bundle = Path(settings["TARGET_BUILD_DIR"]) / product
    if not (bundle / "Info.plist").is_file():
        raise RuntimeError(
            "Simulator application is missing its Info.plist: " + str(bundle)
        )
    with (bundle / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    executable = info.get("CFBundleExecutable", "")
    if "iPhoneSimulator" not in info.get("CFBundleSupportedPlatforms", []):
        raise RuntimeError(
            "Application bundle is not built for the iOS simulator: " + str(bundle)
        )
    if (
        not executable
        or Path(executable).name != executable
        or not (bundle / executable).is_file()
    ):
        raise RuntimeError("Simulator application has no executable: " + str(bundle))
    return bundle


def notices(stage, installed, qt):
    target = stage / "licenses"
    target.mkdir(parents=True, exist_ok=True)
    for f in (ROOT / "resources/licenses").glob("*.txt"):
        shutil.copyfile(f, target / f.name)
    for f in installed.glob("share/*/copyright"):
        shutil.copyfile(f, target / (f.parent.name + ".txt"))
    for f in qt.glob("**/LICENSES/*"):
        if f.is_file():
            shutil.copyfile(f, target / ("Qt-" + f.name))
    for f in (qt / "sbom").glob("*.spdx*"):
        shutil.copyfile(f, target / f.name)
    (target / "QT-SOURCE.txt").write_text(
        "Qt 6.10.3: https://download.qt.io/archive/qt/6.10/6.10.3/single/\nSee docs/dependencies.md for replacement and relinking obligations.\n"
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument(
        "--platform",
        required=True,
        choices=["windows", "linux", "macos", "android", "ios", "ios-simulator"],
    )
    p.add_argument("--arch", required=True)
    p.add_argument("--version", default="development")
    p.add_argument("--skip-qt-install", action="store_true")
    p.add_argument(
        "--fresh",
        action="store_true",
        help="Reset CMake configuration while retaining dependencies",
    )
    a = p.parse_args()
    apple_sdk_version = None
    if (
        a.platform in ("macos", "ios", "ios-simulator")
        and platform.system() != "Darwin"
    ):
        raise RuntimeError("Apple targets require a macOS host with Xcode")
    if a.platform.startswith("ios"):
        apple_sdk_version = subprocess.check_output(
            [
                "xcrun",
                "--sdk",
                "iphoneos" if a.platform == "ios" else "iphonesimulator",
                "--show-sdk-version",
            ],
            text=True,
        ).strip()
        if int(apple_sdk_version.split(".")[0]) < 26:
            raise RuntimeError(
                "Select Xcode with the iOS 26 SDK before building this target"
            )
    sdk = ROOT / ".sdk" / "Qt"
    vp = Path(os.environ["VCPKG_ROOT"])
    vexe = vp / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if not vexe.exists():
        run(
            vp / ("bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"),
            "-disableMetrics",
        )
    desktop = a.platform in ("windows", "linux", "macos")
    mobile = not desktop
    definitions = []
    if a.platform == "windows":
        qtarch = (
            "win64_msvc2022_64"
            if a.arch == "x64"
            else "win64_msvc2022_arm64_cross_compiled"
        )
        qdir = "msvc2022_64" if a.arch == "x64" else "msvc2022_arm64"
        host, target, triplet = "windows", "desktop", a.arch + "-windows"
    elif a.platform == "linux":
        host = "linux" if a.arch == "x64" else "linux_arm64"
        qtarch = "linux_gcc_64" if a.arch == "x64" else "linux_gcc_arm64"
        qdir = "gcc_64" if a.arch == "x64" else "gcc_arm64"
        target = "desktop"
        triplet = a.arch + "-linux"
    elif a.platform == "macos":
        host, target, qtarch, qdir = "mac", "desktop", "clang_64", "macos"
        triplet = ("x64" if a.arch == "x64" else "arm64") + "-osx"
        definitions += [
            "-DCMAKE_OSX_ARCHITECTURES=" + ("x86_64" if a.arch == "x64" else "arm64"),
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0",
        ]
    elif a.platform == "android":
        host, target = {"Windows": "windows", "Darwin": "mac"}.get(
            platform.system(), "linux"
        ), "android"
        qtarch = {
            "arm64-v8a": "android_arm64_v8a",
            "armeabi-v7a": "android_armv7",
            "x86": "android_x86",
            "x86_64": "android_x86_64",
        }[a.arch]
        qdir = qtarch
        triplet = {
            "arm64-v8a": "arm64-android",
            "armeabi-v7a": "arm-neon-android",
            "x86": "x86-android",
            "x86_64": "x64-android",
        }[a.arch]
        definitions += [
            "-DANDROID_ABI=" + a.arch,
            "-DANDROID_PLATFORM=android-28",
            "-DANDROID_NDK=" + os.environ["ANDROID_NDK_HOME"],
            "-DANDROID_NDK_ROOT=" + os.environ["ANDROID_NDK_HOME"],
            "-DANDROID_SDK_ROOT=" + os.environ["ANDROID_HOME"],
            "-DQT_ANDROID_SDK_ROOT=" + os.environ["ANDROID_HOME"],
            "-DQT_ANDROID_NDK_ROOT=" + os.environ["ANDROID_NDK_HOME"],
        ]
    else:
        host, target, qtarch, qdir = "mac", "ios", "ios", "ios"
        triplet = "arm64-ios" if a.platform == "ios" else "x64-ios"
        definitions += [
            "-DCMAKE_OSX_SYSROOT="
            + ("iphoneos" if a.platform == "ios" else "iphonesimulator"),
            "-DCMAKE_OSX_ARCHITECTURES="
            + ("arm64" if a.platform == "ios" else "x86_64"),
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=17.0",
            "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
            "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO",
        ]
    if not a.skip_qt_install:
        install_qt(host, target, qtarch, sdk)
    qt = sdk / "6.10.3" / qdir
    build = ROOT / "build" / (a.platform + "-" + a.arch)
    stage = build / "stage"
    definitions += [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=" + str(qt),
        "-DCMAKE_TOOLCHAIN_FILE=" + str(vp / "scripts/buildsystems/vcpkg.cmake"),
        "-DVCPKG_TARGET_TRIPLET=" + triplet,
        "-DBUILD_TESTING=" + ("ON" if desktop else "OFF"),
        "-DVCPKG_OVERLAY_TRIPLETS=" + str(ROOT / "cmake"),
        "-DAFD_VERSION=" + a.version.removeprefix("v"),
        "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root="
        + str(build / "vcpkg-buildtrees")
        + ";--x-packages-root="
        + str(build / "vcpkg-packages"),
    ]
    if mobile:
        definitions += [
            "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="
            + str(qt / "lib/cmake/Qt6/qt.toolchain.cmake"),
            "-DQT_HOST_PATH=" + str(host_qt_path(sdk, platform.system())),
        ]
    if a.platform == "android":
        definitions += [
            "-DQT_ANDROID_DEPLOYMENT_TYPE="
            + ("Release" if os.environ.get("ANDROID_KEYSTORE_B64") else "Debug")
        ]
        if a.arch in ("arm64-v8a", "x86_64"):
            definitions += ["-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"]
    if a.platform == "windows" and a.arch == "arm64":
        definitions += ["-DQT_HOST_PATH=" + str(sdk / "6.10.3/msvc2022_64")]
    generator = "Xcode" if a.platform.startswith("ios") else "Ninja"
    definitions = [definition.replace("\\", "/") for definition in definitions]
    configure = ["cmake"] + (["--fresh"] if a.fresh else [])
    run(*configure, "-S", ROOT, "-B", build, "-G", generator, *definitions)
    native_build = ["cmake", "--build", build, "--config", "Release", "--parallel", "3"]
    if a.platform == "android":
        native_build += ["--target", "alternativefastdelivery"]
    run(*native_build)
    env = dict(os.environ)
    env["PATH"] = (
        str(qt / "bin")
        + os.pathsep
        + str(build / "vcpkg_installed" / triplet / "bin")
        + os.pathsep
        + env["PATH"]
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_QUICK_BACKEND"] = "software"
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    name = "AlternativeFastDelivery-" + a.version + "-" + a.platform + "-" + a.arch
    if desktop:
        run(
            "ctest",
            "--test-dir",
            build,
            "-C",
            "Release",
            "--output-on-failure",
            env=env,
        )
        run(
            "cmake",
            "--install",
            build,
            "--config",
            "Release",
            "--prefix",
            stage,
            env=env,
        )
        if a.platform == "windows":
            for f in (build / "vcpkg_installed" / triplet / "bin").glob("*.dll"):
                shutil.copy2(f, stage / "bin" / f.name)
            executable = stage / "bin/alternativefastdelivery.exe"
        elif a.platform == "macos":
            executable = (
                stage
                / "alternativefastdelivery.app/Contents/MacOS/alternativefastdelivery"
            )
        else:
            executable = stage / "bin/alternativefastdelivery"
            for f in (build / "vcpkg_installed" / triplet / "lib").glob("*.so*"):
                dest = stage / "lib" / f.name
                dest.parent.mkdir(exist_ok=True)
                shutil.copy2(f, dest, follow_symlinks=True)
            launcher = stage / "AlternativeFastDelivery"
            launcher.write_text(
                '#!/bin/sh\nHERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\nexport LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexec "$HERE/bin/alternativefastdelivery" "$@"\n'
            )
            launcher.chmod(0o755)
            env["LD_LIBRARY_PATH"] = str(stage / "lib")
        if not executable.is_file() or executable.stat().st_size < 50000:
            raise RuntimeError("Missing native executable")
        package_env = dict(env)
        for variable in ("QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
            package_env.pop(variable, None)
        if a.platform == "windows":
            system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
            package_env["QT_QPA_PLATFORM"] = "windows"
            package_env["PATH"] = os.pathsep.join(
                str(p) for p in (stage / "bin", system / "System32", system)
            )
        elif a.platform == "macos":
            # macdeployqt bundles Cocoa, not the SDK's offscreen test plugin.
            package_env["QT_QPA_PLATFORM"] = "cocoa"
        subprocess.run(
            [str(executable), "--interaction-smoke-test"],
            cwd=ROOT,
            env=package_env,
            check=True,
            timeout=20,
        )
        if a.platform == "linux":
            dependencies = subprocess.run(
                ["ldd", str(executable)],
                env=package_env,
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            for line in dependencies.splitlines():
                if "libQt6" in line and str(stage / "lib") not in line:
                    raise RuntimeError(
                        "Qt dependency is missing from the portable package: " + line
                    )
        notices(stage, build / "vcpkg_installed" / triplet, qt)
        if a.platform == "linux":
            with tarfile.open(dist / (name + ".tar.gz"), "w:gz") as t:
                t.add(stage, arcname=name)
        else:
            shutil.make_archive(str(dist / name), "zip", stage)
    elif a.platform == "android":
        # Qt's dependency file does not notice newly added package-template files.
        (build / "src/ui/android-build/alternativefastdelivery.apk").unlink(
            missing_ok=True
        )
        signed = bool(os.environ.get("ANDROID_KEYSTORE_B64"))
        if signed:
            import base64

            key = build / "release.keystore"
            key.write_bytes(
                base64.b64decode(os.environ["ANDROID_KEYSTORE_B64"], validate=True)
            )
            key.chmod(0o600)
            env.update(
                QT_ANDROID_KEYSTORE_PATH=str(key),
                QT_ANDROID_KEYSTORE_ALIAS=os.environ["ANDROID_KEY_ALIAS"],
                QT_ANDROID_KEYSTORE_STORE_PASS=os.environ["ANDROID_STORE_PASSWORD"],
                QT_ANDROID_KEYSTORE_KEY_PASS=os.environ["ANDROID_KEY_PASSWORD"],
            )
            try:
                run("cmake", "-B", build, "-DQT_ANDROID_SIGN_APK=ON", env=env)
                run(
                    "cmake",
                    "--build",
                    build,
                    "--target",
                    "apk",
                    "--parallel",
                    "3",
                    env=env,
                )
            finally:
                key.unlink(missing_ok=True)
            apks = list(build.glob("**/*-release-signed.apk"))
        else:
            run(
                "cmake", "--build", build, "--target", "apk", "--parallel", "3", env=env
            )
            apks = list(build.glob("**/*-debug.apk"))
            name += "-development"
        if len(apks) != 1:
            raise RuntimeError(
                "Expected exactly one signed or development APK, found " + str(apks)
            )
        tools = Path(os.environ["ANDROID_HOME"]) / "build-tools/36.0.0"
        run(
            tools / ("apksigner.bat" if os.name == "nt" else "apksigner"),
            "verify",
            "--verbose",
            apks[0],
        )
        run(
            tools / ("aapt.exe" if os.name == "nt" else "aapt"),
            "dump",
            "badging",
            apks[0],
        )
        run(
            tools / ("zipalign.exe" if os.name == "nt" else "zipalign"),
            "-c",
            "-P",
            "16",
            "4",
            apks[0],
        )
        with zipfile.ZipFile(apks[0]) as package:
            native = [
                n
                for n in package.namelist()
                if n.startswith("lib/") and n.endswith(".so")
            ]
            if not native or not any("libalternativefastdelivery" in n for n in native):
                raise RuntimeError("APK is missing the application library")
            for library_name in native:
                if not library_name.startswith("lib/" + a.arch + "/"):
                    raise RuntimeError("Unexpected ABI in APK: " + library_name)
                with package.open(library_name) as stream:
                    header = stream.read(64)
                    if header[:4] != b"\x7fELF" or header[5] != 1:
                        raise RuntimeError("Invalid ELF library: " + library_name)
                    machine = struct.unpack_from("<H", header, 18)[0]
                    if (
                        machine
                        != {
                            "arm64-v8a": 183,
                            "armeabi-v7a": 40,
                            "x86": 3,
                            "x86_64": 62,
                        }[a.arch]
                    ):
                        raise RuntimeError(
                            "Library machine type does not match APK label: "
                            + library_name
                        )
                    if header[4] == 2:
                        offset = struct.unpack_from("<Q", header, 32)[0]
                        stride, count = struct.unpack_from("<HH", header, 54)
                        if count > 256 or stride < 56 or offset > 65536:
                            raise RuntimeError("Invalid ELF program headers")
                        stream.seek(offset)
                        segments = stream.read(stride * count)
                        for i in range(count):
                            kind, _, _, _, _, _, _, alignment = struct.unpack_from(
                                "<IIQQQQQQ", segments, i * stride
                            )
                            if kind == 1 and alignment < 16384:
                                raise RuntimeError(
                                    "Library lacks 16 KiB page alignment: "
                                    + library_name
                                )
        shutil.copy2(apks[0], dist / (name + ".apk"))
    else:
        if a.platform == "ios":
            archive = build / "AlternativeFastDelivery.xcarchive"
            run(
                "xcodebuild",
                "-project",
                build / "AlternativeFastDelivery.xcodeproj",
                "-scheme",
                "alternativefastdelivery",
                "-configuration",
                "Release",
                "-destination",
                "generic/platform=iOS",
                "-archivePath",
                archive,
                "archive",
                "CODE_SIGNING_ALLOWED=NO",
                "SKIP_INSTALL=NO",
            )
            if not (
                archive / "Products/Applications/alternativefastdelivery.app"
            ).exists():
                raise RuntimeError("Device archive has no app bundle")
            # A separate signing script exports only under a valid device profile.
            if os.environ.get("IOS_CERTIFICATE_B64"):
                run(
                    sys.executable,
                    ROOT / "scripts/sign_ios.py",
                    archive,
                    dist / (name + ".ipa"),
                )
            else:
                shutil.make_archive(
                    str(dist / (name + "-REQUIRES-SIGNING.xcarchive")),
                    "zip",
                    archive.parent,
                    archive.name,
                )
        else:
            app = simulator_bundle(build)
            shutil.make_archive(
                str(dist / (name + "-simulator")), "zip", app.parent, app.name
            )
    metadata = {
        "version": a.version,
        "platform": a.platform,
        "architecture": a.arch,
        "commit": os.environ.get("GITHUB_SHA", "local"),
        "qt_version": "6.10.3",
        "native_configuration": "Release",
        "build_date": __import__("datetime")
        .datetime.now(__import__("datetime").timezone.utc)
        .isoformat(),
        "limitations": "See docs/validation.md; simulator bundles do not install on devices; unsigned archives require signing.",
    }
    if apple_sdk_version:
        metadata["apple_sdk_version"] = apple_sdk_version
    (dist / (name + ".json")).write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
