#include "subsonic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

static pthread_mutex_t transfer_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;
static bool transfer_busy, transfer_cancelled;

bool subsonic_download_busy(void) {
	pthread_mutex_lock(&transfer_lock);
	bool busy = transfer_busy;
	pthread_mutex_unlock(&transfer_lock);
	return busy;
}

void subsonic_download_cancel(void) {
	pthread_mutex_lock(&transfer_lock);
	if (transfer_busy) transfer_cancelled = true;
	if (active_stream) http_stream_wake(active_stream);
	pthread_mutex_unlock(&transfer_lock);
}

static void transfer_end(http_stream_t *stream) {
	pthread_mutex_lock(&transfer_lock);
	active_stream = NULL;
	transfer_busy = false;
	pthread_mutex_unlock(&transfer_lock);
	http_stream_close(stream);
}

static bool fail(subsonic_client_t *c, const char *message) {
	snprintf(c->error, sizeof(c->error), "%s", message);
	return false;
}

void subsonic_client_clear(subsonic_client_t *c) {
	// Volatile writes prevent dead-store elimination of credential cleanup.
	volatile unsigned char *p = (volatile unsigned char *)c;
	for (size_t i = 0; i < sizeof(*c); ++i) p[i] = 0;
}

const char *subsonic_last_error(const subsonic_client_t *c) { return c->error; }

bool subsonic_client_init(subsonic_client_t *c, const char *url, const char *user, const char *password) {
	subsonic_client_clear(c);
	if (!url || !user || !password || !*user || !*password)
		return fail(c, "Server, username and password are required.");
	const char *authority;
	if (!strncmp(url, "https://", 8)) authority = url + 8;
	else if (!strncmp(url, "http://", 7)) authority = url + 7;
	else return fail(c, "Use an http:// or https:// server address.");
	if (!*authority || *authority == '/' || strpbrk(url, "?#@\r\n\t "))
		return fail(c, "Use the server root URL without credentials, query or fragment.");
	if (strlen(url) >= sizeof(c->base_url) || strlen(user) >= sizeof(c->username) ||
		strlen(password) >= sizeof(c->password)) return fail(c, "Account details are too long.");
	strcpy(c->base_url, url);
	size_t n = strlen(c->base_url);
	while (n && c->base_url[n - 1] == '/') c->base_url[--n] = 0;
	strcpy(c->username, user);
	strcpy(c->password, password);
	return true;
}

// Authentication is sent as a POST form: http.c logs failing URL paths.
// A fresh random salt is required for every request (Subsonic API >= 1.13).
static bool auth_form(subsonic_client_t *c, const char *params, char *form, size_t size) {
	c->error[0] = 0;
	if (!c->base_url[0] || !c->username[0] || !c->password[0])
		return fail(c, "Configure a Subsonic account first.");
	unsigned char random[16];
	FILE *rng = fopen("/dev/urandom", "rb");
	if (!rng) return fail(c, "Cannot generate authentication salt.");
	size_t got = fread(random, 1, sizeof(random), rng);
	fclose(rng);
	if (got != sizeof(random)) return fail(c, "Cannot generate authentication salt.");
	char salt[33], token[MD5_HEX_LEN], secret[sizeof(c->password) + 33];
	for (size_t i = 0; i < sizeof(random); ++i) snprintf(salt + 2 * i, 3, "%02x", random[i]);
	snprintf(secret, sizeof(secret), "%s%s", c->password, salt);
	md5_hex(secret, strlen(secret), token);
	volatile char *wipe = secret;
	for (size_t i = 0; i < sizeof(secret); ++i) wipe[i] = 0;
	char user[sizeof(c->username) * 3];
	http_url_encode(c->username, user, sizeof(user));
	int n = snprintf(form, size, "u=%s&t=%s&s=%s&v=1.16.1&c=SonixPlayer&f=json%s",
					 user, token, salt, params);
	if (n < 0 || (size_t)n >= size) return fail(c, "Request is too long.");
	return true;
}

static bool call(subsonic_client_t *c, const char *method, const char *params,
				 char **body, json_doc_t *doc, int *response) {
	char form[4096], url[640];
	if (!auth_form(c, params, form, sizeof(form))) return false;
	snprintf(url, sizeof(url), "%s/rest/%s.view", c->base_url, method);
	http_req_t req = {.method = "POST", .body = form, .want_error_body = true};
	int status = 0;
	if (!http_request(url, &req, body, NULL, 2 * 1024 * 1024, 15, &status))
		return fail(c, "Cannot reach the Subsonic server. Check the address and connection.");
	if (!json_parse(*body, doc)) {
		free(*body);
		return fail(c, "Invalid JSON response from the Subsonic server.");
	}
	*response = json_get(doc, json_root(doc), "subsonic-response");
	char state[16];
	json_obj_str(doc, *response, "status", state, sizeof(state));
	if (status == 200 && !strcmp(state, "ok")) return true;
	// Do not echo arbitrary server messages: a proxy may reflect credentials.
	long code = json_obj_long(doc, json_get(doc, *response, "error"), "code", 0);
	if (code == 40 || status == 401) fail(c, "The server rejected the username or password.");
	else if (code == 50 || status == 403) fail(c, "This account cannot access the requested resource.");
	else snprintf(c->error, sizeof(c->error), "Subsonic request failed (HTTP %d, API %ld).", status, code);
	json_free(doc);
	free(*body);
	return false;
}

bool subsonic_ping(subsonic_client_t *c) {
	char *body;
	json_doc_t doc;
	int response;
	if (!call(c, "ping", "", &body, &doc, &response)) return false;
	json_free(&doc);
	free(body);
	return true;
}

int subsonic_search_songs(subsonic_client_t *c, const char *query, int offset,
						 subsonic_song_t *songs, int capacity) {
	if (!query || strlen(query) > 512 || offset < 0 || !songs || capacity < 1 || capacity > 500) {
		fail(c, "Invalid song search parameters.");
		return -1;
	}
	char encoded[1537], params[1664];
	http_url_encode(query, encoded, sizeof(encoded));
	snprintf(params, sizeof(params), "&query=%s&songOffset=%d&songCount=%d&artistCount=0&albumCount=0",
			 encoded, offset, capacity);
	char *body;
	json_doc_t doc;
	int response;
	if (!call(c, "search3", params, &body, &doc, &response)) return -1;
	int result = json_get(&doc, response, "searchResult3");
	int array = json_get(&doc, result, "song");
	int count = json_len(&doc, array);
	if (result < 0 || doc.toks[result].type != JSON_OBJECT ||
		(array >= 0 && doc.toks[array].type != JSON_ARRAY) || count > capacity) {
		fail(c, "Invalid song search response.");
		count = -1;
	}
	for (int i = 0; i < count; ++i) {
		int item = json_at(&doc, array, i);
		subsonic_song_t *s = &songs[i];
		memset(s, 0, sizeof(*s));
		int id = json_get(&doc, item, "id");
		// Truncated IDs could associate playback events with the wrong song.
		if (id < 0 || doc.toks[id].type != JSON_STRING ||
			doc.toks[id].end - doc.toks[id].start >= (int)sizeof(s->id) ||
			!json_str(&doc, id, s->id, sizeof(s->id)) || !s->id[0]) {
			fail(c, "Server returned an invalid or oversized song ID.");
			count = -1;
			break;
		}
		json_obj_str(&doc, item, "albumId", s->album_id, sizeof(s->album_id));
		json_obj_str(&doc, item, "title", s->title, sizeof(s->title));
		json_obj_str(&doc, item, "artist", s->artist, sizeof(s->artist));
		json_obj_str(&doc, item, "album", s->album, sizeof(s->album));
		json_obj_str(&doc, item, "coverArt", s->cover_id, sizeof(s->cover_id));
		json_obj_str(&doc, item, "suffix", s->suffix, sizeof(s->suffix));
		long duration = json_obj_long(&doc, item, "duration", 0);
		long track = json_obj_long(&doc, item, "track", 0);
		s->duration = duration >= 0 && duration <= 2147483647L ? (int)duration : 0;
		s->track = track >= 0 && track <= 2147483647L ? (int)track : 0;
	}
	json_free(&doc);
	free(body);
	return count;
}

bool subsonic_download(subsonic_client_t *c, const char *id, const char *path) {
	if (!id || !*id || strlen(id) >= 128 || !path || !*path)
		return fail(c, "Invalid song download parameters.");
	char encoded[384], params[400], form[4096], url[640];
	http_url_encode(id, encoded, sizeof(encoded));
	snprintf(params, sizeof(params), "&id=%s", encoded);
	if (!auth_form(c, params, form, sizeof(form))) return false;
	snprintf(url, sizeof(url), "%s/rest/download.view", c->base_url);
	pthread_mutex_lock(&transfer_lock);
	if (transfer_busy) {
		pthread_mutex_unlock(&transfer_lock);
		return fail(c, "Another song download is still running.");
	}
	transfer_busy = true;
	transfer_cancelled = false;
	pthread_mutex_unlock(&transfer_lock);
	http_stream_t stream;
	if (!http_stream_post_open(&stream, url, form, 15)) {
		transfer_end(&stream);
		return fail(c, "Cannot download the song. Check access and use the final server URL without redirects.");
	}
	pthread_mutex_lock(&transfer_lock);
	active_stream = &stream;
	bool cancelled = transfer_cancelled;
	pthread_mutex_unlock(&transfer_lock);
	// API failures can be HTTP 200. Never hand a JSON/XML/HTML error to a decoder.
	bool audio = !strncasecmp(stream.content_type, "audio/", 6) ||
		!strncasecmp(stream.content_type, "application/ogg", 15) ||
		!strncasecmp(stream.content_type, "application/octet-stream", 24);
	const long max_bytes = 512L * 1024L * 1024L;
	if (cancelled || !audio || stream.content_length < 0 || stream.content_length > max_bytes) {
		transfer_end(&stream);
		return fail(c, "Server returned an error or the song exceeds the 512 MiB limit.");
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) { transfer_end(&stream); return fail(c, "Cannot create the song cache file."); }
	FILE *file = fdopen(fd, "wb");
	if (!file) { close(fd); unlink(path); transfer_end(&stream); return fail(c, "Cannot open the song cache file."); }
	char buffer[16384];
	long total = 0;
	bool ok = true;
	for (;;) {
		if (stream.content_length && total == stream.content_length) break;
		pthread_mutex_lock(&transfer_lock);
		cancelled = transfer_cancelled;
		pthread_mutex_unlock(&transfer_lock);
		if (cancelled) { ok = false; break; }
		int n = http_stream_read(&stream, buffer, sizeof(buffer));
		if (n == 0) break;
		if (n < 0 || total > max_bytes - n) { ok = false; break; }
		if (stream.content_length && n > stream.content_length - total) { ok = false; break; }
		if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) { ok = false; break; }
		total += n;
	}
	if (!total || (stream.content_length && total != stream.content_length)) ok = false;
	if (fclose(file) != 0) ok = false;
	pthread_mutex_lock(&transfer_lock);
	if (transfer_cancelled) ok = false;
	pthread_mutex_unlock(&transfer_lock);
	transfer_end(&stream);
	if (!ok) { unlink(path); return fail(c, "Song download was interrupted or storage is full. Try again."); }
	return true;
}
