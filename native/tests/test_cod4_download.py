#!/usr/bin/env python3
"""Run Aspyr's actual i386 downloader against a loopback HTTP server."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(range(256)) * 1024
requests = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        agent = self.headers.get("User-Agent", "")
        requests.append((self.path, agent))
        if self.path == "/denied" or "CFNetwork" in agent:
            self.send_error(403)
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/~images_00.iwd")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        body = PAYLOAD[:4096] if self.path == "/truncated" else PAYLOAD
        for start in range(0, len(body), 4096):
            self.wfile.write(body[start:start + 4096])
            self.wfile.flush()
            time.sleep(0.005)


def main():
    loader = Path(os.environ.get("COD4_DOWNLOAD_TEST_LOADER", ROOT / "build/game_loader"))
    image = ROOT / "build/COD4MP-Compat.app/Contents/SharedSupport/COD4MP.image"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="cod4-download-") as directory:
            for route, success in [("/~images_00.iwd", True), ("/redirect", True),
                                   ("/denied", False), ("/truncated", False)]:
                destination = Path(directory) / "download.iwd"
                destination.unlink(missing_ok=True)
                env = os.environ | {
                    "LP32_COD4_DOWNLOAD_SELFTEST": "1",
                    "LP32_COD4_DOWNLOAD_TEST_URL": f"http://127.0.0.1:{server.server_port}{route}",
                    "LP32_COD4_DOWNLOAD_TEST_PATH": str(destination),
                    "LP32_BACKGROUND_TEST": "1", "LP32_MUTE_AUDIO": "1",
                    "LP32_NO_DIAGNOSTIC_LOG": "1",
                    "LP32_FAIL_ON_UNSUPPORTED_IMPORT": "1",
                }
                result = subprocess.run([str(loader), str(image)], env=env,
                                        capture_output=True, text=True, timeout=15)
                output = result.stdout + result.stderr
                match = re.search(r"cod4-download-selftest: status=(-?\d+) bytes=(\d+) pendingBytes=(\d+)", output)
                assert result.returncode == 0 and match, output
                status, count, pending = map(int, match.groups())
                if success:
                    assert status == 1 and count == len(PAYLOAD), output
                    assert 0 < pending <= count, output
                    assert hashlib.sha256(destination.read_bytes()).digest() == hashlib.sha256(PAYLOAD).digest()
                else:
                    assert status < 0, output
                    assert not destination.exists(), "Failed download left a partial mod"
                print(f"PASS {route}: status={status} bytes={count}")
            assert len(requests) == 5, requests
            assert all(agent == "COD4MPCompat/1.7.2" for _, agent in requests), requests
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
