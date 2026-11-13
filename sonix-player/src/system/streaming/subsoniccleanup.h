#ifndef SUBSONICCLEANUP_H
#define SUBSONICCLEANUP_H
#include "subsonic.h"
// Worker-only, no server requests. Leaves catalog/account intact on file errors.
bool subsoniccleanup_connection(subsonic_client_t *client,const char *root);
#endif
