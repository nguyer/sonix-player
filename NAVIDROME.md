# Navidrome integration

## First milestone: API foundation

`sonix-player/src/system/streaming/subsonic.h` provides an account context,
connection check (`ping`), and paginated song search (`search3`). It uses the
existing HTTP, JSON and MD5 code without adding build dependencies. The normal
Makefile discovers the new source automatically.

Requests use the Subsonic 1.16.1 protocol, JSON responses, and a fresh random
salt with MD5(password + salt) authentication. Parameters are POST form data so
credentials do not appear in URL paths logged by the HTTP client. Use the final
server root address, including any reverse-proxy prefix, without `/rest`.
Prefer HTTPS: authentication tokens are replayable if intercepted over HTTP.

Contexts retain credentials only in memory and must be cleared on logout. No
account settings are persisted yet. Callers must serialize access to a context
and perform network calls on a worker thread. Callers must ignore the output
array when a search returns -1. Song IDs remain opaque strings.

Run the isolated tests from the repository root:

```sh
sh sonix-player/tools/test_subsonic.sh
```

These tests mock the HTTP transport and exercise the actual API client, JSON
parser and MD5 implementation. They cover authentication, fresh salts, URL
encoding, reverse-proxy prefixes, pagination, API/HTTP/network failures,
malformed replies, missing song IDs and credential clearing. They do not prove
compatibility with a live Navidrome server or the device firmware.

## Second milestone: account and song browsing

Open **Streaming > Navidrome** in the rebuilt simulator. Enter the server root
URL, username and password, then select **Connect**. A successful connection
enables **Search songs**. Results show title, artist and album, with twenty songs
per page and Previous/Next actions. Selecting a song downloads it and starts
playback as described below. The last full page may offer Next even when
the following page is empty.

Only the server URL and username are saved, after a successful connection. The
password is kept in the account's memory until disconnect or process exit;
restarting requires entering it again. **Disconnect** clears account credentials,
search results and the query. The editor masks passwords and clears its text
when it is left. New screen labels are currently English.

Network requests run on a worker thread with one request at a time. The UI
consumes completed snapshots on its own thread. Account edits and additional
requests are unavailable while busy; Back still works, and a late result does
not redirect a user who has left the Navidrome page.

Additional checks from the repository root:

```sh
python3 sonix-player/tools/test_subsonic_http.py
make -C sonix-player host -j2
python3 sonix-player/tools/test_subsonic_startup.py
```

The HTTP test runs the real HTTP/JSON/MD5 code against a temporary loopback
server. It verifies POST form encoding, reverse-proxy prefixes, authentication,
search pagination and rejected passwords. The startup check uses a dummy SDL
display with software rendering and temporary settings; it does not exercise
touch interaction or visually verify layout. Connection and song search have
also been verified manually against a live Navidrome server. Live playback and
target-device testing are still required.

## Third milestone: selected-song playback

Restart the rebuilt simulator, connect, search and select a song. The original
file is downloaded fully on a worker thread, then handed to the normal audio
player as a one-song queue. Existing decoding, seeking and playback controls
are reused. Other search results are not automatically queued yet. Playback
does not begin while the file is still arriving, and no transcoding is requested.
Formats that require runtime codec libraries retain those existing requirements.

Files are stored under `<music-storage-root>/.local/subsonic-cache/`. Cache keys
include the server root, username and opaque song ID, and a credential-free
`.identity` sidecar preserves that association across restarts. Title, artist,
album and track number are supplied through the existing `.tags` metadata path.
Cached tracks can be selected again without downloading them. Artwork is not
downloaded yet.

The cache reserves space for a 512 MiB maximum track within a 1 GiB budget.
This is deliberately conservative: new downloads stop once existing cache
contents exceed 512 MiB. There is no automatic eviction or cache-management UI
yet; remove old files from this cache directory while playback is stopped if
it fills up. This cache is separate from the planned offline synchronization
library. Interrupted files are deleted, and the audio file is published by
rename only after its metadata and identity have been written successfully.

Select **Cancel download** to request cancellation. A blocked audio read is
woken; cancellation during connection setup can take the connection timeout.
Requests waiting for another service's download turn must wait for that turn
before completing cancellation. Transfers share the existing download turn,
participate in cross-service cancellation, and prevent Wi-Fi parking and device
suspend while downloading. Authenticated binary POST requests do not follow
redirects; configure the final server URL.

Leaving the Navidrome screens or changing the playback queue before a download
finishes prevents that result from starting playback unexpectedly. Return to
the results and select the cached song again to start it.

The loopback HTTP test now verifies a complete WAV download byte for byte,
cache reuse and account separation, identity recovery, truncated responses,
HTTP-200 API errors, oversized responses, cancellation and redirect refusal.
It also checks the existing GET streaming path. The host build and headless
startup check pass; audible playback against a live server is not yet verified.

## Remaining integration

1. Album, artist and playlist browsing beyond the current song search.
2. Playback before download completion, multi-song queueing, artwork and cache
   management. Persist server identity in the unified library database.
3. Now-playing and scrobble events only for tracks with server identity, with
   durable offline replay. Manually added local tracks produce no server events.
4. A shared resumable download engine supporting both full-library mirroring
   and selective synchronization of albums, artists, playlists and favorites.

Playback during download, persistent catalog, automatic synchronization and
scrobbling are not enabled yet.
