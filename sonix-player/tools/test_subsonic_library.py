"""Link the host application with a test entry point; use an isolated library."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import wave
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import struct
import zlib
from urllib.parse import parse_qs

failed_sync = False
errors = []
missing_art = True
art_requests = {}


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


cover_png = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
             + png_chunk(b"IDAT", zlib.compress((b"\x00" + b"\x20\x80\xe0" * 2) * 2)) + png_chunk(b"IEND", b""))


class CatalogServer(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        global failed_sync, missing_art
        if self.path == "/navidrome/test-fail-next-sync":
            failed_sync = True
        elif self.path == "/navidrome/test-recover-art":
            missing_art = False
        else:
            errors.append("unexpected GET")
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")

    def do_POST(self):
        try:
            form = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode(), keep_blank_values=True)
            assert form["u"] == ["user"]
            assert form["t"] == [hashlib.md5(("private-password" + form["s"][0]).encode()).hexdigest()]
            if self.path == "/navidrome/rest/getCoverArt.view":
                cover_id = form["id"][0]
                assert cover_id in ("shared-cover", "missing-cover")
                assert form["size"] == ["512"]
                art_requests[cover_id] = art_requests.get(cover_id, 0) + 1
                if cover_id == "missing-cover" and missing_art:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(cover_png)))
                self.end_headers()
                self.wfile.write(cover_png)
                return
            if self.path in ("/navidrome/rest/ping.view", "/navidrome/rest/download.view"):
                payload = b'{"subsonic-response":{"status":"ok"}}' if "ping" in self.path else b"fLaC" + b"fixture" * 100
                self.send_response(200)
                self.send_header("Content-Type", "application/json" if "ping" in self.path else "audio/flac")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            assert self.path == "/navidrome/rest/search3.view"
            assert form["query"] == [""]
            assert form["songCount"] == ["50"]
            offset = int(form["songOffset"][0])
            assert offset in (0, 50)
            if failed_sync and offset == 50:
                self.send_error(503)
                return
            songs = [{"id": f"catalog-{i}", "title": f"{'Unpublished' if failed_sync else 'Catalog'} title {i}",
                      "artist": "Catalog artist", "albumArtist": "Catalog album artist", "genre": "Rock",
                      "album": "Catalog album", "albumId": "catalog-album", "suffix": "flac",
                      "coverArt": "shared-cover" if i < 50 else "missing-cover",
                      "duration": 180, "track": i + 1, "year": 2020, "discNumber": 2}
                     for i in range(offset, min(offset + 50, 51))]
            body = json.dumps({"subsonic-response": {"status": "ok", "searchResult3": {"song": songs}}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as error:
            errors.append(str(error))
            self.send_error(500)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sonix-library-test-") as directory:
    tmp = Path(directory)
    (tmp / "settings.ini").write_text("")
    (tmp / "Local.wav.tags").write_text("album=Remote album\nartist=Local artist\nalbum_artist=Local artist\n")
    with wave.open(str(tmp / "Local.wav"), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(44100)
        audio.writeframes(b"\x00\x00" * 4410)
    cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2", "freetype2"], text=True))
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I.", "-DHOST_BUILD=1", "-DLV_CONF_INCLUDE_SIMPLE=1", *cflags, "-c",
                    "tools/test_subsonic_library.c", "-o", str(tmp / "test.o")], cwd=root, check=True)
    # Keep the application's global definitions, but replace its main function.
    subprocess.run(["objcopy", "--redefine-sym", "main=sonix_simulator_main",
                    "build_host/main.o", str(tmp / "main.o")], cwd=root, check=True)
    objects = sorted(str(p) for p in (root / "build_host").rglob("*")
                     if p.suffix in (".o", ".opp") and p not in
                     (root / "build_host/main.o", root / "build_host/gui/streaming/subsonicpage.o"))
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", "sdl2", "freetype2", "opusfile", "wavpack"], text=True))
    subprocess.run(["g++", str(tmp / "test.o"), str(tmp / "main.o"), *objects,
                    *flags, "-lpthread", "-lm", "-ldl", "-lasound",
                    "-Wl,--wrap=device_state_play_queue_index", "-Wl,--wrap=player_refresh_now_playing",
                    "-o", str(tmp / "test")],
                   cwd=root, check=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), CatalogServer)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([str(tmp / "test"), str(tmp), f"http://127.0.0.1:{server.server_port}/navidrome"],
                       cwd=root, check=True, timeout=40)
        assert not errors, errors
        assert art_requests == {"shared-cover": 1, "missing-cover": 2}, art_requests
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
