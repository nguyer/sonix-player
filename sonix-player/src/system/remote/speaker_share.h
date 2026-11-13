#ifndef SPEAKER_SHARE_H
#define SPEAKER_SHARE_H
#include <stdbool.h>
#include <stddef.h>

// Publish exactly one completed local audio file, with an unpredictable URL.
// Blocking setup belongs on the controller worker. The server supports HEAD
// and a single byte range, and never maps request paths to filesystem paths.
bool speaker_share_file(const char *path, const char *local_ip, char *url, size_t cap);
// Choose the local IPv4 address from the route to a speaker, rather than
// assuming a wlan0 interface (the simulator can use Ethernet or WSL).
bool speaker_share_address(const char *speaker_ip, char *out, size_t cap);
void speaker_share_revoke(void);
bool speaker_share_active(void);
#endif
