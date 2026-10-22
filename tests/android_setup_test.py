"""Run Linux SDK bootstrap with fake SDK tools and real interrupted HTTP downloads."""

from contextlib import contextmanager
import http.server
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/setup-android.sh"
SDKMANAGER = """#!/usr/bin/env bash
set -eu
phase=install
[[ "$*" == *--licenses* ]] && phase=licenses
count_file="$TEST_ROOT/$phase.count"
count=0
[[ ! -f "$count_file" ]] || count=$(cat "$count_file")
count=$((count + 1))
echo "$count" > "$count_file"
printf '%s\\n' "$*" >> "$TEST_ROOT/sdk.calls"
if [[ "$phase" == "${FAIL_PHASE:-}" && "$count" -le "${FAIL_COUNT:-0}" ]]; then
    exit 23
fi
"""


@unittest.skipUnless(
    os.name == "posix"
    and all(shutil.which(tool) for tool in ("bash", "curl", "unzip")),
    "Android CI setup requires POSIX bash, curl and unzip",
)
class AndroidSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.sdk = self.root / "sdk with spaces"
        self.manager = self.sdk / "cmdline-tools/latest/bin/sdkmanager"
        self.environment = self.root / "github.env"
        self.env = dict(
            os.environ,
            ANDROID_HOME=str(self.sdk),
            RUNNER_TEMP=str(self.root),
            GITHUB_ENV=str(self.environment),
            TEST_ROOT=str(self.root),
            PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
        )
        self.env.pop("FAIL_PHASE", None)
        self.env.pop("FAIL_COUNT", None)
        self.write_tool(
            "sleep", '#!/usr/bin/env bash\necho "$*" >> "$TEST_ROOT/sleeps"\n'
        )
        self.write_tool("curl", "#!/usr/bin/env bash\nexit 91\n")

    def write_tool(self, name, content):
        path = self.bin / name
        path.write_text(content)
        path.chmod(0o755)

    def install_manager(self):
        self.manager.parent.mkdir(parents=True)
        self.manager.write_text(SDKMANAGER)
        self.manager.chmod(0o755)

    def run_setup(self):
        return subprocess.run(
            ["bash", str(SCRIPT)],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=45,
        )

    def check_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            self.environment.read_text(),
            f"ANDROID_NDK_HOME={self.sdk}/ndk/27.2.12479018\n",
        )
        self.assertIn(
            "platforms;android-36 build-tools;36.0.0 ndk;27.2.12479018",
            (self.root / "sdk.calls").read_text(),
        )

    def test_existing_manager_avoids_bootstrap_download(self):
        self.install_manager()
        self.check_success(self.run_setup())
        self.assertEqual((self.root / "licenses.count").read_text().strip(), "1")
        self.assertEqual((self.root / "install.count").read_text().strip(), "1")

    def test_sdkmanager_retries_license_and_package_failures(self):
        self.install_manager()
        for phase in ("licenses", "install"):
            with self.subTest(phase=phase):
                self.env.update(FAIL_PHASE=phase, FAIL_COUNT="1")
                self.check_success(self.run_setup())
                self.assertEqual(
                    (self.root / f"{phase}.count").read_text().strip(), "2"
                )
                for file in self.root.glob("*.count"):
                    file.unlink()
                self.environment.unlink()

    def test_persistent_sdk_error_stops_before_exporting_ndk(self):
        self.install_manager()
        self.env.update(FAIL_PHASE="install", FAIL_COUNT="99")
        result = self.run_setup()
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertEqual((self.root / "install.count").read_text().strip(), "3")
        self.assertFalse(self.environment.exists())

    @contextmanager
    def archive_server(self, payload, interrupt_first=False):
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append(self.request_version)
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                if interrupt_first and len(requests) == 1:
                    self.wfile.write(payload[:32])
                    self.wfile.flush()
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                self.wfile.write(payload)

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                self.env["TEST_DOWNLOAD_URL"] = (
                    f"http://127.0.0.1:{server.server_port}/tools.zip"
                )
                self.env["TEST_REAL_CURL"] = shutil.which("curl")
                self.write_tool(
                    "curl",
                    """#!/usr/bin/env bash
args=()
for arg in "$@"; do
    if [[ "$arg" == https://dl.google.com/android/repository/* ]]; then
        arg="$TEST_DOWNLOAD_URL"
    fi
    args+=("$arg")
done
exec "$TEST_REAL_CURL" "${args[@]}"
""",
                )
                yield requests
            finally:
                server.shutdown()
                thread.join()

    def test_fallback_download_recovers_from_truncated_response(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            info = zipfile.ZipInfo("cmdline-tools/bin/sdkmanager")
            info.external_attr = 0o100755 << 16
            archive.writestr(info, SDKMANAGER)
        with self.archive_server(data.getvalue(), interrupt_first=True) as requests:
            self.check_success(self.run_setup())
        self.assertEqual(requests, ["HTTP/1.1", "HTTP/1.1"])
        self.assertEqual(
            (self.root / "android-tools.zip").read_bytes(), data.getvalue()
        )

    def test_invalid_download_never_runs_sdkmanager(self):
        with self.archive_server(b"not a zip file"):
            result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "sdk.calls").exists())
        self.assertFalse(self.environment.exists())


if __name__ == "__main__":
    unittest.main()
