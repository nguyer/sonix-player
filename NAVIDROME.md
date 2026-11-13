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

## Connecting and synchronizing

Open **Streaming > Navidrome**. Enter the server root URL, username and password,
then press **Connect**. Empty passwords appear blank; entered or saved passwords
appear as dots. Successful connections are saved and restored automatically.

Press **Sync** to update the music catalog, artwork and server playlists, then
download music selected by the offline settings. Browse and play songs from the
built-in music library; the initial test-only song search has been removed.

The red trash icon beside the Navidrome title removes the connection after confirmation. It removes login
credentials and stops server synchronization. Library entries, playlists and
downloaded music remain on the device; completed downloads remain playable.
Connect again to restore access to uncached server tracks and synchronization.
The editor masks passwords and clears its text when it is left. New labels are
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
contents exceed 512 MiB. There is no automatic quota eviction; use Remove download
or Remove all downloads under Offline / cache settings if it fills up. Pinned offline downloads are separate from this playback-cache budget. Interrupted files are deleted, and the audio file is published by
rename only after its metadata and identity have been written successfully.

Select **Cancel sync** to request cancellation. A blocked audio read is
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
2. Choose **Streaming > Navidrome > Sync**.
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
**Remove connection** removes the saved account and disconnects, retaining
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

After updating, restart the simulator and run **Sync**
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
Forgetting the saved connection suspends automatic work until that account is connected again.

Only linked playlist tables receive edit triggers. Server imports and successful
syncs clear their own pending markers, preventing feedback loops. Background
passes process pending linked playlists only: they neither upload local lists
nor import every newly created server playlist. Use **Sync** to refresh server-origin additions and changes when there are no local
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


## Offline synchronization

Open **Navidrome → Offline / cache settings** after connecting. Automatic sync is
manual by default. Choose **Full library** to mirror the catalog, or **Selective** to
download from the library itself. Album and playlist pages show a download icon
to the left of the other toolbar controls (including the alternative album header). When all Navidrome tracks
in that view are downloaded, it changes to a trash icon for removing downloads.
Local-only tracks are unaffected.

Album/playlist track menus and the now-playing three-dot menu offer **Download**
or **Remove download** for Navidrome tracks in Selective mode. Downloads start
immediately when connected, subject to storage and charging settings. Offline
selections are saved for a later Sync. Linked server playlists remain selected
as playlists, so later synchronization includes new playlist tracks. Local
playlists select their server tracks individually.

Explicitly removing a song excludes it from overlapping album/playlist rules,
so automatic sync does not immediately download it again. Downloading the song
or its album/playlist again clears that exclusion. Existing legacy selections
remain effective; there are no separate selection menus in offline settings.

The red **Remove all downloads** button asks for confirmation, clears selections,
resets to Selective mode with automatic caching off, and deletes downloaded music for this connection.
It keeps local music, other connections' downloads, library metadata, playlists
and artwork. Removing a currently playing download stops that track.

Settings persist per server account. Configure a storage limit (or unlimited),
a free-space reserve and optionally charging-only downloads. Automatic sync
and its refresh interval are configured on the main Navidrome page and apply
to catalog, artwork, playlists and any configured offline downloads. The main Navidrome **Sync** button refreshes the catalog and playlists, then
downloads selected tracks. **Cancel sync** keeps completed downloads.
Foreground playback takes priority over background downloads.

Use the library's explicit Remove download actions to remove selected offline
copies. Independently, a successful catalog sync automatically cleans up files
for tracks removed from that server. Local music is preserved, and currently
playing/paused removed files are deferred until playback stops.

When server checks fail, uncached server tracks are dimmed and cannot be
selected for playback. Next/previous playback skips them. Complete cached
tracks and local music remain playable. On hardware, Wi-Fi link notifications
immediately disable uncached tracks when disconnected. Reconnection triggers
a server check. Healthy servers are not periodically pinged; a failed request
enables recovery checks every 30 seconds while the network is connected.
Successful requests restore availability and stop recovery checks. Wi-Fi
association does not itself guarantee server reachability. The simulator uses
host networking, so server request failures trigger its recovery checks.

This first implementation downloads original files, with a 512 MiB per-track
limit. Completed tracks survive cancellation/restart; an interrupted individual
file restarts from the beginning. Transcoding and HTTP byte-range resume are
not implemented yet. Changes in the server-reported file size trigger a fresh
download. Automatic sync requires the app to be running.

Offline mode offers Selective and Full library as choice chips; there is no Off option. On the main page, Automatic sync is an on/off
switch; its Refresh interval slider is shown only when enabled. Offline storage
limit uses a stepped slider. Reserve free space is shown only for Unlimited;
the saved reserve remains enforced as a safety margin for bounded downloads.
Charging-only downloads use a switch. Storage includes an Unlimited
position. Offline sync is started from the main Sync button.

Navidrome settings and selection lists retain their scroll position when a
control changes. The Offline / cache settings row has a submenu arrow.


In Selective mode, **Automatically cache recently played** controls retention
of playback downloads. When enabled (the existing default), playback copies
remain cached. When disabled, a song can still download before playback, but
its temporary copy is discarded after playback stops or moves to another song.
Paused playback keeps its copy. Interrupted-session temporary copies are
cleaned up on the next connection attempt. Explicit offline downloads are
protected, and downloading a temporary copy explicitly promotes it to an
offline download. The toggle applies only to Selective mode.

The Navidrome trash icon uses the normal toolbar colour. The main Navidrome settings page omits status text. Synchronization activity
appears as a rotating sync glyph in the status bar; errors use popups.


While an album or playlist has pending offline downloads, its download control
shows a rotating circle instead of the download/trash glyph. The status-bar sync
glyph covers catalog sync, playlist sync, playback downloads and offline jobs.
Background reachability checks do not count as synchronization. Albums with no
available tracks are dimmed when their server is unreachable; albums with a
downloaded track and local-only albums remain available.

The main Sync button becomes **Cancel sync** during catalog/playlist sync or
song downloads, keeping its position instead of inserting extra actions.
Cancel stops active server requests/offline transfers while retaining completed
downloads and saved selections. Album and playlist progress uses the same
rotating sync glyph as the status bar, at the normal toolbar size and colour.

Now-playing metadata falls back to the locally stored server catalog before an
audio file or its sidecars exist. Track title, artist and album therefore remain
visible while downloading or when the server cannot be reached, including in
the pull-down player and restored queues. File tags and ReplayGain remain intact.


Theme updates track only live settings sliders; destroying/rebuilding a page
unregisters its sliders. Accent, theme and Dynamic tint changes therefore cannot
repaint deleted widgets, and all live sliders are covered without a fixed cap.

Album availability is resolved by a lower-priority worker and cached for visible
lists. Row binding never scans an album or checks its files. Network changes,
library revisions and download/removal events invalidate results. A new album
stays available until its first background result arrives. Unavailable rows dim
foreground text/artwork while retaining opaque cards for efficient scrolling.


Navidrome UI state is event driven: request completion, connection changes,
library changes, background album results, and offline-job state/progress post
coalesced notifications to the GUI thread. The 50 ms completion poll, one-second
library availability/icon poll, and 500 ms offline-page state poll are removed.
Repeated notifications share reserved mailbox slots, so a full ordinary GUI
queue cannot drop these state updates.

One one-second maintenance timer remains for scheduled refresh/retry deadlines,
playlist edit quiet periods, and temporary playback-file cleanup. Spinner
animation and the application's existing GUI dispatch fallback remain separate.


After a complete catalog has been fetched and committed successfully, downloads
for tracks absent from that server catalog are deleted, including their pin,
temporary, tag and identity sidecars. The offline manifest is cleared for those
paths. This applies to explicit downloads and ordinary playback-cache copies;
it is separate from removing an entry from a playlist or deselecting a download.

Cleanup records are committed with the catalog and survive restart. Failed or
cancelled catalog fetches do not create removal records. Each deletion verifies
the account identity and deterministic cache path, preserving local music and
other connections' files. Failed deletions remain queued for retry. A currently
playing or paused file is deferred until playback no longer needs it; the
existing maintenance worker then finishes cleanup.


Unchanged offline downloads take a fast path: validated pinned copies with
unchanged server size stamps skip fetching, pin rewrites and per-track manifest
updates. Disk usage is reconciled once per pass rather than summed for every
track. Fixed 100 ms inter-track pauses are removed; foreground requests still
interrupt/yield through the existing download handoff. Sync logs report catalog
fetch/artwork/publish/cleanup, playlist time, and cached versus fetched/promoted
offline work. An unchanged catalog still requires server enumeration; timings
help distinguish that network cost from local cache processing.


Startup atomically restores remote metadata and reconciles album groups against
all indexed tracks. Missing valid groups are recreated, and orphaned/NULL-key
groups from older databases are removed. This prevents phantom “(no name)”
albums after restart without requiring a network sync, while retaining local
albums and separate same-titled server albums.


Server display text uses the same boundary whitespace/control cleanup as local
tags; song IDs and album IDs remain untouched. Startup normalizes persisted
remote album names and restores both album name and server album key on existing
media rows. This repairs albums split by trailing-space differences between
server metadata and downloaded-file tags, and removes the empty legacy groups.


## Download and connection removal

Remove download / Remove all downloads delete owned audio, identity/tag files,
pin/temporary markers, interrupted transfer files and saved lyric sidecars.
Ownership is checked against the account identity and deterministic cache path;
catalog metadata can prove ownership for an interrupted transfer with no
identity file yet. Unreferenced artwork for the account is pruned. Artwork still
used by its synced catalog remains, so albums continue to show covers even
when their audio is not downloaded.

Remove connection now removes the account's cached audio and artwork, remote
catalog entries and empty album/artist groups, offline settings/rules/manifests,
server-linked playlist tables and their backups, and remembered/history/favorite
references to its tracks. References to those tracks are removed from local
playlists, while the local-only playlists themselves and their local tracks
remain. The active track is unloaded and owned queue entries removed before
background cleanup. Other accounts and their owned files are preserved.

The confirmation explains this scope. Cleanup is local; nothing is deleted on
the server. File cleanup runs before the transactional library purge and saved
login removal, so a file failure leaves the connection available for retry.
Legacy generated image/waveform keys do not contain account ownership:
connection removal resets those disposable music caches, preserving book cover
caches and all source music. Images/waveforms for retained music rebuild on use.
Deleted image rows are vacuumed to release their stored pixels and disk space.

Regression coverage uses isolated simulator data for single/all download
removal, temporary/interrupted sidecars, shared versus orphan artwork, orphan
owned downloads, server playlist backups, local/other-account preservation,
failed cleanup/retry, and reopening the library after connection removal.


## Connection settings

Server address, Username and Password remain visible above the general sync
controls, including after connecting. Passwords are masked. Finishing a changed
field automatically attempts login once all three fields are nonempty; accepting
an unchanged field or leaving fields incomplete does not send a request.
There is no separate Connect button. If offline work is running, the connection
attempt waits for it to finish.

Automatic sync, Offline / cache settings and Sync remain visible when no login
has succeeded, but disabled. They enable for successfully established/saved
matching credentials, even while the server is temporarily offline. Edited
credentials that have not succeeded do not expose controls for the old account.
Refresh interval remains conditional on Automatic sync being enabled.


Only while charging now belongs to the general Automatic sync controls and is
shown only when Automatic sync is on. It gates the start of scheduled catalog,
artwork, playlist and offline refreshes. Explicit Sync and Download actions,
playback downloads and automatic playback caching ignore this preference.
It does not cancel a sync already running when the charger is disconnected.
Existing saved charging preferences are retained in their new location.


New accounts default to a 4 GiB offline storage budget; 2 GiB is also available
on the storage slider. Existing configured budgets are retained. Remove all
downloads uses the confirmation label Remove all, resets Offline mode to
Selective immediately in the visible settings page, and preserves the automatic
playback caching preference. It does not turn that preference off.
