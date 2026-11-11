"""Loopback-only integration test. No Navidrome account or external network."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import io
import wave
import time
from urllib.parse import parse_qs

seen_salts = set()
errors = []
audio_buffer = io.BytesIO()
with wave.open(audio_buffer, "wb") as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(44100)
    wav.writeframes(b"\x00\x00" * 44100)
audio_bytes = audio_buffer.getvalue()
download_count = 0


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path != "/navidrome/test-audio":
            errors.append(AssertionError("Unexpected GET or followed authenticated redirect"))
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(audio_bytes)))
        self.end_headers()
        try:
            self.wfile.write(audio_bytes)
        except (BrokenPipeError, ConnectionResetError):
            pass  # GET smoke test deliberately closes after reading the header.

    def do_POST(self):
        global download_count
        try:
            assert "?" not in self.path
            assert self.headers["Content-Type"] == "application/x-www-form-urlencoded"
            form = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
            assert form["u"] in (["test+user"], ["another-user"])
            assert form["v"] == ["1.16.1"]
            assert form["f"] == ["json"]
            salt = form["s"][0]
            assert salt not in seen_salts
            seen_salts.add(salt)
            token = hashlib.md5(("test-password" + salt).encode()).hexdigest()
            if self.path == "/navidrome/rest/download.view":
                assert form["t"] == [token]
                song_id = form["id"][0]
                if song_id == "redirect":
                    self.send_response(307)
                    self.send_header("Location", "/must-not-follow")
                    self.end_headers()
                    return
                payload = b'{"subsonic-response":{"status":"failed"}}' if song_id == "api-error" else audio_bytes
                self.send_response(200)
                self.send_header("Content-Type", "application/json" if song_id == "api-error" else "audio/wav")
                length = len(payload) + 100 if song_id == "truncated" else len(payload)
                if song_id == "oversized":
                    length = 512 * 1024 * 1024 + 1
                    payload = b""
                self.send_header("Content-Length", str(length))
                self.end_headers()
                if song_id == "slow":
                    time.sleep(1)
                    return
                self.wfile.write(payload)
                download_count += 1
                return
            response = {"status": "ok", "version": "1.16.1"}
            if form["t"] != [token]:
                response = {"status": "failed", "error": {"code": 40, "message": "Wrong password"}}
            elif self.path == "/navidrome/rest/search3.view":
                assert form["query"] == ["AC/DC & live"]
                assert form["songOffset"] == ["10"]
                assert form["songCount"] == ["2"]
                response["searchResult3"] = {"song": [{"id": "server-song-1", "duration": 185, "suffix": "wav"}]}
            else:
                assert self.path == "/navidrome/rest/ping.view"
            body = json.dumps({"subsonic-response": response}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as error:
            errors.append(error)
            self.send_error(500)


root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonix-http-test-") as directory:
    executable = str(Path(directory) / "test")
    subprocess.run([
        "cc", "-Wall", "-Wextra", "-Werror", "-g", "-DHOST_BUILD=1", "-I.",
        "tools/test_subsonic_http.c", "src/system/streaming/subsonic.c",
        "src/system/streaming/subsoniccache.c",
        "src/system/core/json.c", "src/system/core/md5.c",
        "src/system/net/http.c", "src/system/net/tls.c",
        "-pthread", "-ldl", "-o", executable,
    ], cwd=root, check=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        subprocess.run([executable, f"http://127.0.0.1:{server.server_port}/navidrome", directory],
                       check=True, timeout=30)
        assert not errors, errors
        assert len(seen_salts) == 10
        assert download_count == 5
        cached = list(Path(directory).glob(".local/subsonic-cache/*.wav"))
        assert len(cached) == 2
        assert all(path.read_bytes() == audio_bytes for path in cached)
        assert not list(Path(directory).glob(".local/subsonic-cache/*.part"))
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
