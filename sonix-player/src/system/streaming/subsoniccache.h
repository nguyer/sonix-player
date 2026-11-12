#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H
#include "subsonic.h"

typedef struct {
	char magic[8];
	char server[512];
	char username[128];
	char song_id[128];
} subsonic_identity_t;

// Full-file cache, 1 GiB total / 512 MiB per track. Worker-only, serialized.
bool subsoniccache_fetch(subsonic_client_t *client, const subsonic_song_t *song,
						 const char *root, char *path, size_t size);
// Persistent association for later scrobbling; contains no password or token.
bool subsoniccache_identity(const char *path, subsonic_identity_t *identity);
void subsoniccache_source(const subsonic_client_t *client, char out[33]);
// Predicts the playback path without creating files or downloading audio.
bool subsoniccache_path(const subsonic_client_t *client, const subsonic_song_t *song,
	const char *root, char *path, size_t size);
#endif
