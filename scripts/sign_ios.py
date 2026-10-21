#!/usr/bin/env python3
"""Export a device-registered development or ad-hoc IPA using Xcode."""
import base64
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import uuid
import shlex
import re
from datetime import datetime, timezone


def run(*args, capture=False):
    return subprocess.run(
        [str(x) for x in args], check=True, capture_output=capture
    ).stdout


def main():
    archive, output = map(Path, sys.argv[1:3])
    method = os.environ.get("IOS_EXPORT_METHOD", "debugging")
    if method not in ("debugging", "release-testing"):
        raise RuntimeError(
            "Only registered-device development or ad hoc export is allowed"
        )
    keychain_name = "afd-" + uuid.uuid4().hex + ".keychain-db"
    password = uuid.uuid4().hex
    profile_path = None
    previous_keychains = shlex.split(
        run("security", "list-keychains", "-d", "user", capture=True).decode()
    )
    with tempfile.TemporaryDirectory(prefix="afd-signing-") as temp:
        temp = Path(temp)
        cert = temp / "identity.p12"
        profile = temp / "profile.mobileprovision"
        cert.write_bytes(
            base64.b64decode(os.environ["IOS_CERTIFICATE_B64"], validate=True)
        )
        profile.write_bytes(
            base64.b64decode(os.environ["IOS_PROFILE_B64"], validate=True)
        )
        cert.chmod(0o600)
        profile.chmod(0o600)
        info = plistlib.loads(run("security", "cms", "-D", "-i", profile, capture=True))
        if not info.get("ProvisionedDevices") or info.get("ProvisionsAllDevices"):
            raise RuntimeError(
                "Expected a device-registered development/ad-hoc profile"
            )
        if info["ExpirationDate"].replace(tzinfo=timezone.utc) < datetime.now(
            timezone.utc
        ):
            raise RuntimeError("Provisioning profile expired")
        team = info["TeamIdentifier"][0]
        bundle = "org.alternativefastdelivery.app"
        if info["Entitlements"]["application-identifier"] != team + "." + bundle:
            raise RuntimeError("Provisioning bundle ID mismatch")
        profile_path = (
            Path.home()
            / "Library/MobileDevice/Provisioning Profiles"
            / (info["UUID"] + ".mobileprovision")
        )
        profile_path.parent.mkdir(parents=True, exist_ok=True)
        if profile_path.exists():
            raise RuntimeError("Refusing to overwrite an existing profile")
        try:
            shutil.copyfile(profile, profile_path)
            run("security", "create-keychain", "-p", password, keychain_name)
            run("security", "set-keychain-settings", "-lut", "21600", keychain_name)
            run("security", "unlock-keychain", "-p", password, keychain_name)
            run(
                "security",
                "list-keychains",
                "-d",
                "user",
                "-s",
                keychain_name,
                *previous_keychains
            )
            run(
                "security",
                "import",
                cert,
                "-k",
                keychain_name,
                "-P",
                os.environ["IOS_CERTIFICATE_PASSWORD"],
                "-T",
                "/usr/bin/codesign",
            )
            run(
                "security",
                "set-key-partition-list",
                "-S",
                "apple-tool:,apple:",
                "-k",
                password,
                keychain_name,
            )
            identities = run(
                "security",
                "find-identity",
                "-v",
                "-p",
                "codesigning",
                keychain_name,
                capture=True,
            ).decode()
            matches = re.findall(r'\b([0-9A-F]{40})\s+"([^"]+)"', identities)
            if len(matches) != 1:
                raise RuntimeError("Expected exactly one usable signing identity")
            signing_hash, signing_name = matches[0]
            apps = list((archive / "Products/Applications").glob("*.app"))
            if len(apps) != 1:
                raise RuntimeError("Archive must contain exactly one application")
            app = apps[0]
            entitlements = temp / "Entitlements.plist"
            entitlements.write_bytes(plistlib.dumps(info["Entitlements"]))
            shutil.copyfile(profile, app / "embedded.mobileprovision")
            nested = list(app.glob("Frameworks/*.framework")) + list(
                app.glob("Frameworks/*.dylib")
            )
            for item in nested:
                run(
                    "codesign",
                    "--force",
                    "--sign",
                    signing_hash,
                    "--keychain",
                    keychain_name,
                    item,
                )
            run(
                "codesign",
                "--force",
                "--sign",
                signing_hash,
                "--keychain",
                keychain_name,
                "--entitlements",
                entitlements,
                "--generate-entitlement-der",
                app,
            )
            archive_info = archive / "Info.plist"
            archive_metadata = plistlib.loads(archive_info.read_bytes())
            properties = archive_metadata.setdefault("ApplicationProperties", {})
            properties["SigningIdentity"] = signing_name
            properties["Team"] = team
            archive_info.write_bytes(plistlib.dumps(archive_metadata))
            opts = {
                "method": method,
                "teamID": team,
                "signingStyle": "manual",
                "provisioningProfiles": {bundle: info["UUID"]},
                "signingCertificate": signing_hash,
                "destination": "export",
                "stripSwiftSymbols": True,
                "manageAppVersionAndBuildNumber": False,
            }
            options = temp / "ExportOptions.plist"
            options.write_bytes(plistlib.dumps(opts))
            # Sign the app and nested frameworks using Xcode's exported provisioning policy.
            run(
                "xcodebuild",
                "-exportArchive",
                "-archivePath",
                archive,
                "-exportPath",
                temp / "export",
                "-exportOptionsPlist",
                options,
                "OTHER_CODE_SIGN_FLAGS=--keychain " + keychain_name,
            )
            ipas = list((temp / "export").glob("*.ipa"))
            if len(ipas) != 1:
                raise RuntimeError("Xcode did not produce an IPA")
            import zipfile

            with zipfile.ZipFile(ipas[0]) as z:
                z.extractall(temp / "verify")
            apps = list((temp / "verify/Payload").glob("*.app"))
            if len(apps) != 1 or not (apps[0] / "embedded.mobileprovision").exists():
                raise RuntimeError("IPA has no provisioned app")
            run("codesign", "--verify", "--deep", "--strict", apps[0])
            shutil.copy2(ipas[0], output)
        finally:
            if profile_path:
                profile_path.unlink(missing_ok=True)
            subprocess.run(
                ["security", "list-keychains", "-d", "user", "-s", *previous_keychains],
                check=False,
                capture_output=True,
            )
            subprocess.run(
                ["security", "delete-keychain", keychain_name],
                check=False,
                capture_output=True,
            )


if __name__ == "__main__":
    main()
