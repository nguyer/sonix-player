#include "subsonicpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/system.h"
#include "src/gui/nowplaying/player.h"

#define PAGE_SIZE 20
lv_obj_t *subsonic_screen;
static lv_obj_t *home, *editor, *field, *edit_title, *results_screen, *results;
static keyboard_t *keyboard;
static char server[512], username[128], password[256], query[513];
static subsonic_client_t account;
#ifdef HOST_BUILD
static char host_storage_root[512];
#endif

static const char *music_storage_root(void) {
#ifdef HOST_BUILD
	// main.c resolves SONIX_SD_ROOT / the desktop Documents folder into the
	// GUI configuration. The device mount layer is not initialized on the host.
	return host_storage_root[0] ? host_storage_root : NULL;
#else
	return storage_sd_root();
#endif
}
static bool connected, busy;
static bool busy_play;
static atomic_bool play_cancelled;
static char message[256] = "Enter your server and account details.";
static int editing, page_offset, page_count;
static subsonic_song_t page[PAGE_SIZE];

// At most one request exists. The worker owns its snapshot until the timer
// consumes it. Even a full gui_post queue cannot strand the page in busy state.
typedef struct {
	subsonic_client_t client;
	bool login;
	bool play;
	subsonic_song_t song;
	char root[512], path[512];
	unsigned queue_revision;
	int queue_index;
	char query[513];
	int offset, count;
	subsonic_song_t songs[PAGE_SIZE];
} request_t;
static pthread_mutex_t result_lock = PTHREAD_MUTEX_INITIALIZER;
static request_t *completed;

static void rebuild_home(void);
static void rebuild_results(void);
static void start_request(bool login, int offset, int play_index);

static bool ignore_tap(void) { return busy || switcher_back_drag_active(); }

static void clear_password(void) {
	volatile char *p = password;
	for (size_t i = 0; i < sizeof(password); ++i) p[i] = 0;
}

static void *request_worker(void *arg) {
	request_t *r = arg;
	if (r->play) streamturn_acquire();
	if (r->play && atomic_load(&play_cancelled)) {
		r->count = -1;
		snprintf(r->client.error, sizeof(r->client.error), "Song download cancelled.");
	} else r->count = r->play ? (subsoniccache_fetch(&r->client, &r->song, r->root, r->path, sizeof(r->path)) ? 0 : -1) :
		r->login ? (subsonic_ping(&r->client) ? 0 : -1) :
		subsonic_search_songs(&r->client, r->query, r->offset, r->songs, PAGE_SIZE);
	if (r->play) streamturn_release();
	pthread_mutex_lock(&result_lock);
	completed = r;
	pthread_mutex_unlock(&result_lock);
	return NULL;
}

static void result_tick(lv_timer_t *timer) {
	(void)timer;
	pthread_mutex_lock(&result_lock);
	request_t *r = completed;
	completed = NULL;
	pthread_mutex_unlock(&result_lock);
	if (!r) return;
	busy = false;
	busy_play = false;
	if (r->play && atomic_load(&play_cancelled)) {
		r->count = -1;
		snprintf(r->client.error, sizeof(r->client.error), "Song download cancelled.");
	}
	if (r->count < 0) {
		snprintf(message, sizeof(message), "%s", subsonic_last_error(&r->client));
		if (r->login) { connected = false; clear_password(); }
	} else if (r->login) {
		subsonic_client_clear(&account);
		account = r->client;
		connected = true;
		clear_password();
		config_set("subsonic", "server", server);
		config_set("subsonic", "username", username);
		snprintf(message, sizeof(message), "%s", config_save() ? "Connected. Search for songs to browse your library." :
			"Connected. Could not save the server and username.");
	} else if (r->play) {
		bool foreground = lv_screen_active() == results_screen || lv_screen_active() == subsonic_screen;
		const char *root = music_storage_root();
		if (foreground && gui_card_available() && root && !strcmp(root, r->root) &&
			playlist_revision() == r->queue_revision && playlist_current_index() == r->queue_index) {
			const char *paths[] = {r->path};
			device_state_play_list(paths, 1, 0);
			player_refresh_now_playing();
			switch_screen(player_screen);
			snprintf(message, sizeof(message), "Song ready.");
		} else snprintf(message, sizeof(message), "Song saved. Select it again to start playback.");
	} else {
		page_offset = r->offset;
		page_count = r->count;
		memcpy(page, r->songs, sizeof(page));
		snprintf(message, sizeof(message), "Connected.");
	}
	bool show_results = !r->login && !r->play && r->count >= 0;
	subsonic_client_clear(&r->client);
	free(r);
	rebuild_home();
	rebuild_results();
	// A result arriving after Back updates the page without pulling the user
	// away from the screen they chose in the meantime.
	if (show_results && lv_screen_active() == subsonic_screen) switch_screen(results_screen);
}

static void start_request(bool login, int offset, int play_index) {
	if (busy) return;
	request_t *r = calloc(1, sizeof(*r));
	if (!r) { snprintf(message, sizeof(message), "Not enough memory."); rebuild_home(); return; }
	r->login = login;
	r->offset = offset;
	r->play = play_index >= 0;
	if (login) {
		if (!subsonic_client_init(&r->client, server, username, password)) {
			snprintf(message, sizeof(message), "%s", subsonic_last_error(&r->client));
			subsonic_client_clear(&r->client); free(r); rebuild_home(); return;
		}
	} else {
		if (!connected) { free(r); return; }
		r->client = account;
		snprintf(r->query, sizeof(r->query), "%s", query);
		if (play_index >= 0) {
			const char *root = music_storage_root();
			if (play_index >= page_count || !gui_card_available() || !root || !*root || strlen(root) >= sizeof(r->root)) {
				subsonic_client_clear(&r->client); free(r);
				snprintf(message, sizeof(message), "Music storage is unavailable.");
				rebuild_home(); rebuild_results(); return;
			}
			r->song = page[play_index];
			snprintf(r->root, sizeof(r->root), "%s", root);
			r->queue_revision = playlist_revision();
			r->queue_index = playlist_current_index();
		}
	}
	busy = true;
	busy_play = r->play;
	atomic_store(&play_cancelled, false);
	pthread_t thread;
	int error = pthread_create(&thread, NULL, request_worker, r);
	if (error) {
		busy = false;
		busy_play = false;
		subsonic_client_clear(&r->client); free(r);
		snprintf(message, sizeof(message), "Could not start the connection worker.");
	} else {
		pthread_detach(thread);
		snprintf(message, sizeof(message), "%s", login ? "Connecting..." :
			play_index >= 0 ? "Downloading song before playback..." : "Searching...");
	}
	rebuild_home();
	rebuild_results();
}

static void connect_cb(lv_event_t *e) { (void)e; if (!ignore_tap()) start_request(true, 0, -1); }

static void disconnect_cb(lv_event_t *e) {
	(void)e;
	if (ignore_tap()) return;
	connected = false;
	subsonic_client_clear(&account);
	clear_password();
	memset(page, 0, sizeof(page));
	page_count = page_offset = 0;
	query[0] = 0;
	snprintf(message, sizeof(message), "Disconnected. Enter your password to reconnect.");
	rebuild_home(); rebuild_results();
}

static void edit_accept(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(field);
	if (editing == 0) snprintf(server, sizeof(server), "%s", text);
	else if (editing == 1) snprintf(username, sizeof(username), "%s", text);
	else if (editing == 2) snprintf(password, sizeof(password), "%s", text);
	else {
		snprintf(query, sizeof(query), "%s", text);
		page_count = page_offset = 0;
		memset(page, 0, sizeof(page));
	}
	lv_textarea_set_text(field, "");
	keyboard_set_visible(keyboard, false);
	switch_screen_return_to(subsonic_screen);
	rebuild_home();
	if (editing == 3) start_request(false, 0, -1);
}

static void editor_left(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(field, "");
	keyboard_set_visible(keyboard, false);
}

static void edit_cb(lv_event_t *e) {
	if (ignore_tap()) return;
	editing = (int)(intptr_t)lv_event_get_user_data(e);
	const char *titles[] = {"Server address", "Username", "Password", "Search songs"};
	const char *values[] = {server, username, password, query};
	const int limits[] = {511, 127, 255, 512};
	lv_label_set_text(edit_title, titles[editing]);
	lv_textarea_set_max_length(field, limits[editing]);
	lv_textarea_set_password_mode(field, editing == 2);
	keyboard_refresh_password(field);
	lv_textarea_set_text(field, values[editing]);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(editor);
}

static void next_cb(lv_event_t *e) { (void)e; if (!ignore_tap()) start_request(false, page_offset + PAGE_SIZE, -1); }
static void prev_cb(lv_event_t *e) { (void)e; if (!ignore_tap()) start_request(false, page_offset - PAGE_SIZE, -1); }

static void play_cb(lv_event_t *e) {
	if (!ignore_tap()) start_request(false, 0, (int)(intptr_t)lv_event_get_user_data(e));
}

static void results_cb(lv_event_t *e) {
	(void)e;
	if (!ignore_tap()) switch_screen(results_screen);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	if (!busy_play || switcher_back_drag_active()) return;
	atomic_store(&play_cancelled, true);
	subsonic_download_cancel();
	snprintf(message, sizeof(message), "Cancelling song download...");
	rebuild_home(); rebuild_results();
}

static void rebuild_home(void) {
	lv_obj_clean(home);
	settingsrow_heading(home, message);
	if (busy) {
		if (busy_play) settingsrow_action(home, "Cancel download", cancel_cb, NULL);
		return;
	}
	if (connected) {
		settingsrow_action(home, "Search songs", edit_cb, (void *)(intptr_t)3);
		if (query[0]) settingsrow_action(home, "Search results", results_cb, NULL);
		settingsrow_action(home, "Disconnect", disconnect_cb, NULL);
	} else {
		lv_obj_t *value;
		settingsrow_add(home, "Server address", &value, edit_cb, (void *)(intptr_t)0);
		lv_label_set_text(value, server[0] ? server : "Not set");
		settingsrow_add(home, "Username", &value, edit_cb, (void *)(intptr_t)1);
		lv_label_set_text(value, username[0] ? username : "Not set");
		settingsrow_add(home, "Password", &value, edit_cb, (void *)(intptr_t)2);
		lv_label_set_text(value, password[0] ? "Entered" : "Not set");
		settingsrow_action(home, "Connect", connect_cb, NULL);
		settingsrow_heading(home, "Use your server root URL. Passwords are kept only until disconnect or exit.");
	}
}

static void rebuild_results(void) {
	lv_obj_clean(results);
	if (busy) {
		settingsrow_heading(results, message);
		if (busy_play) settingsrow_action(results, "Cancel download", cancel_cb, NULL);
		return;
	}
	settingsrow_heading(results, message);
	if (!page_count) settingsrow_heading(results, "No songs found.");
	for (int i = 0; i < page_count; ++i) {
		char text[800];
		snprintf(text, sizeof(text), "%s\n%s — %s", page[i].title, page[i].artist, page[i].album);
		lv_obj_t *row = settingsrow_action(results, text, play_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 3);
	}
	settingsrow_heading(results, "Select a song to download and play it.");
	if (page_offset > 0) settingsrow_action(results, "Previous page", prev_cb, NULL);
	if (page_count == PAGE_SIZE) settingsrow_action(results, "Next page", next_cb, NULL);
}

void subsonicpage_init(gui_config_t *cfg) {
#ifdef HOST_BUILD
	if (cfg->sd_root_path && strlen(cfg->sd_root_path) < sizeof(host_storage_root))
		snprintf(host_storage_root, sizeof(host_storage_root), "%s", cfg->sd_root_path);
#endif
	streamturn_register_abandon(subsonic_download_cancel);
	snprintf(server, sizeof(server), "%s", config_get("subsonic", "server", ""));
	snprintf(username, sizeof(username), "%s", config_get("subsonic", "username", ""));
	subsonic_screen = lv_obj_create(NULL);
	home = settingsrow_page(subsonic_screen, cfg, "Navidrome");
	switcher_attach_back_gesture(subsonic_screen);
	results_screen = lv_obj_create(NULL);
	results = settingsrow_page(results_screen, cfg, "Navidrome songs");
	switcher_attach_back_gesture(results_screen);
	editor = lv_obj_create(NULL);
	lv_obj_add_style(editor, &theme_style_screen, 0);
	edit_title = settingsrow_title(editor, cfg, "Navidrome");
	field = lv_textarea_create(editor);
	lv_textarea_set_one_line(field, true);
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	keyboard_style_password(field, 0);
	keyboard = keyboard_create(editor, cfg->screen_width, 316, field, NULL, "ok", edit_accept, NULL);
	keyboard_set_visible(keyboard, false);
	lv_obj_add_event_cb(editor, editor_left, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(editor);
	lv_timer_create(result_tick, 50, NULL);
	rebuild_home(); rebuild_results();
}
