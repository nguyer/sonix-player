#include "speaker_share.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int listener = -1, published = -1, clients;
static unsigned generation;
static unsigned short port;
static char bound_ip[16], resource[128], mime[48];

bool speaker_share_address(const char *speaker_ip, char *out, size_t cap) {
	if (!out || !cap) return false;
	out[0] = 0;
	struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(1400)};
	if (!speaker_ip || inet_pton(AF_INET, speaker_ip, &peer.sin_addr) != 1 || peer.sin_addr.s_addr == INADDR_ANY) return false;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return false;
	struct sockaddr_in local; socklen_t size = sizeof(local);
	// UDP connect chooses a route without sending a packet to the speaker.
	bool ok = connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
		getsockname(fd, (struct sockaddr *)&local, &size) == 0 && local.sin_addr.s_addr != INADDR_ANY &&
		inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)cap) != NULL;
	close(fd); return ok;
}

static bool send_all(int fd, const void *data, size_t size) {
	const char *p = data;
	while (size) {
		ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n; size -= n;
	}
	return true;
}

static void reply(int fd, const char *status) {
	char header[256]; int n = snprintf(header, sizeof(header),
		"HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
	send_all(fd, header, n);
}

// Strict decimal parser, rejecting signs, overflow and empty fields.
static bool decimal(const char **p, uint64_t *value) {
	const char *start = *p; uint64_t n = 0;
	while (**p >= '0' && **p <= '9') {
		unsigned digit = *(*p)++ - '0';
		if (n > (UINT64_MAX - digit) / 10) return false;
		n = n * 10 + digit;
	}
	*value = n; return *p != start;
}

static bool byte_range(const char *p, uint64_t size, uint64_t *start, uint64_t *end) {
	if (strncmp(p, "bytes=", 6) || !size) return false;
	p += 6;
	if (*p == '-') {
		uint64_t suffix; p++;
		if (!decimal(&p, &suffix) || !suffix || *p) return false;
		*start = suffix < size ? size - suffix : 0; *end = size - 1; return true;
	}
	if (!decimal(&p, start) || *p++ != '-' || *start >= size) return false;
	*end = size - 1;
	if (*p && !decimal(&p, end)) return false;
	if (*p || *end < *start) return false;
	if (*end >= size) *end = size - 1;
	return true;
}

static void serve(int fd) {
	struct timeval timeout = {.tv_sec = 5};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	struct timespec began, now; clock_gettime(CLOCK_MONOTONIC, &began);
	char request[8192]; size_t used = 0;
	while (used < sizeof(request) - 1) {
		ssize_t n = recv(fd, request + used, sizeof(request) - used - 1, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return;
		used += n; request[used] = 0;
		if (strstr(request, "\r\n\r\n")) break;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - began.tv_sec >= 5) { reply(fd, "408 Request Timeout"); return; }
	}
	char *line = strstr(request, "\r\n");
	if (!line || !strstr(request, "\r\n\r\n")) { reply(fd, "400 Bad Request"); return; }
	*line = 0;
	char method[8], path[160], version[16], extra;
	if (sscanf(request, "%7s %159s %15s %c", method, path, version, &extra) != 3 ||
		(strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) { reply(fd, "400 Bad Request"); return; }
	bool head = !strcmp(method, "HEAD");
	if (!head && strcmp(method, "GET")) { reply(fd, "405 Method Not Allowed"); return; }
	int file = -1; unsigned serial; char content_type[48];
	pthread_mutex_lock(&lock);
	serial = generation;
	if (published >= 0 && !strcmp(path, resource)) file = dup(published);
	snprintf(content_type, sizeof(content_type), "%s", mime);
	pthread_mutex_unlock(&lock);
	if (file < 0) { reply(fd, "404 Not Found"); return; }
	struct stat st;
	if (fstat(file, &st) || st.st_size <= 0) { close(file); reply(fd, "404 Not Found"); return; }
	uint64_t size = st.st_size, start = 0, end = size - 1;
	bool ranged = false, invalid = false;
	char *p = line + 2;
	while (*p && p[0] != '\r') {
		char *next = strstr(p, "\r\n");
		if (!next) { invalid = true; break; }
		*next = 0;
		if (!strncasecmp(p, "Range:", 6)) {
			p += 6; while (*p == ' ' || *p == '\t') p++;
			if (ranged || !byte_range(p, size, &start, &end)) invalid = true;
			ranged = true;
		}
		p = next + 2;
	}
	char header[640]; int n;
	if (invalid) {
		n = snprintf(header, sizeof(header), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%" PRIu64
			"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", size);
		send_all(fd, header, n); close(file); return;
	}
	char range[128] = "";
	if (ranged) snprintf(range, sizeof(range), "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n", start, end, size);
	n = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %" PRIu64
		"\r\nAccept-Ranges: bytes\r\n%sConnection: close\r\n\r\n",
		ranged ? "206 Partial Content" : "200 OK", content_type, end - start + 1, range);
	if (!send_all(fd, header, n) || head) { close(file); return; }
	char buffer[32768];
	while (start <= end) {
		pthread_mutex_lock(&lock); bool valid = generation == serial; pthread_mutex_unlock(&lock);
		if (!valid) break;
		size_t want = end - start + 1 > sizeof(buffer) ? sizeof(buffer) : (size_t)(end - start + 1);
		ssize_t got = pread(file, buffer, want, (off_t)start);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0 || !send_all(fd, buffer, got)) break;
		start += got;
	}
	close(file);
}

static void *client_thread(void *arg) {
	int fd = *(int *)arg; free(arg); serve(fd); close(fd);
	pthread_mutex_lock(&lock); clients--; pthread_mutex_unlock(&lock);
	return NULL;
}

static void *accept_thread(void *arg) {
	int fd = *(int *)arg; free(arg);
	while (true) {
		int client = accept(fd, NULL, NULL);
		if (client < 0) { if (errno == EINTR) continue; break; }
		pthread_mutex_lock(&lock); bool full = clients >= 4; if (!full) clients++; pthread_mutex_unlock(&lock);
		if (full) { close(client); continue; }
		int *owned = malloc(sizeof(*owned)); pthread_t thread;
		if (owned) *owned = client;
		pthread_attr_t attr; pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 128 * 1024); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		int result = owned ? pthread_create(&thread, &attr, client_thread, owned) : -1;
		pthread_attr_destroy(&attr);
		if (result) {
			free(owned); close(client); pthread_mutex_lock(&lock); clients--; pthread_mutex_unlock(&lock);
		}
	}
	close(fd); return NULL;
}

void speaker_share_revoke(void) {
	pthread_mutex_lock(&lock);
	if (published >= 0) close(published);
	published = -1; resource[0] = 0; generation++;
	pthread_mutex_unlock(&lock);
}

bool speaker_share_active(void) {
	pthread_mutex_lock(&lock); bool active = published >= 0; pthread_mutex_unlock(&lock);
	return active;
}

bool speaker_share_file(const char *path, const char *local_ip, char *url, size_t cap) {
	if (!path || !local_ip || !url || !cap) return false;
	url[0] = 0;
	struct in_addr address;
	if (inet_pton(AF_INET, local_ip, &address) != 1 || address.s_addr == INADDR_ANY) return false;
	const char *extension = strrchr(path, '.'); const char *type = NULL;
	if (!extension) return false;
	if (!strcasecmp(extension, ".mp3")) type = "audio/mpeg";
	else if (!strcasecmp(extension, ".flac")) type = "audio/flac";
	else if (!strcasecmp(extension, ".wav")) type = "audio/wav";
	else if (!strcasecmp(extension, ".m4a")) type = "audio/mp4";
	else if (!strcasecmp(extension, ".aac")) type = "audio/aac";
	else if (!strcasecmp(extension, ".aif") || !strcasecmp(extension, ".aiff")) type = "audio/aiff";
	if (!type) return false;
	int file = open(path, O_RDONLY | O_NONBLOCK); struct stat st;
	if (file < 0) return false;
	if (fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) { close(file); return false; }
	unsigned char random[24]; int rng = open("/dev/urandom", O_RDONLY);
	ssize_t got = rng >= 0 ? read(rng, random, sizeof(random)) : -1;
	if (rng >= 0) close(rng);
	if (got != (ssize_t)sizeof(random)) { close(file); return false; }
	char token[49]; for (size_t i = 0; i < sizeof(random); i++) snprintf(token + i * 2, 3, "%02x", random[i]);
	char new_resource[128]; snprintf(new_resource, sizeof(new_resource), "/track/%s%s", token, extension);
	// This API has a single controller worker. Only request readers run concurrently.
	if (listener >= 0 && strcmp(bound_ip, local_ip)) {
		// The accept thread owns close(); shutdown wakes its blocking accept.
		shutdown(listener, SHUT_RDWR); listener = -1;
	}
	if (listener < 0) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr = address}; socklen_t len = sizeof(addr);
		if (fd < 0) { close(file); return false; }
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 4) || getsockname(fd, (struct sockaddr *)&addr, &len)) {
			close(fd); close(file); return false;
		}
		int *owned = malloc(sizeof(*owned)); pthread_t thread;
		if (owned) *owned = fd;
		if (!owned || pthread_create(&thread, NULL, accept_thread, owned)) { free(owned); close(fd); close(file); return false; }
		pthread_detach(thread); listener = fd; port = ntohs(addr.sin_port);
		snprintf(bound_ip, sizeof(bound_ip), "%s", local_ip);
	}
	int n = snprintf(url, cap, "http://%s:%u%s", local_ip, port, new_resource);
	if (n < 0 || (size_t)n >= cap) { close(file); url[0] = 0; return false; }
	pthread_mutex_lock(&lock);
	if (published >= 0) close(published);
	published = file; generation++;
	snprintf(resource, sizeof(resource), "%s", new_resource); snprintf(mime, sizeof(mime), "%s", type);
	pthread_mutex_unlock(&lock); return true;
}
