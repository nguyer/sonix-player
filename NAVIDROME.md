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

API contexts retain credentials in memory and must be cleared on logout. The UI
now persists the account separately as described in the fourth milestone below.
Callers must serialize access to a context
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

Successful connections are now saved and restored automatically (fourth
milestone). **Disconnect** clears the in-memory account, search results and
query; **Forget saved connection** also deletes saved credentials. The editor
masks passwords and clears its text when it is left. New screen labels are
currently English.

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
Cached tracks can be selected again without downloading them. Artwork is now
fetched separately during catalog sync, as described below.

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
2. Playback before download completion and cache management. Search
   results still play one selected song; ordinary library queues support multiple
   remote tracks through the preparation hook.
3. Now-playing and scrobble events only for tracks with server identity, with
   durable offline replay. Manually added local tracks produce no server events.
4. A shared resumable download engine supporting both full-library mirroring
   and selective synchronization of albums, artists, playlists and favorites.

Playback during download, automatic synchronization of audio files and scrobbling
are not enabled yet.

## Fourth milestone: saved account and unified remote library

1. Restart the rebuilt simulator and connect once to save the account.
2. Choose **Streaming > Navidrome > Sync catalog to music library**.
3. Browse the normal Music views (tracks, albums, artists, album artists, genres
   and search). Remote metadata appears beside local files, without downloading
   the audio. Matching titles are separate tracks; no local-file matching occurs.
4. Play from those views normally. Missing remote audio downloads on a worker,
   then the same library queue resumes at the selected position. Album queues
   keep their remaining tracks. Cached audio can play without a connection.

Catalog refresh is manual. It enumerates paginated `search3` with an empty query
(the OpenSubsonic catalog enumeration behavior). Pages are staged in SQLite;
only a successful complete enumeration replaces that account's visible catalog.
Cancellation, network errors and concurrent library scans leave the previous
catalog available. Full scans and Detect changes restore remote entries from
their persisted metadata table, independent of whether audio is cached.

The catalog records server, account, opaque song and album IDs, source-scoped
cache paths, title, artist, album artist, genre, year, disc and track numbers.
There are no empty audio placeholder files. Local-only tracks never pass through
the Navidrome download path. This does not add scrobbling or full-library audio
mirroring yet, and the existing bounded audio cache still applies.

The saved account is `<SONIX_CONFIG>.subsonic-account` (on the device, beside
`/usr/data/device_config.ini`). It is written atomically with owner-only `0600`
permissions and contains reusable credentials **without encryption**. In the
simulator, keep using the same persistent `SONIX_CONFIG` path between runs.
If no settings path is configured or storage is unwritable, the UI explicitly
reports that the connection is available only for the session.

The saved account is authenticated in the background at startup. A temporary
network failure does not erase it; Connect retries with the saved credentials.
**Forget saved connection** removes the saved account and disconnects, retaining
catalog metadata and downloaded audio. Uncached songs then require reconnecting
to the matching account; downloaded songs remain playable.

Run the unified-library checks after building:

```sh
python3 sonix-player/tools/test_subsonic_library.py
```

This uses isolated settings, a temporary library database, a loopback server and
the real account/catalog/library/UI-preparation code. It verifies restricted
account-file permissions, automatic login from saved credentials, two-page
catalog sync, failed/cancelled sync preservation, metadata persistence, full and
incremental rescans, local-file preservation, queued remote download, offline
cache reuse and account deletion. The final playback handoff is wrapped to
avoid producing sound; manual verification of the new library UI remains needed.

## Fifth milestone: artwork before playback

Catalog sync now fetches one 512-pixel cover for each distinct cover ID (falling
back to album ID), with requests authenticated using POST forms. Shared covers
are kept separately under `.local/subsonic-art`, scoped to the server/account.
Cached images are reused on later syncs and are independent of downloaded audio.

The normal album-art loader resolves these covers for remote library tracks even
when their audio files do not exist yet. Thumbnail keys include the fetched
image, so a previous cached absence does not hide newly available artwork.
Images remain available after local library scans and restarts.

After updating, restart the simulator and run **Sync catalog to music library**
once to backfill existing catalog artwork. Missing or invalid covers do not fail
the song catalog import; the completion message reports unavailable covers, and
another sync retries them. Image responses are limited to 2 MiB and must be
JPEG or PNG. Sync cancellation preserves the previous catalog; covers fetched
before cancellation remain reusable in the artwork cache.

The unified-library test covers image decoding before audio exists, shared-cover
deduplication, missing-image retry, invalidation of negative thumbnails, and
artwork survival across scans/restarts. The HTTP tests reject an HTTP-200 JSON
error presented in place of an image.

### Album grouping across restarts

Remote album groups retain their server/account/album identity during local
library reorganization. Folder-based album joining applies only to local files;
remote records with identical titles remain separate when their album IDs differ.
Startup restores group keys from persisted remote metadata and removes orphan
groups left by older builds. Existing empty duplicate albums are repaired by
restarting the updated application, without another catalog sync.

The regression test reproduces the old restart failure using the grouped album
index, then verifies startup repair, populated album lists, local-album
preservation and distinct same-title remote albums with folder joining enabled.

## Sixth milestone: playlist synchronization

Restart the rebuilt simulator, then choose **Streaming > Navidrome > Sync
playlists with server**. Server playlists appear in the normal Music playlist
list. Sync the song catalog first so every playlist entry can be associated
with its server-scoped song identity. Playlist audio uses the same on-demand
download preparation as album and track queues.

Existing local playlists stay local. To upload one explicitly, open its ellipsis
menu in Music > Playlists and choose **Upload to Navidrome**. All entries must
have Navidrome IDs from the connected account; a playlist containing local-only
files or another account's tracks is rejected before anything is sent. No song
matching or partial upload is performed. Successfully uploaded playlists become
linked to their server IDs. Their menu then offers **Sync with Navidrome**.

Server refresh remains available through the sync actions. Device edits to linked
playlists now synchronize automatically as described below. Ordered entries
(including duplicates), renames and deletions synchronize
in both directions. A local deletion of an owned linked playlist removes the
server copy on the next successful sync if the server version has not changed.
Deleting a server playlist removes its unchanged local mirror on sync. Local
playlist edits, pending deletions and account/playlist mappings survive restarts.

Names are not identities: remote IDs are stored separately. A server playlist
whose name collides with a local playlist receives a distinct local display
name, without overwriting the local playlist or changing the server's name.
Read-only shared playlists can be imported; attempts to send local changes to
them are refused and the local edits remain available.

Each link records the last synchronized title and ordered-ID fingerprint. If
both sides changed differently, sync stops with a conflict message and preserves
both versions. Resolve by making the contents/titles agree, or restoring one
side to its last synchronized version, then sync again. The Subsonic API does
not provide conditional versioned playlist writes: conflict checks compare
snapshots taken during sync, not a server-side atomic compare-and-swap.

Local playlist replacement and mapping updates are atomic. Local revision
checks reject updates if playlists changed during the request. Network errors
do not advance the baseline. If creation succeeded but its reply was lost, a
retry adopts an identical owned server playlist rather than creating another;
ambiguous multiple matches require manual inspection. Sync is atomic per local
playlist, not for the entire account: already completed playlists stay synced
when a later conflict, network failure or cancellation stops the run. A remote
content update and rename are separate API requests; a partial success may
require aligning the two versions and retrying.

Requests and responses remain bounded (2 MiB each; at most 1,000 server playlist
headers / 10,000 entries in one snapshot). Oversized or malformed data fails
instead of silently dropping entries. These limits can reject a large playlist
before the nominal entry maximum if its full server response is too large.

The loopback integration suite verifies pulls and pushes, order/duplicates,
renames in both directions, two-sided conflicts, deletions, empty playlists,
explicit uploads, local-name collision protection, invalid local tracks,
read-only shared playlists, persisted offline edits/deletions, network failures,
recovery after a lost create reply, and uploads larger than the old 4 KiB form
buffer. Live-server playlist editing and target-device testing remain to be done.

## Seventh milestone: automatic server-origin playlist edits

The built-in playlist page separates **Local playlists** and **Navidrome
playlists**. Origin is the persisted server/account/playlist link, not a name
prefix or the tracks a playlist happens to contain. Creating or modifying a
local playlist never makes it a server playlist. Explicit **Upload to
Navidrome** remains the opt-in way to link a local playlist.

Edits to linked playlists (contents, ordering, renames and deletions) queue
automatic background synchronization. A short quiet period coalesces edit
bursts. Pending work lives in SQLite and survives restart; the saved matching
account is used when available. Failure retries back off for about 30 seconds.
Disconnecting suspends automatic work until that account is connected again.

Only linked playlist tables receive edit triggers. Server imports and successful
syncs clear their own pending markers, preventing feedback loops. Background
passes process pending linked playlists only: they neither upload local lists
nor import every newly created server playlist. Use **Sync playlists with
server** to refresh server-origin additions and changes when there are no local
edits. Existing links from older builds are reconciled once during upgrade.

Background work does not navigate away from the current screen. Conflicting or
read-only edits remain queued and reported; healthy pending playlists can still
sync. Identical repeated errors do not repeatedly display notifications. Local
playlist backups imported under a server playlist's name become separate local
copies, avoiding accidental replacement or deletion of server identity.

Tests cover zero automatic-sync calls for local-only edits, background uploads
without navigation changes, persistent queued edits, failure/retry behavior,
healthy playlists progressing beside a read-only conflict, automatic deletion,
upgrade of existing links, and backup-import origin isolation.
