// Real HTTP transport exercised against the loopback fixture in the Python test.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/net/http.h"

// Only translations and the TLS CA setting are stubbed; transport is real.
const char *tr(const char *text) { return text; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section; (void)key; return fallback;
}

static subsonic_client_t *cancel_client;
static const char *cancel_path;
static void *download_until_cancelled(void *unused) {
	(void)unused;
	assert(!subsonic_download(cancel_client, "slow", cancel_path));
	return NULL;
}

int main(int argc, char **argv) {
	assert(argc == 3);
	// The existing GET stream path still works after adding POST streaming.
	char stream_url[640];
	snprintf(stream_url, sizeof(stream_url), "%s/test-audio", argv[1]);
	http_stream_t stream;
	assert(http_stream_open(&stream, stream_url, 5));
	char signature[4];
	assert(http_stream_read(&stream, signature, sizeof(signature)) == sizeof(signature));
	assert(!memcmp(signature, "RIFF", 4));
	http_stream_close(&stream);
	subsonic_client_t c;
	assert(subsonic_client_init(&c, argv[1], "test+user", "test-password"));
	assert(subsonic_ping(&c));
	char *bad_cover = NULL;
	size_t bad_cover_size = 0;
	assert(!subsonic_cover(&c, "invalid-cover", &bad_cover, &bad_cover_size));
	assert(!bad_cover && !bad_cover_size);
	subsonic_song_t songs[2];
	assert(subsonic_search_songs(&c, "AC/DC & live", 10, songs, 2) == 1);
	assert(!strcmp(songs[0].id, "server-song-1"));
	assert(songs[0].duration == 185);
	assert(!strcmp(songs[0].suffix, "wav"));
	char path[512], again[512];
	assert(!subsoniccache_fetch(&c, &songs[0], NULL, path, sizeof(path)));
	assert(strstr(subsonic_last_error(&c), "storage is unavailable"));
	assert(subsoniccache_fetch(&c, &songs[0], argv[2], path, sizeof(path)));
	FILE *audio = fopen(path, "rb");
	assert(audio);
	char header[12];
	assert(fread(header, 1, sizeof(header), audio) == sizeof(header));
	assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4));
	fclose(audio);
	subsonic_identity_t identity;
	assert(subsoniccache_identity(path, &identity));
	assert(!strcmp(identity.server, argv[1]));
	assert(!strcmp(identity.username, "test+user"));
	assert(!strcmp(identity.song_id, "server-song-1"));
	assert(subsoniccache_fetch(&c, &songs[0], argv[2], again, sizeof(again)));
	assert(!strcmp(path, again)); // no second network call
	assert(!subsoniccache_identity("/tmp/local-song.wav", &identity));
	// Different account -> different cache key, even for the same server ID.
	subsonic_client_t other = c;
	strcpy(other.username, "another-user");
	assert(subsoniccache_fetch(&other, &songs[0], argv[2], again, sizeof(again)));
	assert(strcmp(path, again));
	subsonic_client_clear(&other);
	char partial[600];
	snprintf(partial, sizeof(partial), "%s/failure.wav", argv[2]);
	const char *bad_ids[] = {"truncated", "api-error", "redirect", "oversized"};
	for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); ++i) {
		assert(!subsonic_download(&c, bad_ids[i], partial));
		assert(access(partial, F_OK) != 0);
		assert(!subsonic_download_busy());
	}
	cancel_client = &c;
	cancel_path = partial;
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, download_until_cancelled, NULL));
	for (int i = 0; i < 5000 && !subsonic_download_busy(); ++i) usleep(1000);
	assert(subsonic_download_busy());
	usleep(100000);
	subsonic_download_cancel();
	pthread_join(worker, NULL);
	assert(!subsonic_download_busy());
	assert(access(partial, F_OK) != 0);
	subsonic_client_clear(&c);
	assert(subsonic_client_init(&c, argv[1], "test+user", "wrong-password"));
	assert(!subsonic_ping(&c));
	assert(strstr(subsonic_last_error(&c), "username or password"));
	subsonic_client_clear(&c);
	puts("Subsonic real-HTTP tests passed");
}
