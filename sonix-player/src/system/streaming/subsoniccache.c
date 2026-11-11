#include "subsoniccache.h"
#include "src/system/core/md5.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static bool fail(subsonic_client_t *c, const char *message) {
	snprintf(c->error, sizeof(c->error), "%s", message);
	return false;
}

bool subsoniccache_identity(const char *path, subsonic_identity_t *identity) {
	char name[600];
	if (!path || !strstr(path, "/.local/subsonic-cache/") ||
		snprintf(name, sizeof(name), "%s.identity", path) >= (int)sizeof(name)) return false;
	FILE *f = fopen(name, "rb");
	if (!f) return false;
	bool ok = fread(identity, sizeof(*identity), 1, f) == 1 && fgetc(f) == EOF;
	fclose(f);
	return ok && !memcmp(identity->magic, "SONIXS1", 8) &&
		memchr(identity->server, 0, sizeof(identity->server)) &&
		memchr(identity->username, 0, sizeof(identity->username)) &&
		memchr(identity->song_id, 0, sizeof(identity->song_id));
}

static bool make_dir(const char *path) {
	if (mkdir(path, 0700) == 0) return true;
	struct stat st;
	return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void tag(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=", key);
	for (; *value; ++value) fputc(*value == '\n' || *value == '\r' ? ' ' : *value, f);
	fputc('\n', f);
}

bool subsoniccache_fetch(subsonic_client_t *c, const subsonic_song_t *song,
						 const char *root, char *path, size_t size) {
	c->error[0] = 0;
	path[0] = 0;
	if (!root || !*root || !song->id[0]) return fail(c, "Music storage is unavailable.");
	char suffix[sizeof(song->suffix)];
	snprintf(suffix, sizeof(suffix), "%s", song->suffix);
	for (char *p = suffix; *p; ++p) *p = (char)tolower((unsigned char)*p);
	const char *formats[] = {"mp3", "flac", "ogg", "opus", "m4a", "aac", "wav", "wv", "ape", "aiff", "aif", "alac", "dsf", "dff", "m4b", "mp4", "caf"};
	bool supported = false;
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
		if (!strcmp(suffix, formats[i])) supported = true;
	if (!supported) return fail(c, "This song's original format is not supported yet.");
	char dir[440];
	if (snprintf(dir, sizeof(dir), "%s/.local", root) >= (int)sizeof(dir) || !make_dir(dir))
		return fail(c, "Cannot create the music cache directory.");
	if (snprintf(dir, sizeof(dir), "%s/.local/subsonic-cache", root) >= (int)sizeof(dir) || !make_dir(dir))
		return fail(c, "Cannot create the music cache directory.");
	subsonic_identity_t identity = {0};
	memcpy(identity.magic, "SONIXS1", 8);
	snprintf(identity.server, sizeof(identity.server), "%s", c->base_url);
	snprintf(identity.username, sizeof(identity.username), "%s", c->username);
	snprintf(identity.song_id, sizeof(identity.song_id), "%s", song->id);
	char key[33], target[512], part[544], tags[544], identity_path[544];
	md5_hex(&identity, sizeof(identity), key);
	if (snprintf(target, sizeof(target), "%s/%s.%s", dir, key, suffix) >= (int)sizeof(target) || strlen(target) >= size)
		return fail(c, "Music cache path is too long.");
	struct stat st;
	subsonic_identity_t saved;
	if (stat(target, &st) == 0 && st.st_size > 0 && subsoniccache_identity(target, &saved) &&
		!memcmp(&identity, &saved, sizeof(identity))) { strcpy(path, target); return true; }
	snprintf(part, sizeof(part), "%s.part", target);
	// Only this single serialized downloader owns .part files in this directory.
	unlink(part);
	// Reserve a whole maximum-sized track before writing. No automatic eviction
	// yet: a sweep must know which files the playback queue still needs.
	DIR *d = opendir(dir);
	if (!d) return fail(c, "Cannot read the music cache directory.");
	long long bytes = 0;
	struct dirent *entry;
	while ((entry = readdir(d))) {
		char file[768];
		snprintf(file, sizeof(file), "%s/%s", dir, entry->d_name);
		if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) bytes += st.st_size;
	}
	closedir(d);
	if (bytes > 512LL * 1024 * 1024) return fail(c, "Song cache is full. Remove old cached songs before downloading more.");
	if (!subsonic_download(c, song->id, part)) return false;
	snprintf(tags, sizeof(tags), "%s.tags", target);
	snprintf(identity_path, sizeof(identity_path), "%s.identity", target);
	FILE *f = fopen(tags, "w");
	bool ok = f != NULL;
	if (f) {
		tag(f, "title", song->title); tag(f, "artist", song->artist); tag(f, "album", song->album);
		fprintf(f, "track=%d\n", song->track);
		if (ferror(f)) ok = false;
		if (fclose(f) != 0) ok = false;
	}
	f = ok ? fopen(identity_path, "wb") : NULL;
	if (f) {
		if (fwrite(&identity, sizeof(identity), 1, f) != 1) ok = false;
		if (fclose(f) != 0) ok = false;
	} else ok = false;
	if (!ok || rename(part, target) != 0) {
		unlink(part); unlink(tags); unlink(identity_path);
		return fail(c, "Cannot finish saving the downloaded song.");
	}
	strcpy(path, target);
	return true;
}
