#ifndef SUBSONICOFFLINEPAGE_H
#define SUBSONICOFFLINEPAGE_H
#include "src/gui/shell/gui.h"
#include "src/system/streaming/subsonic.h"
void subsonicofflinepage_init(gui_config_t *cfg,subsonic_client_t *account);
void subsonicofflinepage_open(void);
void subsonicofflinepage_notify(void);
void subsonicofflinepage_refresh(void);
#endif
