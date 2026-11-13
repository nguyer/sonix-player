#!/usr/bin/env python3
"""Linux-only, isolated protocol and real HTTP range-server checks; no speakers."""
import concurrent.futures
import ctypes
import http.client
import os
from pathlib import Path
import subprocess
import tempfile
from urllib.parse import urlsplit


def main():
    root = Path(__file__).resolve().parents[1]
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="sonix-speakers-") as directory:
        temp = Path(directory)
        flags = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-D_FILE_OFFSET_BITS=64", "-I", str(root)]
        protocol = temp / "test_sonos"
        subprocess.run(flags + [str(root / "tools/test_sonos.c"), str(root / "src/system/remote/sonos.c"),
                               "-o", str(protocol)], check=True)
        subprocess.run([str(protocol)], check=True)
        output = temp / "test_output"
        subprocess.run(flags + ["-pthread", str(root / "tools/test_speaker_output.c"), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=30)
        library = temp / "share.so"
        subprocess.run(flags + ["-shared", "-fPIC", "-pthread", str(root / "src/system/remote/speaker_share.c"),
                               "-o", str(library)], check=True)
        share = ctypes.CDLL(str(library))
        share.speaker_share_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        share.speaker_share_file.restype = ctypes.c_bool
        share.speaker_share_active.restype = ctypes.c_bool
        share.speaker_share_address.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        share.speaker_share_address.restype = ctypes.c_bool
        local = ctypes.create_string_buffer(40)
        assert share.speaker_share_address(b"127.0.0.1", local, len(local))
        assert local.value == b"127.0.0.1"  # works with no wlan0 interface
        assert not share.speaker_share_address(b"invalid", local, len(local))
        assert local.value == b""
        assert not share.speaker_share_address(b"0.0.0.0", local, len(local))
        data = bytes(range(256)) * 1024
        track = temp / "track.mp3"
        track.write_bytes(data)
        url = ctypes.create_string_buffer(256)
        assert share.speaker_share_file(os.fsencode(track), b"127.0.0.1", url, len(url))
        address = urlsplit(url.value.decode())

        def request(method="GET", path=None, headers=None):
            connection = http.client.HTTPConnection(address.hostname, address.port, timeout=5)
            connection.request(method, path or address.path, headers=headers or {})
            response = connection.getresponse()
            status, fields, body = response.status, dict(response.getheaders()), response.read()
            connection.close()
            return status, fields, body

        status, headers, body = request()
        assert status == 200 and body == data and headers["Content-Length"] == str(len(data))
        assert request("HEAD")[2] == b""
        status, headers, body = request(headers={"Range": "bytes=100-199"})
        assert status == 206 and body == data[100:200]
        assert headers["Content-Range"] == f"bytes 100-199/{len(data)}"
        assert request(headers={"Range": "bytes=-100"})[2] == data[-100:]
        assert request(headers={"Range": "bytes=100-"})[2] == data[100:]
        assert request(headers={"Range": "bytes=0-999999"})[2] == data
        for invalid in ["bytes=999999-", "bytes=20-10", "bytes=0-1,4-5", "bytes=-0", "bytes=18446744073709551616-"]:
            assert request(headers={"Range": invalid})[0] == 416, invalid
        assert request("POST")[0] == 405
        assert request(path="/etc/passwd")[0] == 404
        assert request(path="/track/../../etc/passwd")[0] == 404
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            replies = list(pool.map(lambda _: request(headers={"Range": "bytes=123-456"}), range(3)))
        assert all(status == 206 and body == data[123:457] for status, _, body in replies)
        old_path = address.path
        assert share.speaker_share_file(os.fsencode(track), b"127.0.0.1", url, len(url))
        assert request(path=old_path)[0] == 404
        address = urlsplit(url.value.decode())
        assert request()[2] == data
        assert share.speaker_share_active()
        share.speaker_share_revoke()
        assert not share.speaker_share_active()
        assert request()[0] == 404
        assert not share.speaker_share_file(b"/etc/passwd", b"127.0.0.1", url, len(url))
        assert not share.speaker_share_file(os.fsencode(temp), b"127.0.0.1", url, len(url))
        print("Speaker HTTP server tests passed")


if __name__ == "__main__":
    main()
