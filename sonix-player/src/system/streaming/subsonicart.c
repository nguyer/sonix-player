#include "subsonicart.h"
#include "subsoniccache.h"
#include "src/system/library/libraryremote.h"
#include "src/system/core/md5.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool art_path(const char *root, const char *source, const char *id, char *out, size_t size) {
	if (!root || !*root || !id || !*id) return false;
	char hash[33];
	md5_hex(id, strlen(id), hash);
	return snprintf(out, size, "%s/.local/subsonic-art/%s-%s.jpg", root, source, hash) < (int)size;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool directory(const char *path) {
	struct stat st;
	return mkdir(path, 0700) == 0 || (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool subsonicart_fetch(subsonic_client_t *c, const char *id, const char *root) {
	char source[33], path[640], dir[560], temporary[680];
	subsoniccache_source(c, source);
	if (!art_path(root, source, id, path, sizeof(path))) return false;
	if (exists(path)) return true;
	if (snprintf(dir, sizeof(dir), "%s/.local", root) >= (int)sizeof(dir) || !directory(dir)) return false;
	if (snprintf(dir, sizeof(dir), "%s/.local/subsonic-art", root) >= (int)sizeof(dir) || !directory(dir)) return false;
	char *data = NULL;
	size_t size = 0;
	if (!subsonic_cover(c, id, &data, &size)) return false;
	snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	int fd = mkstemp(temporary);
	FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
	bool ok = f != NULL;
	if (f) {
		if (fwrite(data, 1, size, f) != size) ok = false;
		if (fclose(f) != 0) ok = false;
	} else if (fd >= 0) close(fd);
	free(data);
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok && fd >= 0) unlink(temporary);
	return ok;
}

bool subsonicart_for_track(const char *path, char *out, size_t size) {
	if (!path) return false;
	const char *cache = strstr(path, "/.local/subsonic-cache/");
	if (!cache || cache - path >= 512) return false;
	library_remote_track_t remote;
	if (!library_remote_get(path, &remote)) return false;
	char root[512];
	memcpy(root, path, (size_t)(cache - path)); root[cache - path] = 0;
	const char *id = remote.song.cover_id[0] ? remote.song.cover_id : remote.song.album_id;
	return art_path(root, remote.source, id, out, size) && exists(out);
}
