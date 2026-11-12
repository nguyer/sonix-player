#ifndef SUBSONICCATALOG_H
#define SUBSONICCATALOG_H
#include "subsonic.h"
#include <stdatomic.h>
// Metadata-only sync. Visible catalog is replaced only after every page arrived.
int subsoniccatalog_sync(subsonic_client_t *client, const char *root, const atomic_bool *cancel);
bool subsoniccatalog_busy(void);
int subsoniccatalog_cleanup(subsonic_client_t *client,const char *root,const atomic_bool *cancel);
#endif
