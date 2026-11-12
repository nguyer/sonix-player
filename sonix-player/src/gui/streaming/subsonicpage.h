#ifndef SUBSONICPAGE_H
#define SUBSONICPAGE_H
#include "src/gui/shell/gui.h"
extern lv_obj_t *subsonic_screen;
void subsonicpage_init(gui_config_t *cfg);
// NULL syncs linked/server playlists; a local name explicitly uploads or syncs it.
bool subsonicpage_sync_playlists(const char *local_name);
bool subsonicpage_selective(void);
bool subsonicpage_sync_busy(void);
bool subsonicpage_download_pending(const char *const *paths,int count,const char *playlist);
// -1: no eligible Navidrome tracks, 0: incomplete, 1: all eligible tracks cached.
int subsonicpage_download_state(const char *const *paths,int count);
bool subsonicpage_download_paths(const char *const *paths,int count,int kind,const char *group,bool remove);
bool subsonicpage_remove_all_downloads(void);
unsigned subsonicpage_download_revision(void);
void subsonicpage_downloads_changed(void);
#endif
