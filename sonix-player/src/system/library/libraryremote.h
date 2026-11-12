#ifndef LIBRARYREMOTE_H
#define LIBRARYREMOTE_H
#include "src/system/streaming/subsonic.h"

typedef struct {
	char path[512], source[33], server[512], username[128];
	subsonic_song_t song;
} library_remote_track_t;

// Staging does not change the visible library. Publish replaces one account's
// catalog atomically, preserving local tracks and catalogs from other sources.
bool library_remote_begin(const char *source);
bool library_remote_stage(const library_remote_track_t *rows, int count);
bool library_remote_publish(const char *source);
bool library_remote_get(const char *path, library_remote_track_t *row);
int library_remote_count(const char *source);
// Unique cover IDs for a staged catalog, with album-ID fallback.
int library_remote_covers(const char *source, int offset, char ids[][128], int capacity);
#endif
