#include "subsonic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

static pthread_mutex_t availability_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {char server[512],username[128];bool available;} servers[4];
static unsigned server_slot,availability_revision;
unsigned subsonic_availability_revision(void){pthread_mutex_lock(&availability_lock);unsigned value=availability_revision;pthread_mutex_unlock(&availability_lock);return value;}
static void (*availability_notify)(void);
void subsonic_set_availability_notify(void (*notify)(void)){pthread_mutex_lock(&availability_lock);availability_notify=notify;pthread_mutex_unlock(&availability_lock);}
static void notify_availability(void){pthread_mutex_lock(&availability_lock);void (*notify)(void)=availability_notify;pthread_mutex_unlock(&availability_lock);if(notify)notify();}
static bool network_available=true;
void subsonic_forget_server(const char *server,const char *username) {
 pthread_mutex_lock(&availability_lock);
 for(int i=0;i<4;++i)if(!strcmp(servers[i].server,server) && !strcmp(servers[i].username,username))servers[i].available=false;
 ++availability_revision;
 pthread_mutex_unlock(&availability_lock);notify_availability();
}
bool subsonic_network_available(void) {
 pthread_mutex_lock(&availability_lock);bool value=network_available;
 pthread_mutex_unlock(&availability_lock);return value;
}
void subsonic_set_network_available(bool available) {
 pthread_mutex_lock(&availability_lock);
 if(network_available!=available){for(int i=0;i<4;++i)servers[i].available=false;++availability_revision;}
 network_available=available;
 pthread_mutex_unlock(&availability_lock);notify_availability();
}
static void note_server(const subsonic_client_t *c,bool available) {
 pthread_mutex_lock(&availability_lock);unsigned previous=availability_revision;int slot=-1;
 for(int i=0;i<4;++i)if(!strcmp(servers[i].server,c->base_url) && !strcmp(servers[i].username,c->username))slot=i;
 if(slot<0){slot=(int)(server_slot++%4);++availability_revision;}
 if(servers[slot].available!=(available && network_available))++availability_revision;
 snprintf(servers[slot].server,sizeof(servers[slot].server),"%s",c->base_url);
 snprintf(servers[slot].username,sizeof(servers[slot].username),"%s",c->username);
 servers[slot].available=available && network_available;bool changed=previous!=availability_revision;pthread_mutex_unlock(&availability_lock);if(changed)notify_availability();
}
bool subsonic_server_available(const char *server,const char *username){
 pthread_mutex_lock(&availability_lock);bool available=false;
 for(int i=0;i<4;++i)if(!strcmp(servers[i].server,server) && !strcmp(servers[i].username,username))available=servers[i].available;
 pthread_mutex_unlock(&availability_lock);return available;
}

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

bool subsonic_api_call(subsonic_client_t *c, const char *method, const char *params,
				 char **body, json_doc_t *doc, int *response) {
	char url[640];
	size_t form_size = strlen(params) + 1400;
	if (form_size > 2 * 1024 * 1024) return fail(c, "Request is too long.");
	char *form = malloc(form_size);
	if (!form) return fail(c, "Not enough memory for the request.");
	if (!auth_form(c, params, form, form_size)) { free(form); return false; }
	snprintf(url, sizeof(url), "%s/rest/%s.view", c->base_url, method);
	http_req_t req = {.method = "POST", .body = form, .want_error_body = true};
	int status = 0;
	bool requested = http_request(url, &req, body, NULL, 2 * 1024 * 1024, 15, &status);
	free(form);
	if (!requested){note_server(c,false);return fail(c,"Cannot reach the Subsonic server. Check the address and connection.");}
	note_server(c,status<500 && status!=401 && status!=403);
	if (!json_parse(*body, doc)) {
		note_server(c,false);
		free(*body);
		return fail(c, "Invalid JSON response from the Subsonic server.");
	}
	*response = json_get(doc, json_root(doc), "subsonic-response");
	char state[16];
	json_obj_str(doc, *response, "status", state, sizeof(state));
	if (status == 200 && !strcmp(state, "ok")) return true;
	// Do not echo arbitrary server messages: a proxy may reflect credentials.
	long code = json_obj_long(doc, json_get(doc, *response, "error"), "code", 0);
	if (code == 40 || status == 401) {note_server(c,false);fail(c, "The server rejected the username or password.");}
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
	if (!subsonic_api_call(c, "ping", "", &body, &doc, &response)) return false;
	json_free(&doc);
	free(body);
	return true;
}

bool subsonic_cover(subsonic_client_t *c, const char *id, char **data, size_t *size) {
	*data = NULL;
	*size = 0;
	if (!id || !*id || strlen(id) >= 128) return fail(c, "Invalid cover ID.");
	char encoded[384], params[420], form[4096], url[640];
	http_url_encode(id, encoded, sizeof(encoded));
	snprintf(params, sizeof(params), "&id=%s&size=512", encoded);
	if (!auth_form(c, params, form, sizeof(form))) return false;
	snprintf(url, sizeof(url), "%s/rest/getCoverArt.view", c->base_url);
	http_req_t req = {.method="POST", .body=form};
	int status = 0;
	const size_t limit = 2 * 1024 * 1024;
	if (!http_request(url, &req, data, size, limit, 15, &status)) return fail(c, "Cannot fetch album artwork.");
	bool jpeg = *size >= 4 && !memcmp(*data, "\xff\xd8", 2) && !memcmp(*data + *size - 2, "\xff\xd9", 2);
	bool png = *size >= 20 && !memcmp(*data, "\x89PNG\r\n\x1a\n", 8) &&
		!memcmp(*data + *size - 12, "\0\0\0\0IEND", 8);
	if (status != 200 || *size >= limit || (!jpeg && !png)) {
		free(*data); *data = NULL; *size = 0;
		return fail(c, "Server returned invalid or oversized album artwork.");
	}
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
	if (!subsonic_api_call(c, "search3", params, &body, &doc, &response)) return -1;
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
		json_obj_str(&doc, item, "albumArtist", s->album_artist, sizeof(s->album_artist));
		json_obj_str(&doc, item, "genre", s->genre, sizeof(s->genre));
		s->size_bytes=json_obj_llong(&doc,item,"size",0);
		json_obj_str(&doc,item,"created",s->created,sizeof(s->created));
		s->year = (int)json_obj_long(&doc, item, "year", 0);
		s->disc = (int)json_obj_long(&doc, item, "discNumber", 1);
		long duration = json_obj_long(&doc, item, "duration", 0);
		long track = json_obj_long(&doc, item, "track", 0);
		s->duration = duration >= 0 && duration <= 2147483647L ? (int)duration : 0;
		s->track = track >= 0 && track <= 2147483647L ? (int)track : 0;
        subsonic_song_normalize(s);
	}
	json_free(&doc);
	free(body);
	return count;
}

bool subsonic_download(subsonic_client_t *c,const char *id,const char *path) {
 return subsonic_download_limited(c,id,path,512LL*1024*1024,0,NULL);
}

bool subsonic_download_limited(subsonic_client_t *c,const char *id,const char *path,
 long long allowance,long long reserve,const atomic_bool *cancel) {
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
		note_server(c,false);
		transfer_end(&stream);
		return fail(c, "Cannot download the song. Check access and use the final server URL without redirects.");
	}
	note_server(c,true);
	pthread_mutex_lock(&transfer_lock);
	active_stream = &stream;
	bool cancelled = transfer_cancelled;
	pthread_mutex_unlock(&transfer_lock);
	// API failures can be HTTP 200. Never hand a JSON/XML/HTML error to a decoder.
	bool audio = !strncasecmp(stream.content_type, "audio/", 6) ||
		!strncasecmp(stream.content_type, "application/ogg", 15) ||
		!strncasecmp(stream.content_type, "application/octet-stream", 24);
 long long budget=allowance;
 if(cancel && atomic_load(cancel)){transfer_end(&stream);return fail(c,"Download interrupted.");}
 char parent[600];snprintf(parent,sizeof(parent),"%s",path);
 char *slash=strrchr(parent,'/');if(slash)*slash=0;else strcpy(parent,".");
 struct statvfs space;
 if(reserve>0){
  if(statvfs(parent,&space)!=0){transfer_end(&stream);return fail(c,"Cannot check free storage.");}
  long long available=(long long)space.f_bavail*(long long)space.f_frsize-reserve;
  if(available<budget)budget=available;
 }
 const long max_bytes=(long)(budget<512LL*1024*1024?budget:512LL*1024*1024);
 if(max_bytes<=0){transfer_end(&stream);return fail(c,"Offline storage limit or free-space reserve reached.");}
	if (cancelled || !audio || stream.content_length < 0 || stream.content_length > max_bytes) {
		transfer_end(&stream);
		return fail(c, "Song exceeds the download/storage limit, or the server returned an error.");
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
  if(cancel && atomic_load(cancel))cancelled=true;
  if (cancelled) { ok = false; break; }
  if(reserve>0 && total%(1024*1024)<(long)sizeof(buffer)){
   if(statvfs(parent,&space)!=0 || (long long)space.f_bavail*(long long)space.f_frsize<reserve+(long long)sizeof(buffer)){ok=false;break;}
  }
		int n = http_stream_read(&stream, buffer, sizeof(buffer));
		if (n == 0) { if(stream.content_length && total!=stream.content_length)note_server(c,false); break; }
		if (n < 0) { if(!cancelled && !(cancel && atomic_load(cancel)))note_server(c,false); ok=false;break; }
        if (total > max_bytes - n) { ok = false; break; }
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

void subsonic_clean_text(char *text){
 if(!text)return;
 for(char *p=text;*p;++p)if((unsigned char)*p<0x20 || (unsigned char)*p==0x7f)*p=' ';
 size_t length=strlen(text);while(length && text[length-1]==' ')text[--length]=0;
 size_t lead=0;while(text[lead]==' ')++lead;if(lead)memmove(text,text+lead,length-lead+1);
}
void subsonic_song_normalize(subsonic_song_t *song){
 subsonic_clean_text(song->title);subsonic_clean_text(song->artist);
 subsonic_clean_text(song->album_artist);subsonic_clean_text(song->album);subsonic_clean_text(song->genre);
}
