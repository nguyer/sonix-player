#ifndef LIBRARYREMOTE_H
#define LIBRARYREMOTE_H
#include "src/system/streaming/subsonic.h"
#include "metadata.h"

typedef struct {
	char path[512], source[33], server[512], username[128];
	subsonic_song_t song;
} library_remote_track_t;

// Staging does not change the visible library. Publish replaces one account's
// catalog atomically, preserving local tracks and catalogs from other sources.
bool library_remote_begin(const char *source);
bool library_remote_stage(const library_remote_track_t *rows, int count);
bool library_remote_publish(const char *source);
typedef struct {char path[512],id[128];} library_remote_removed_t;
int library_remote_removed(const char *source,int offset,library_remote_removed_t *rows,int capacity);
bool library_remote_removed_done(const char *source,const char *path);
bool library_remote_removed_pending(const char *source,const char *keep);
// Fill missing display tags from the persistent catalog, even before audio exists.
bool library_remote_metadata(const char *path,song_metadata_t *metadata);
bool library_remote_get(const char *path, library_remote_track_t *row);
int library_remote_count(const char *source);
// Unique cover IDs for a staged catalog, with album-ID fallback.
int library_remote_covers(const char *source, int offset, char ids[][128], int capacity);

typedef struct {
	char source[33], id[128], local_name[201], base_local[201], remote_name[256], hash[33];
} library_playlist_link_t;
int library_playlist_links(const char *source, library_playlist_link_t **out);
bool library_playlist_link(const char *local_name, library_playlist_link_t *out);
bool library_playlist_link_record(const library_playlist_link_t *link, unsigned revision);
bool library_playlist_link_drop(const library_playlist_link_t *link, bool drop_local, unsigned revision);
// Complete ordered IDs; refuses local files or tracks from another account.
bool library_playlist_remote_ids(const char *source, const char *name, char (**ids)[128], int *count, unsigned *revision);
bool library_playlist_remote_replace(const library_playlist_link_t *link, const char *old_name,
	const char (*ids)[128], int count, unsigned revision);
bool library_playlist_auto_pending(const char *source, int quiet_seconds);
bool library_playlist_auto_link_pending(const char *source, const char *id);
#endif
