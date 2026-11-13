#include "wireless.h"

#include "lvgl/lvgl.h"

#include "src/gui/wireless/airplay.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/speakers.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"

lv_obj_t *wireless_screen;

void wireless_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"wi_fi", &icon_menu_wifi_settings, &wifisettings_screen, NULL},
		{"bluetooth", &icon_menu_bluetooth, &btsettings_screen, NULL},
		{"airplay", &icon_menu_airplay, &airplay_screen, NULL},
		{"transfer", &icon_menu_wifi_transfer, &wifitransfer_screen, NULL},
		{"sonixlink", &icon_menu_sonixlink, &sonixlink_screen, NULL},
		{"dlna", &icon_menu_dlna, &dlna_screen, NULL},
		{"Speakers", &icon_menu_dlna, &speakers_screen, NULL},
	};

	// Keep the existing tile size; additional wireless features scroll below.
	lv_obj_t *grid = gridpage_build(wireless_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_set_scrolling(grid);

	settingsrow_title(wireless_screen, cfg, "wireless");
}
