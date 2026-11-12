#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H
#include "src/gui/shell/gui.h"
extern lv_obj_t *subsonic_screen;
void subsonicpage_init(gui_config_t *cfg);
// NULL syncs linked/server playlists; a local name explicitly uploads or syncs it.
bool subsonicpage_sync_playlists(const char *local_name);
#endif
