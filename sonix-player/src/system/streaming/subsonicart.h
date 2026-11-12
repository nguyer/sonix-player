#ifndef SUBSONICART_H
#define SUBSONICART_H
#include "subsonic.h"
// Shared, persistent artwork, independent of the audio cache. Worker-only fetch.
bool subsonicart_fetch(subsonic_client_t *client, const char *cover_id, const char *root);
// Local lookup only: safe for cover-loader threads and never performs network IO.
bool subsonicart_for_track(const char *track_path, char *out, size_t size);
#endif
