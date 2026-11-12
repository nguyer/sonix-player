#ifndef SUBSONICPLAYLISTS_H
#define SUBSONICPLAYLISTS_H
#include "subsonic.h"
#include <stdatomic.h>
// Full server refresh and explicit upload. Local-only playlists never auto-sync.
int subsonicplaylists_sync(subsonic_client_t *client, const atomic_bool *cancel);
bool subsonicplaylists_upload(subsonic_client_t *client, const char *local_name, const atomic_bool *cancel);
bool subsonicplaylists_busy(void);
// Background pass: only already-linked playlists with persisted local edits.
int subsonicplaylists_sync_pending(subsonic_client_t *client, const atomic_bool *cancel);
#endif
