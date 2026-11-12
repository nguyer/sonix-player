#include "subsonicaccount.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
	char magic[8], server[512], username[128], password[256];
} account_file_t;

static bool name_for(const char *settings, char *out, size_t size) {
	return settings && *settings && snprintf(out, size, "%s.subsonic-account", settings) < (int)size;
}

static void wipe(account_file_t *data) {
	volatile unsigned char *p = (volatile unsigned char *)data;
	for (size_t i = 0; i < sizeof(*data); ++i) p[i] = 0;
}

bool subsonicaccount_save(const char *settings, const subsonic_client_t *c) {
	char name[600], temporary[640];
	if (!name_for(settings, name, sizeof(name))) return false;
	snprintf(temporary, sizeof(temporary), "%s.XXXXXX", name);
	int fd = mkstemp(temporary);
	if (fd < 0) return false;
	account_file_t data = {0};
	memcpy(data.magic, "SONIXA1", 8);
	strcpy(data.server, c->base_url); strcpy(data.username, c->username); strcpy(data.password, c->password);
	FILE *f = fdopen(fd, "wb");
	bool ok = f && fchmod(fd, 0600) == 0;
	if (f) {
		if (ok && fwrite(&data, sizeof(data), 1, f) != 1) ok = false;
		if (fflush(f) != 0 || fsync(fd) != 0) ok = false;
		if (fclose(f) != 0) ok = false;
	} else close(fd);
	wipe(&data);
	if (ok) ok = rename(temporary, name) == 0;
	if (!ok) unlink(temporary);
	return ok;
}

bool subsonicaccount_load(const char *settings, subsonic_client_t *c) {
	subsonic_client_clear(c);
	char name[600];
	if (!name_for(settings, name, sizeof(name))) return false;
	int fd = open(name, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return false;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		(st.st_mode & 077) || st.st_size != sizeof(account_file_t)) { close(fd); return false; }
	FILE *f = fdopen(fd, "rb");
	if (!f) { close(fd); return false; }
	account_file_t data = {0};
	bool ok = fread(&data, sizeof(data), 1, f) == 1;
	fclose(f);
	ok = ok && !memcmp(data.magic, "SONIXA1", 8) && memchr(data.server, 0, sizeof(data.server)) &&
		memchr(data.username, 0, sizeof(data.username)) && memchr(data.password, 0, sizeof(data.password));
	if (ok) ok = subsonic_client_init(c, data.server, data.username, data.password);
	wipe(&data);
	return ok;
}

bool subsonicaccount_forget(const char *settings) {
	char name[600];
	return name_for(settings, name, sizeof(name)) && (unlink(name) == 0 || errno == ENOENT);
}
