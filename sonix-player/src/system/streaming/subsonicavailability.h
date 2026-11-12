#ifndef SUBSONICAVAILABILITY_H
#define SUBSONICAVAILABILITY_H
#include <stdbool.h>
// Local lookup only. Local files retain existing behavior; remote entries need
// either a complete downloaded file or a reachable authenticated server.
bool subsonic_track_available(const char *path);
// Album lookups return a cached value immediately and queue background work
// on a miss. Unknown albums stay available until their first result arrives.
void subsonicavailability_refresh(void);
void subsonicavailability_set_notify(void (*notify)(void));
void subsonicavailability_invalidate(void);
bool subsonic_album_available(const char *album);
#endif
