#ifndef SUBSONICACCOUNT_H
#define SUBSONICACCOUNT_H
#include "subsonic.h"

// Account file beside the settings, restricted to its owner (0600). Contains
// reusable credentials; it is not encrypted. Does not log account contents.
bool subsonicaccount_save(const char *settings, const subsonic_client_t *client);
bool subsonicaccount_load(const char *settings, subsonic_client_t *client);
bool subsonicaccount_forget(const char *settings);
#endif
