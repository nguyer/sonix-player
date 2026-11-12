#ifndef SUBSONICPLAYLISTAPI_H
#define SUBSONICPLAYLISTAPI_H
#include "subsonic.h"
typedef struct { char id[128], name[256], owner[128]; } subsonic_playlist_t;
typedef struct {
	subsonic_playlist_t info;
	char (*ids)[128];
	int count;
} subsonic_playlist_snapshot_t;
int subsonic_playlists(subsonic_client_t *client, subsonic_playlist_t **out);
bool subsonic_playlist_get(subsonic_client_t *client, const char *id, subsonic_playlist_snapshot_t *out);
void subsonic_playlist_free(subsonic_playlist_snapshot_t *snapshot);
bool subsonic_playlist_put(subsonic_client_t *client, const char *id, const char *name,
	const char (*ids)[128], int count, char out_id[128]);
bool subsonic_playlist_delete(subsonic_client_t *client, const char *id);
#endif
