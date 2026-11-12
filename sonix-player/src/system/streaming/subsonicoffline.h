#ifndef SUBSONICOFFLINE_H
#define SUBSONICOFFLINE_H
#include "subsonic.h"
typedef struct { bool running,removing; int done,total; long long bytes; char message[256],source[33],path[512]; } subsonic_offline_status_t;
void subsonicoffline_set_notify(void (*notify)(void));
bool subsonicoffline_start(const subsonic_client_t *client,const char *root,bool refresh);
// Explicit removal checks account identity and deterministic cache paths.
bool subsonicoffline_remove(const subsonic_client_t *client,const char *root,const char *const *paths,int count,bool all);
void subsonicoffline_cancel(void);
void subsonicoffline_status(subsonic_offline_status_t *out);
bool subsonicoffline_busy(void);
unsigned subsonicoffline_revision(void);
void subsonicoffline_yield_begin(void);
void subsonicoffline_yield_end(void);
#endif
