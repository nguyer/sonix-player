#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H
#include "subsonic.h"
#include <stdint.h>

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
bool subsoniccache_fetch_offline(subsonic_client_t *client,const subsonic_song_t *song,
 const char *root,char *path,size_t size,int64_t allowance,int64_t reserve,bool force,const atomic_bool *cancel);
bool subsoniccache_mark_temporary(const char *path);
bool subsoniccache_is_temporary(const char *path);
bool subsoniccache_discard_temporary(const subsonic_client_t *client,const char *root,const char *path);
void subsoniccache_cleanup_temporary(const subsonic_client_t *client,const char *root,const char *keep);
bool subsoniccache_remove_owned(const subsonic_client_t *client,const char *root,const char *path,const char *id);
#endif
