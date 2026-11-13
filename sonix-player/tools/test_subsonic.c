// Standalone contract test; links the real JSON and MD5 implementations.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/system/streaming/subsonic.h"
#include "src/system/net/http.h"
#include "src/system/core/md5.h"
#include "src/system/core/lang.h"

// This transport-only test checks message keys; the GUI test loads real catalogs.
const char *tr(const char *key) { return key; }

static const char *reply = "{\"subsonic-response\":{\"status\":\"ok\"}}";
static int status = 200, requests;
static bool network_ok = true;
static char previous_salt[33];

char *http_url_encode(const char *in, char *out, size_t size) {
	char *start = out;
	while (*in && size > 3) {
		unsigned char ch = (unsigned char)*in++;
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9')) { *out++ = ch; --size; }
		else { snprintf(out, 4, "%%%02X", ch); out += 3; size -= 3; }
	}
	*out = 0;
	return start;
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *len,
				  size_t limit, int timeout, int *code) {
	(void)len; (void)limit; (void)timeout;
	++requests;
	assert(!strcmp(req->method, "POST"));
	assert(!strchr(url, '?'));
	assert(strstr(url, "https://music.example/navidrome/rest/"));
	assert(strstr(req->body, "u=user%2Bname"));
	assert(!strstr(req->body, "secret"));
	const char *salt_start = strstr(req->body, "&s=");
	assert(salt_start);
	char salt[33], secret[64], token[33];
	memcpy(salt, salt_start + 3, 32); salt[32] = 0;
	assert(strcmp(salt, previous_salt));
	strcpy(previous_salt, salt);
	snprintf(secret, sizeof(secret), "secret%s", salt);
	md5_hex(secret, strlen(secret), token);
	assert(strstr(req->body, token));
	if (strstr(url, "search3")) {
		assert(strstr(req->body, "query=AC%2FDC%20%26%20live"));
		assert(strstr(req->body, "songOffset=10&songCount=2"));
	}
	if (!network_ok) return false;
	*code = status;
	*out = strdup(reply);
	return true;
}

int main(void) {
	subsonic_client_t c;
	assert(!subsonic_client_init(&c, "ftp://music.example", "u", "p"));
	assert(!subsonic_client_init(&c, "https://u:p@music.example", "u", "p"));
	assert(!subsonic_client_init(&c, "https://music.example?bad=1", "u", "p"));
	assert(subsonic_client_init(&c, "https://music.example/navidrome///", "user+name", "secret"));
	assert(subsonic_ping(&c));
	assert(subsonic_ping(&c));
	reply = "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":40,\"message\":\"secret\"}}}";
	assert(!subsonic_ping(&c));
	assert(!strcmp(subsonic_last_error(&c), tr("navidrome_the_server_rejected_the_username_or_password")));
	assert(!strstr(subsonic_last_error(&c), "secret"));
	reply = "<html>proxy error</html>";
	assert(!subsonic_ping(&c));
	reply = "{}";
	assert(!subsonic_ping(&c));
	reply = "{\"subsonic-response\":{\"status\":\"ok\"}}";
	status = 503;
	assert(!subsonic_ping(&c));
	status = 200;
	network_ok = false;
	assert(!subsonic_ping(&c));
	network_ok = true;
	subsonic_song_t songs[2];
	reply = "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"song\":[{\"id\":\"opaque-123\",\"title\":\"A \\u266b\",\"duration\":123}]}}}";
	assert(subsonic_search_songs(&c, "AC/DC & live", 10, songs, 2) == 1);
	assert(!strcmp(songs[0].id, "opaque-123"));
	assert(!strcmp(songs[0].title, "A \xe2\x99\xab"));
	assert(songs[0].duration == 123);
	reply = "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{}}}";
	assert(subsonic_search_songs(&c, "AC/DC & live", 10, songs, 2) == 0);
	reply = "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"song\":[{\"title\":\"missing ID\"}]}}}";
	assert(subsonic_search_songs(&c, "AC/DC & live", 10, songs, 2) == -1);
	int before = requests;
	assert(subsonic_search_songs(&c, "query", -1, songs, 2) == -1);
	assert(requests == before);
	subsonic_client_clear(&c);
	for (size_t i = 0; i < sizeof(c); ++i) assert(((unsigned char *)&c)[i] == 0);
	puts("Subsonic contract tests passed");
}
