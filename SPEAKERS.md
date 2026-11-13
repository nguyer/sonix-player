# Wireless playback / Sonos

The wireless icon immediately left of the three-dot menu in **Now Playing**
opens the **Output** bottom sheet. **This device** selects local playback.
A playback icon marks the active local output or Sonos group, including while
paused.

Rooms refresh automatically each time the picker opens. With no known speaker,
the controller discovers the household; subsequent opens refresh its topology.
There are no discovery/refresh buttons or status text. Explicit command errors
use popups; an empty automatic discovery leaves local playback available.
**Connect by speaker IP** appears only in simulator builds. It loads all rooms
from one speaker without multicast and retains that address for the session.

Each existing group is one card, with a selectable group header and member
rooms underneath. Group members have an **X** to leave their group. Standalone
rooms have a **+** to join the active wireless group (disabled if local playback
is selected, or the room itself is the active output). Select a standalone room
first to start building a new group.

Every room in the active output group has its own volume slider, initialized
from that room's actual volume. Until the read completes, its slider is disabled.
Volume writes address the individual room, not the group. Pending drag requests
coalesce by room ID, retaining the last value without blocking the UI. Discovery
and volume replies update the UI through posted events; no UI timer is added.
Key/overlay volume input updates all known active-group sliders together on
the next GUI pass, before speaker acknowledgement. Per-room offsets and bounds
are preserved in this preview. Partial replies cannot overwrite predictions
while group changes are pending. After completion, a full group read reconciles
the displayed values; errors restore the prior display before refreshing.
Key/overlay volume feedback also updates the open sheet. Confirmed volume
measurements carry monotonic timestamps so a delayed picker reply cannot undo
a newer playback-volume report. A slider being dragged keeps its finger value.
After a join/leave acknowledgement, the worker briefly rechecks topology until
the requested membership is visible; this is bounded to the action, not a
permanent polling timer. The open group cards then update without reopening.

The Now Playing Output icon is hidden while Wi-Fi has no connection/address.
The simulator uses live host network interfaces when no Wi-Fi hardware is
exposed (for example WSL Ethernet). Visibility uses the existing player refresh.

Removing the room that anchors the active output follows a remaining group
member without publishing a fresh track. Bonded satellites/stereo internals are
not offered as independently groupable rooms.

Selecting a room hands the loaded track to its group coordinator, preserving
position and play/pause intent. Selecting This device stops the owned Sonos
stream, ends sharing, and restores local playback at the reported position.
A paused handoff loads without briefly playing first. Selecting a room without
a loaded track selects it for the next file played.

## Shared controls and queue

The audio facade routes file playback, play/pause, stop, seek, progress,
current-file queries and completion to the selected output. The ordinary queue
retains order, repeat, shuffle, next/previous and automatic advance. The status
bar and Now Playing consume the same playback status. Wireless progress advances\nbetween whole-second reports without small backward corrections; explicit or\nsubstantial backward seeks reset the position. Hardware keys, control
centre and the volume overlay adjust every member of the active group by the
same percentage-point delta, preserving their independent levels. Each speaker
clamps its own 0–100 bounds; a speaker at a bound does not block the others.
The device volume indicator uses the selected room as its reference; moving
the device volume slider computes a group-wide delta from that reference.
Output's per-room sliders continue to set only that room. Local saved volume
profiles are unaffected. Rapid key presses accumulate deltas, consumed exactly
once even when other playback requests arrive during HTTP. Confirmed group
adjustments refresh all room sliders in the open Output sheet.

All playback HTTP work runs on a background output worker. Commands wake it
immediately; revisions discard stale replies after a skip or output switch.
UI updates use gui_post, including discovery/setup completions; there is no
speaker UI polling timer.

While wireless output is selected, the worker reads transport, position and
room volume about once per second (plus request time) to reflect changes from
other controllers. It sleeps without polling when local output is selected.
This is active-output feedback, not background discovery; UPnP subscriptions
are not implemented. Natural completion posts the queue-completion notification
used by local audio. Explicit stops, network failures and URI replacement by
another controller do not advance the queue. Near-end stopped feedback is used
to recognize completion.

Network errors stop the displayed playing state, surface a message, and leave
wireless output selected for retry or switching to This device. New tracks
supersede in-flight handoffs/download waits. Music must finish downloading
before Sonos can fetch a stable file. This wait is off the GUI thread; the
prior track stops if a skip must wait. Volume can still be changed while waiting.

## Simulator networking

Manual testing can use the simulator with real Sonos speakers on the same LAN.
**Connect by speaker IP** queries one speaker on port 1400 and loads the
household's rooms/groups without multicast. Enter IPv4 without a scheme or
port. The address is retained for the session.

Playback requires the reverse connection: Sonos fetches the file from the
simulator. The app chooses its local address from the route to the coordinator
without assuming the hardware wlan0 interface.

WSL NAT generally prevents this reverse connection. Enable mirrored networking
in the Windows user's .wslconfig on supported Windows/WSL versions:

    [wsl2]
    networkingMode=mirrored

Restart WSL after saving work. Windows/Hyper-V firewall must also allow the
audio server TCP port. This port is assigned dynamically and can change after
a simulator restart. Inspect its listener with ss -ltnp, then add a specific
inbound port rule in elevated PowerShell. The app does not change WSL or
firewall settings.

See [Microsoft's WSL networking instructions](https://learn.microsoft.com/en-us/windows/wsl/networking#mirrored-mode-networking).

## File sharing and limits

One completed file is served through an unpredictable per-publication HTTP URL,
with HEAD, single byte ranges, and at most four concurrent requests. URL
rotation or switching to local output revokes the previous file. Request paths
are never mapped to filesystem paths. Server credentials and filesystem paths
are not sent to speakers.

MP3, FLAC, WAV, AAC, M4A and AIFF extensions are accepted; the speaker must
support the actual codec, sample rate and bit depth. No transcoding is provided.
Live radio, receiver and game/metronome audio use local output; starting an
external PCM source deselects Sonos. Local DSP, gapless PCM, speed processing
and fades are not applied on Sonos.

The player stays on and connected while wireless output is selected, preventing
standby, radio parking and idle shutdown. The screen can turn off. Switch to
This device to release that hold. Output selection is session-only. Physical
player behavior/firmware builds remain unverified; a physical player is not
needed for simulator/Sonos testing.

## Validation

Run from the repository root in WSL:

    make -C sonix-player host -j2
    python3 sonix-player/tools/test_speakers.py
    python3 sonix-player/tools/test_speakers_ui.py
    python3 sonix-player/tools/test_subsonic_library.py

Tests exercise real SOAP construction/parsing, coordinator routing, grouping,
manual addressing, seek and failures; the real sharing server on loopback;
paused loading, overlapping commands, download cancellation, completion,
disconnects and external URI replacement. Full-application tests exercise the
actual audio/device/volume facades and queue next/previous/automatic advance
against a mock speaker. Offscreen UI tests verify non-overlapping touch targets,
sheet lifecycle/parenting and theme changes, rendering snapshots to
/tmp/sonix-wireless-player.ppm and /tmp/sonix-wireless-sheet.ppm.

Simulator checks with real Sonos:
1. Open the picker from Now Playing; discover or enter a speaker IP.
2. Select a room while playing and paused; check position and local silence.
   Change volume using slider, control centre and keys.
3. Test pause, resume, seek, next, previous and automatic next track with the
   screen off.
4. Pause or change volume in the Sonos app; check the simulator follows.
5. Switch groups, regroup, rapidly skip, disconnect a speaker, and return to
   This device. Confirm errors are visible and no unrelated stream is stopped
   after another controller replaces this one.
