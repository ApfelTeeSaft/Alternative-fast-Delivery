#!/usr/bin/env python3
"""Tag validation and CI-only, idempotent release publication."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
TARGETS = [
    ("windows", "x64"),
    ("windows", "arm64"),
    ("linux", "x64"),
    ("linux", "arm64"),
    ("macos", "x64"),
    ("macos", "arm64"),
    ("android", "arm64-v8a"),
    ("android", "armeabi-v7a"),
    ("android", "x86"),
    ("android", "x86_64"),
    ("ios", "arm64"),
    ("ios-simulator", "x64"),
]


def validate(tag):
    if not re.fullmatch(
        r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(?:[0-9A-Za-z-]+)(?:\.[0-9A-Za-z-]+)*)?",
        tag,
    ):
        raise RuntimeError(
            "Expected a semantic version tag such as v1.0.0 or v2.0.0-beta.1"
        )
    if "-" in tag and any(
        t.isdecimal() and len(t) > 1 and t[0] == "0"
        for t in tag.split("-", 1)[1].split(".")
    ):
        raise RuntimeError("Numeric prerelease identifiers cannot have leading zeroes")
    return "-" in tag


def prepare(tag):
    files = sorted(DIST.iterdir())
    for osname, arch in TARGETS:
        prefix = f"AlternativeFastDelivery-{tag}-{osname}-{arch}"
        candidates = [
            f
            for f in files
            if f.name.startswith(prefix)
            and f.name[len(prefix) : len(prefix) + 1] in (".", "-")
            and not f.name.endswith(".json")
        ]
        if len(candidates) != 1:
            raise RuntimeError(
                f"Expected one validated deliverable for {osname}/{arch}: {candidates}"
            )
        f = candidates[0]
        if f.stat().st_size < 50000:
            raise RuntimeError("Artifact is implausibly small: " + f.name)
        if f.name.endswith((".zip", ".apk", ".ipa")):
            with zipfile.ZipFile(f) as z:
                if z.testzip():
                    raise RuntimeError("Archive CRC failure")
                names = z.namelist()
                if f.suffix == ".apk" and not any(
                    n.startswith("lib/") and n.endswith(".so") for n in names
                ):
                    raise RuntimeError("APK has no native libraries")
                if f.suffix == ".ipa" and not any(
                    n.endswith("/embedded.mobileprovision") for n in names
                ):
                    raise RuntimeError("IPA has no device profile")
        elif f.name.endswith(".tar.gz"):
            with tarfile.open(f) as t:
                if not any(
                    m.name.endswith("/bin/alternativefastdelivery") and m.size > 50000
                    for m in t
                ):
                    raise RuntimeError("Linux package lacks executable")
    for f in files:
        if not f.name.startswith(f"AlternativeFastDelivery-{tag}-"):
            raise RuntimeError("Unrelated asset in release directory: " + f.name)
    checksums = []
    for f in files:
        with f.open("rb") as stream:
            checksums.append(
                hashlib.file_digest(stream, "sha256").hexdigest() + "  " + f.name + "\n"
            )
    (DIST / "SHA256SUMS.txt").write_text("".join(checksums))
    signing = (
        "Device-registered IPA; installation is limited to the configured profile."
        if any(f.suffix == ".ipa" for f in files)
        else "iOS archive REQUIRES SIGNING; it cannot install directly on stock devices."
    )
    notes = f"""AlternativeFastDelivery {tag}

Commit: {os.environ.get('GITHUB_SHA','unknown')}
Build date (UTC): {__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat()}

Windows x64/ARM64: extract ZIP, run bin/alternativefastdelivery.exe.
Linux x64/ARM64: extract tar.gz, run AlternativeFastDelivery; Ubuntu 24.04 build baseline.
macOS Intel/Apple Silicon: extract the matching app ZIP. Unsigned unless separately signed by the owner.
Android: choose your ABI. Development APKs have temporary test identities and cannot provide stable updates. Configure a persistent signing key for production.
iOS: {signing} Simulator bundles cannot install on devices.

SHA-256 checksums are in SHA256SUMS.txt. Per-target JSON files record build provenance.
BitTorrent may upload pieces. Email and index content remain local. No party affiliation.
"""
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build/release-notes.md").write_text(notes)


def publish(tag):
    # Deliberately unavailable from local invocations and manually dispatched jobs.
    if (
        os.environ.get("GITHUB_ACTIONS") != "true"
        or os.environ.get("GITHUB_EVENT_NAME") != "push"
        or os.environ.get("GITHUB_REF") != "refs/tags/" + tag
    ):
        raise RuntimeError("Publication is permitted only in tag-push GitHub Actions")
    gh = ["gh", "release"]
    existing = subprocess.run(
        gh + ["view", tag, "--json", "assets,isDraft,body"],
        capture_output=True,
        text=True,
    )
    if existing.returncode == 0:
        info = json.loads(existing.stdout)
        remote = {f["name"] for f in info["assets"]}
        if not info["isDraft"]:
            expected = {f.name for f in DIST.iterdir()}
            if remote == expected:
                print("Release already complete; no changes")
                return
            raise RuntimeError(
                "Existing published release differs; refusing to overwrite assets"
            )
        if not info["body"].startswith("AlternativeFastDelivery " + tag):
            raise RuntimeError("Refusing to modify an unrelated draft release")
    else:
        subprocess.run(
            gh
            + [
                "create",
                tag,
                "--verify-tag",
                "--draft",
                "--title",
                "AlternativeFastDelivery " + tag,
                "--notes-file",
                str(ROOT / "build/release-notes.md"),
            ],
            check=True,
        )
        remote = set()
    for f in sorted(DIST.iterdir()):
        if f.name not in remote:
            subprocess.run(gh + ["upload", tag, str(f)], check=True)
    subprocess.run(
        gh
        + ["edit", tag, "--draft=false", "--prerelease=" + str(validate(tag)).lower()],
        check=True,
    )


if __name__ == "__main__":
    tag = os.environ.get("RELEASE_TAG", "")
    pre = validate(tag)
    if sys.argv[1] == "validate":
        with open(os.environ["GITHUB_OUTPUT"], "a") as f:
            f.write(f"tag={tag}\nprerelease={str(pre).lower()}\n")
    elif sys.argv[1] == "prepare":
        prepare(tag)
    elif sys.argv[1] == "publish":
        publish(tag)
    else:
        raise RuntimeError("Unknown operation")
