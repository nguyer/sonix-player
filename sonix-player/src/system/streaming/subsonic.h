#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// One account per context. Initialize before use; do not share a context across
// workers without external locking. Network calls block: never call on the UI
// thread. Credentials live only in memory; no configuration is written here.
typedef struct {
	char base_url[512]; // server root, including an optional reverse-proxy path
	char username[128];
	char password[256];
	char error[256];
} subsonic_client_t;

typedef struct {
	char id[128]; // opaque server ID, never a number or a local-file identity
	char album_id[128];
	char title[256];
	char artist[256];
	char album[256];
	char cover_id[128];
	int duration;
	int track;
	char suffix[16]; // original audio container, from the server
} subsonic_song_t;

// Rejects unsupported URLs, embedded credentials, and oversized input rather
// than silently connecting with truncated account information.
bool subsonic_client_init(subsonic_client_t *client, const char *base_url,
						  const char *username, const char *password);
void subsonic_client_clear(subsonic_client_t *client);
const char *subsonic_last_error(const subsonic_client_t *client);
bool subsonic_ping(subsonic_client_t *client);
// Returns songs copied, or -1. Caller controls pagination; maximum page is 500.
int subsonic_search_songs(subsonic_client_t *client, const char *query, int offset,
						 subsonic_song_t *songs, int capacity);

// Worker-only. Streams the original audio to a new file using bounded memory.
// Never overwrites an existing file; removes partial output on any failure.
bool subsonic_download(subsonic_client_t *client, const char *song_id, const char *path);
// Thread-safe cancellation and power-management status for the active transfer.
void subsonic_download_cancel(void);
bool subsonic_download_busy(void);

#endif
