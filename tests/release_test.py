import importlib.util
import os
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "scripts/release.py"
spec = importlib.util.spec_from_file_location("afd_release", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseTests(unittest.TestCase):
    def test_stable_tags(self):
        for tag in ("v0.1.0", "v1.0.0", "v2.10.300"):
            self.assertFalse(module.validate(tag))

    def test_prerelease_tags(self):
        for tag in ("v2.0.0-beta.1", "v1.0.0-rc.0"):
            self.assertTrue(module.validate(tag))

    def test_rejects_non_versions(self):
        for tag in (
            "main",
            "v1.2",
            "1.2.3",
            "v01.2.3",
            "v1.2.3-beta.01",
            "v1.2.3;echo bad",
            "v1.2.3\n",
            "v1.2.3-",
        ):
            with self.assertRaises(RuntimeError):
                module.validate(tag)

    def test_local_publication_guard(self):
        old = os.environ.pop("GITHUB_ACTIONS", None)
        try:
            with self.assertRaises(RuntimeError):
                module.publish("v1.0.0")
        finally:
            if old is not None:
                os.environ["GITHUB_ACTIONS"] = old

    def test_architecture_names_do_not_overlap(self):
        prefix = "AlternativeFastDelivery-v1.0.0-android-x86"

        def matches(name):
            return name.startswith(prefix) and name[len(prefix) : len(prefix) + 1] in (
                ".",
                "-",
            )

        self.assertTrue(matches(prefix + ".apk"))
        self.assertTrue(matches(prefix + "-development.apk"))
        self.assertFalse(matches(prefix + "_64.apk"))


if __name__ == "__main__":
    unittest.main()
