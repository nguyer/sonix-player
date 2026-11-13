#ifndef SONOS_H
#define SONOS_H

#include <stdbool.h>
#include <stddef.h>

#define SONOS_MAX_ROOMS 32
#define SONOS_ID_MAX 80

typedef struct {
	char id[SONOS_ID_MAX];
	char coordinator[SONOS_ID_MAX];
	char name[128];
	char ip[16];
} sonos_room_t;

typedef struct {
	sonos_room_t rooms[SONOS_MAX_ROOMS];
	int count;
	char seed_ip[16];
	char error[192];
} sonos_t;

// Blocking LAN controller: call only on a worker, with serialized context access.
// No cloud account, queue ownership or stereo/home-theatre reconfiguration.
bool sonos_discover(sonos_t *s);
// Query one speaker directly and retrieve all rooms in its household.
// Accepts an IPv4 address only, without a scheme, port or path.
bool sonos_connect(sonos_t *s, const char *ip);
bool sonos_refresh(sonos_t *s);
bool sonos_join(sonos_t *s, const char *room_id, const char *target_id);
bool sonos_leave(sonos_t *s, const char *room_id);
bool sonos_transport(sonos_t *s, const char *room_id, const char *action);
// Reads a single member using already refreshed topology (no group volume).
bool sonos_get_room_volume(sonos_t *s, const char *room_id, int *percent);
// Apply the same percentage-point change to every visible group member.
// Absolute input computes the delta from the selected room's current level.
bool sonos_group_volume(sonos_t *s, const char *room_id, int value, bool absolute);
bool sonos_volume(sonos_t *s, const char *room_id, int percent);
bool sonos_load_uri(sonos_t *s, const char *room_id, const char *uri, bool play);
bool sonos_play_uri(sonos_t *s, const char *room_id, const char *uri);

typedef struct {
    char state[32], uri[512];
    double position, duration;
    double position_sample; // monotonic seconds when position was received
    int volume;
} sonos_playback_t;
bool sonos_get_playback(sonos_t *s, const char *id, sonos_playback_t *out);
bool sonos_seek(sonos_t *s, const char *id, double seconds);

// Protocol parser is public for fixture tests. On failure clears the room list.
bool sonos_parse_topology(sonos_t *s, const char *xml);

#endif
