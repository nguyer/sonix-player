#include "src/gui/library/medialist.h"
#include "src/gui/shell/topbar.h"
#include "src/system/net/wifi.h"
#include "subsonicpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/confirm.h"

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
#include "src/system/streaming/subsonicaccount.h"
#include "src/system/streaming/subsoniccatalog.h"
#include "src/system/streaming/subsonicoffline.h"
#include "src/system/streaming/subsonicavailability.h"
#include "src/system/library/libraryoffline.h"
#include "src/gui/streaming/subsonicofflinepage.h"
#include "src/system/streaming/subsonicplaylists.h"
#include "src/system/library/libraryremote.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/system.h"
#include "src/gui/nowplaying/player.h"
#include <sys/stat.h>
#include <time.h>

lv_obj_t *subsonic_screen;
static lv_obj_t *home, *editor, *field, *edit_title, *remove_connection_icon;
static unsigned download_revision;
unsigned subsonicpage_download_revision(void){return download_revision+subsonicoffline_revision();}
void subsonicpage_downloads_changed(void){++download_revision;subsonicavailability_invalidate();topbar_refresh_navidrome();medialist_notify_availability();}
static keyboard_t *keyboard;
static char server[512], username[128], password[256];
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
static bool busy_sync, saved_connection;
static char waiting_path[512];
static atomic_bool play_cancelled;
static char message[256] = "Enter your server and account details.";
static int editing;

// At most one request exists. The worker owns its snapshot until the timer
// consumes it. Even a full gui_post queue cannot strand the page in busy state.
typedef struct {
	subsonic_client_t client;
	bool login;
	bool play;
	bool sync, resume_queue;
	bool playlists;
	bool background;
	bool probe;
    bool temporary;
	char upload[201];
	subsonic_song_t song;
	char root[512], path[512];
	unsigned queue_revision;
	int queue_index;
	int count;
} request_t;
static pthread_mutex_t result_lock = PTHREAD_MUTEX_INITIALIZER;
static request_t *completed;

static void rebuild_home(void);
static void request_ready(void *unused);
static void availability_event(void *unused);
static void offline_event(void *unused);
static void queue_availability(void){gui_post_coalesced(availability_event,NULL);}
static void queue_offline(void){gui_post_coalesced(offline_event,NULL);}
static void start_request(bool login);
static void submit_request(request_t *r);
static void prepare_waiting(void);
static void auto_sync_playlists(void);
static void offline_and_probe_tick(void);
static time_t probe_at,offline_retry_at;
static time_t auto_retry_at;
static char auto_last_error[256];

static bool ignore_tap(void) { return busy || switcher_back_drag_active(); }

static void clear_password(void) {
	volatile char *p = password;
	for (size_t i = 0; i < sizeof(password); ++i) p[i] = 0;
}


typedef struct {char path[512],root[512];subsonic_client_t owner;} temporary_t;
static temporary_t temporary_files[8];
static void discard_finished_temporary(void){
 if(busy_play || subsonicoffline_busy())return;
 char current[512];audio_get_current_file(current,sizeof(current));
 bool stopped=audio_get_status()==AUDIO_STATUS_STOPPED;
 for(int i=0;i<8;++i)if(temporary_files[i].path[0] &&
  (stopped || strcmp(current,temporary_files[i].path))){
  temporary_t *file=&temporary_files[i];
  if(subsoniccache_discard_temporary(&file->owner,file->root,file->path)){
   subsonic_client_clear(&file->owner);file->path[0]=0;subsonicpage_downloads_changed();
  }
 }
}
static void remember_temporary(const subsonic_client_t *owner,const char *root,const char *path){
 if(!subsoniccache_is_temporary(path))return;
 for(int i=0;i<8;++i)if(!strcmp(temporary_files[i].path,path))return;
 for(int i=0;i<8;++i)if(!temporary_files[i].path[0]){
  snprintf(temporary_files[i].path,512,"%s",path);snprintf(temporary_files[i].root,512,"%s",root);
  temporary_files[i].owner=*owner; // no password is needed to validate deletion
  memset(temporary_files[i].owner.password,0,sizeof(temporary_files[i].owner.password));return;
 }
}
static bool temporary_playback(void){
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 return library_offline_options(source,&options) && options.mode==OFFLINE_SELECTIVE && !options.auto_cache;
}

static int sync_all(request_t *r) {
 int count=subsoniccatalog_sync(&r->client,r->root,&play_cancelled);
 if(count<0 || atomic_load(&play_cancelled))return -1;
 if(subsonicplaylists_sync(&r->client,&play_cancelled)<0)return -1;
 return count;
}

static bool fetch_playback(request_t *r){
 if(!r->temporary)return subsoniccache_fetch(&r->client,&r->song,r->root,r->path,sizeof(r->path));
 char source[33];offline_options_t options;subsoniccache_source(&r->client,source);
 if(!library_offline_options(source,&options))return false;
 return subsoniccache_fetch_offline(&r->client,&r->song,r->root,r->path,sizeof(r->path),512LL*1024*1024,options.reserve_bytes,false,&play_cancelled);
}
static void *request_worker(void *arg) {
	request_t *r = arg;
    if(r->login){const char *root=music_storage_root();if(root){char keep[512];audio_get_current_file(keep,sizeof(keep));subsoniccache_cleanup_temporary(&r->client,root,audio_get_status()==AUDIO_STATUS_STOPPED?NULL:keep);}}
	if(r->play)subsonicoffline_yield_begin();
	if (r->play) streamturn_acquire();

 if((r->play || r->sync || r->playlists) && atomic_load(&play_cancelled)){
  r->count=-1;snprintf(r->client.error,sizeof(r->client.error),"Sync cancelled.");
 } else r->count = r->probe ? (subsonic_ping(&r->client)?0:-1) : r->playlists ? (r->upload[0] ?
		(subsonicplaylists_upload(&r->client,r->upload,&play_cancelled)?0:-1) :
		(r->background ? subsonicplaylists_sync_pending(&r->client,&play_cancelled) :
		subsonicplaylists_sync(&r->client,&play_cancelled))) :
		r->sync ? sync_all(r) :
		r->play ? (fetch_playback(r) ? 0 : -1) :
		r->login ? (subsonic_ping(&r->client) ? 0 : -1) :
		-1;
 if(r->play && r->count>=0 && r->temporary && !subsoniccache_mark_temporary(r->path)){
  r->count=-1;snprintf(r->client.error,sizeof(r->client.error),"Cannot mark the temporary playback download.");
 }
	if (r->play){streamturn_release();subsonicoffline_yield_end();}
	pthread_mutex_lock(&result_lock);
	completed = r;
	pthread_mutex_unlock(&result_lock);
    gui_post_coalesced(request_ready,NULL);
	return NULL;
}

static void consume_result(void) {
	pthread_mutex_lock(&result_lock);
	request_t *r = completed;
	completed = NULL;
	pthread_mutex_unlock(&result_lock);
	if (!r) {if(!busy)prepare_waiting();return;}
	busy = false;
	busy_play = false;
 busy_sync = false;
 topbar_refresh_navidrome();
 if(r->probe){
  connected=r->count>=0;
  if(!connected)snprintf(message,sizeof(message),"Server unavailable. Downloaded music is still playable.");
  probe_at=time(NULL)+30;subsonic_client_clear(&r->client);free(r);rebuild_home();return;
 }
 if(r->login)probe_at=time(NULL)+30;
 if(r->play && r->path[0])remember_temporary(&r->client,r->root,r->path);

 if((r->play || r->sync || r->playlists) && atomic_load(&play_cancelled)){
  r->count=-1;snprintf(r->client.error,sizeof(r->client.error),"Sync cancelled.");
 }
	if (r->count < 0) {
		snprintf(message, sizeof(message), "%s", subsonic_last_error(&r->client));
		if (r->login) { connected = false; clear_password(); }
		if (r->resume_queue || !r->background) gui_notify_popup(message);
		if(r->background){if(r->sync)offline_retry_at=time(NULL)+60;else auto_retry_at=time(NULL)+30;}
		if(r->background && strcmp(auto_last_error,message)) {
			snprintf(auto_last_error,sizeof(auto_last_error),"%s",message);gui_notify_popup(message);
		}
	} else if (r->login) {
		subsonic_client_clear(&account);
		account = r->client;
		connected = true;
		clear_password();
		config_set("subsonic", "server", server);
		config_set("subsonic", "username", username);
		config_save();
		saved_connection = subsonicaccount_save(config_path(), &account);
		snprintf(message, sizeof(message), "%s", saved_connection ? "Connected." :
			"Connected for this session. Could not save the connection; check the settings file location.");
	} else if(r->playlists) {
		if(r->background){auto_last_error[0]=0;auto_retry_at=0;}
		if(r->upload[0]) snprintf(message,sizeof(message),"Playlist synchronized with Navidrome.");
		else snprintf(message,sizeof(message),"%d Navidrome playlist(s) synchronized.",r->count);
	} else if (r->sync) {
		snprintf(message, sizeof(message), "Synced %d songs and server playlists.", r->count);
        char source[33];offline_options_t options;subsoniccache_source(&account,source);
        library_offline_complete(source,time(NULL));offline_retry_at=0;
        if(library_offline_options(source,&options) && options.mode!=OFFLINE_OFF) {
            if(subsonicoffline_start(&account,r->root,false))
                snprintf(message,sizeof(message),"Catalog and playlists synced. Downloading offline music...");
            else snprintf(message,sizeof(message),"Catalog and playlists synced. Offline downloads are waiting; check offline settings.");
        } else library_reorganize();
	} else if (r->play) {
        remember_temporary(&r->client,r->root,r->path);
		bool foreground = r->resume_queue || lv_screen_active() == subsonic_screen;
		const char *root = music_storage_root();
		if (foreground && gui_card_available() && root && !strcmp(root, r->root) &&
			playlist_revision() == r->queue_revision && playlist_current_index() == r->queue_index) {
			if (r->resume_queue) device_state_play_queue_index(r->queue_index);
			else {
				const char *paths[] = {r->path};
				device_state_play_list(paths, 1, 0);
			}
			player_refresh_now_playing();
			if (!r->resume_queue) switch_screen(player_screen);
			snprintf(message, sizeof(message), "Song ready.");
		} else snprintf(message, sizeof(message), "Song saved. Select it again to start playback.");
	}
	if(r->play)subsonicpage_downloads_changed();
	subsonic_client_clear(&r->client);
	free(r);
	topbar_refresh_navidrome();
	rebuild_home();

	// A result arriving after Back updates the page without pulling the user
	// away from the screen they chose in the meantime.
}

static void request_ready(void *unused){(void)unused;consume_result();if(!busy)prepare_waiting();}
static void availability_event(void *unused){
 (void)unused;medialist_notify_availability();
}
static void offline_event(void *unused){
 (void)unused;
 subsonicpage_downloads_changed();rebuild_home();subsonicofflinepage_notify();
 if(!busy)prepare_waiting();
}
static void maintenance_tick(lv_timer_t *timer){
 (void)timer;
 discard_finished_temporary();
 if(!busy)prepare_waiting();
 if(!busy)auto_sync_playlists();
 if(!busy)offline_and_probe_tick();
}

static void start_request(bool login) {
 if(busy)return;
 if(subsonicoffline_busy()){gui_notify_popup("Stop offline sync before changing the account or catalog.");return;}
 request_t *r=calloc(1,sizeof(*r));if(!r)return;
 r->login=login;r->sync=!login;
 if(login) {
  bool restored=!password[0] && subsonicaccount_load(config_path(),&r->client) &&
   !strcmp(r->client.base_url,server) && !strcmp(r->client.username,username);
  if(!restored && !subsonic_client_init(&r->client,server,username,password)){
   snprintf(message,sizeof(message),"%s",subsonic_last_error(&r->client));
   subsonic_client_clear(&r->client);free(r);gui_notify_popup(message);rebuild_home();return;
  }
 } else {
  if(!account.password[0]){free(r);return;}
  r->client=account;
  const char *root=music_storage_root();
  if(!root || !gui_card_available() || strlen(root)>=sizeof(r->root)){
   subsonic_client_clear(&r->client);free(r);
   gui_notify_popup("Music storage is unavailable.");return;
  }
  strcpy(r->root,root);
 }
 submit_request(r);
}

static void submit_request(request_t *r) {
	busy = true;
	busy_play = r->play;
	busy_sync = r->sync || r->playlists;
    topbar_refresh_navidrome();
	atomic_store(&play_cancelled, false);
	pthread_t thread;
	int error = pthread_create(&thread, NULL, request_worker, r);
	if (error) {
		busy = false;
		busy_play = false;
		busy_sync = false;
		subsonic_client_clear(&r->client); free(r);
		snprintf(message, sizeof(message), "Could not start the connection worker.");
	} else {
		pthread_detach(thread);
		snprintf(message, sizeof(message), "%s", r->login ? "Connecting..." :
			r->playlists ? "Synchronizing Navidrome playlists..." :
			r->sync ? "Syncing catalog, artwork and playlists..." : r->play ? "Downloading song before playback..." : "Working...");
	}
	topbar_refresh_navidrome();
	rebuild_home();

}

// Polling never sends requests for local-only playlist edits. Quiet time
// coalesces drag/reorder bursts; retries are bounded and do not steal navigation.
static void auto_sync_playlists(void) {
 static uint32_t last_probe;
 if(!subsonic_network_available() || busy || subsonicoffline_busy() || !account.password[0] || lv_tick_elaps(last_probe)<1000)return;
 last_probe=lv_tick_get();
 time_t now=time(NULL);
 if(now<auto_retry_at && auto_retry_at-now<=30)return;
 char source[33];subsoniccache_source(&account,source);
 if(!library_playlist_auto_pending(source,2))return;
 request_t *r=calloc(1,sizeof(*r));
 if(!r)return;
 r->client=account;r->playlists=true;r->background=true;
 auto_retry_at=now+30;
 submit_request(r);
}

#ifndef HOST_BUILD
static void network_changed(bool online){
 (void)online;
 // Re-read the latest snapshot: an older worker notification may race a
 // user switching the radio off.
 wifi_status_t state;wifi_get_status(&state);
 bool available=state.state==WIFI_STATE_CONNECTED && state.ip[0];
 subsonic_set_network_available(available);
 if(!available)subsonicoffline_cancel();
}
#endif
static void offline_and_probe_tick(void){
 static uint32_t last_tick;
 if(busy || !account.password[0] || lv_tick_elaps(last_tick)<1000)return;
 last_tick=lv_tick_get();
 bool network=subsonic_network_available();
 static bool was_network;
 if(network!=was_network){was_network=network;probe_at=0;}
 if(!network){connected=false;return;}
 if(subsonic_server_available(account.base_url,account.username) && !connected){connected=true;rebuild_home();}
 if(subsonicoffline_busy())return;
 char source[33];subsoniccache_source(&account,source);
 offline_options_t options;time_t now=time(NULL);
 if(library_offline_options(source,&options) && options.automatic &&
  subsonic_server_available(account.base_url,account.username) &&
  now>=offline_retry_at && (!options.last_run || now-options.last_run>=options.interval_minutes*60) && gui_card_available()){
  const char *root=music_storage_root();
  if(root && strlen(root)<sizeof(((request_t *)0)->root)){
   request_t *r=calloc(1,sizeof(*r));
   if(r){r->client=account;r->sync=true;r->background=true;strcpy(r->root,root);
    offline_retry_at=now+60;submit_request(r);return;}
  }
 }
 if(!subsonic_server_available(account.base_url,account.username) && now>=probe_at){
  request_t *r=calloc(1,sizeof(*r));if(!r)return;
  r->client=account;r->probe=true;r->background=true;probe_at=now+30;submit_request(r);
 }
}
static void offline_open_cb(lv_event_t *e){(void)e;subsonicofflinepage_open();}

static bool prepare_track(const char *path) {
	library_remote_track_t remote;
	if (!library_remote_get(path, &remote)) return true;
	if(!subsonic_track_available(path)){device_state_stop();return false;}
	struct stat st;
	if (stat(path, &st) == 0 && st.st_size > 0) {
        if(temporary_playback() && subsoniccache_mark_temporary(path)){
         const char *root=music_storage_root();if(root)remember_temporary(&account,root,path);subsonicpage_downloads_changed();
        }
        return true;
    }
	snprintf(waiting_path, sizeof(waiting_path), "%s", path);
    gui_post_coalesced(request_ready,NULL);
	device_state_stop();
	if (busy_play) { atomic_store(&play_cancelled, true); subsonic_download_cancel(); }
	return false;
}

static void prepare_waiting(void) {
	if (!waiting_path[0]) return;
	char current[512];
	if (!playlist_current_path(current, sizeof(current)) || strcmp(current, waiting_path)) { waiting_path[0] = 0; return; }
	library_remote_track_t remote;
	bool found = library_remote_get(waiting_path, &remote);
	waiting_path[0] = 0;
	if (!found) return;
	request_t *r = calloc(1, sizeof(*r));
	if (!r) { gui_notify_popup("Not enough memory to prepare this song."); return; }
	r->client = account;
	if (!r->client.password[0]) subsonicaccount_load(config_path(), &r->client);
	if (!r->client.password[0] || strcmp(r->client.base_url, remote.server) || strcmp(r->client.username, remote.username)) {
		gui_notify_popup("Connect to this song's Navidrome account before playing it.");
		subsonic_client_clear(&r->client); free(r); return;
	}
	const char *root = music_storage_root();
	if (!root || !gui_card_available() || strlen(root) >= sizeof(r->root)) {
		gui_notify_popup("Music storage is unavailable."); subsonic_client_clear(&r->client); free(r); return;
	}
	r->song = remote.song; r->play = true; r->resume_queue = true;r->temporary=temporary_playback();
	strcpy(r->root, root);
	r->queue_revision = playlist_revision(); r->queue_index = playlist_current_index();
	submit_request(r);
}

static void connect_cb(lv_event_t *e) { (void)e; if (!ignore_tap()) start_request(true); }

static void disconnect_cb(lv_event_t *e) {
	(void)e;
	if (ignore_tap()) return;
	subsonicoffline_cancel();
	connected = false;
	subsonic_client_clear(&account);
	clear_password();
	snprintf(message, sizeof(message), "Disconnected. Enter your password to reconnect.");
	rebuild_home();
}

static void sync_cb(lv_event_t *e) {
	(void)e;
	if (!ignore_tap()) start_request(false);
}

bool subsonicpage_sync_playlists(const char *name) {
	if(busy) {gui_notify_popup("Navidrome is busy. Try again when it finishes.");return false;}
	request_t *r=calloc(1,sizeof(*r));
	if(!r)return false;
	r->client=account;
	if(!r->client.password[0])subsonicaccount_load(config_path(),&r->client);
	if(!r->client.password[0]) {
		gui_notify_popup("Connect to Navidrome first.");subsonic_client_clear(&r->client);free(r);return false;
	}
	r->playlists=true;
	if(name)snprintf(r->upload,sizeof(r->upload),"%s",name);
	submit_request(r);switch_screen(subsonic_screen);return true;
}


static void forget_confirmed(void *unused) {
    (void)unused;
	if (ignore_tap()) return;
	if (!subsonicaccount_forget(config_path())) {
		snprintf(message, sizeof(message), "Could not remove the saved connection."); rebuild_home(); return;
	}
	saved_connection = false;
	subsonic_forget_server(account.base_url,account.username);
	disconnect_cb(NULL);
	server[0] = username[0] = 0;
	config_set("subsonic", "server", ""); config_set("subsonic", "username", ""); config_save();
	snprintf(message, sizeof(message), "Saved connection forgotten. Library entries and downloaded songs are kept.");
	rebuild_home();
}

static void forget_cb(lv_event_t *e) {
 (void)e;
 if(ignore_tap() || subsonicoffline_busy())return;
 confirm_show("Remove connection?",
  "The saved login will be removed. Library entries, playlists and downloaded music stay on this device. Downloaded music remains playable. Server access and synchronization stop until you connect again.",
  "Remove",forget_confirmed,NULL);
}


bool subsonicpage_selective(void){
 if(!account.password[0])return false;
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 return library_offline_options(source,&options) && options.mode==OFFLINE_SELECTIVE;
}
static bool active_remote(const char *path,library_remote_track_t *track){
 return library_remote_get(path,track) && !strcmp(track->server,account.base_url) && !strcmp(track->username,account.username);
}
static bool complete_download(const char *path,const char *id){
 struct stat st;subsonic_identity_t identity;
 return !subsoniccache_is_temporary(path) && stat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 &&
  subsoniccache_identity(path,&identity) && !strcmp(identity.server,account.base_url) && !strcmp(identity.username,account.username) && !strcmp(identity.song_id,id);
}
int subsonicpage_download_state(const char *const *paths,int count){
 if(!paths || count<1 || !subsonicpage_selective())return -1;
 bool any=false,complete=true;
 for(int i=0;i<count;++i){library_remote_track_t track;if(active_remote(paths[i],&track)){
  any=true;if(!complete_download(paths[i],track.song.id))complete=false;
 }}
 return any?(complete?1:0):-1;
}

bool subsonicpage_sync_busy(void){return busy_sync || busy_play || subsonicoffline_busy();}
bool subsonicpage_download_pending(const char *const *paths,int count,const char *playlist){
 subsonic_offline_status_t status;subsonicoffline_status(&status);
 if(!status.running || status.removing)return false;
 char source[33];subsoniccache_source(&account,source);if(strcmp(source,status.source))return false;
 library_playlist_link_t link;
 bool selected_playlist=playlist && *playlist && library_playlist_link(playlist,&link) && !strcmp(link.source,source) && library_offline_has_rule(source,OFFLINE_PLAYLIST,link.id);
 for(int i=0;i<count;++i){library_remote_track_t track;if(!active_remote(paths[i],&track))continue;
  if(!strcmp(status.path,paths[i]))return true;
  if(!complete_download(paths[i],track.song.id) && !library_offline_has_rule(source,OFFLINE_EXCLUDE,track.song.id) &&
   (selected_playlist || library_offline_wanted(paths[i]) || library_offline_has_rule(source,OFFLINE_SONG,track.song.id) || library_offline_has_rule(source,OFFLINE_ALBUM,track.song.album_id)))return true;
 }
 return false;
}
bool subsonicpage_download_paths(const char *const *paths,int count,int kind,const char *group,bool remove){
 if(busy || subsonicoffline_busy()){gui_notify_popup("Wait for the current Navidrome operation to finish.");return false;}
 if(!subsonicpage_selective() || !paths || count<1)return false;
 char (*ids)[128]=calloc((size_t)count,128);const char **eligible=calloc((size_t)count,sizeof(*eligible));
 if(!ids || !eligible){free(ids);free(eligible);return false;}
 int n=0;char source[33],value[256]="";subsoniccache_source(&account,source);
 bool same_album=true;
 for(int i=0;i<count;++i){library_remote_track_t track;if(active_remote(paths[i],&track)){
  strcpy(ids[n],track.song.id);eligible[n]=paths[i];
  if(n==0)snprintf(value,sizeof(value),"%s",track.song.album_id);
  else if(strcmp(value,track.song.album_id))same_album=false;
  ++n;
 }}
 if(kind==OFFLINE_PLAYLIST){
  library_playlist_link_t link;
  if(group && library_playlist_link(group,&link) && !strcmp(link.source,source))snprintf(value,sizeof(value),"%s",link.id);
  else kind=OFFLINE_SONG;
 }else if(kind!=OFFLINE_ALBUM || !same_album || !value[0])kind=OFFLINE_SONG;
 bool ok=n>0 && library_offline_select(source,kind,value,(const char (*)[128])ids,n,!remove);
 const char *root=music_storage_root();
 if(ok && remove){
  device_state_t state;device_state_get(&state);
  for(int i=0;i<n;++i)if(!strcmp(state.current_file,eligible[i])){device_state_stop();break;}
  ok=root && subsonicoffline_remove(&account,root,eligible,n,false);
 }else if(ok){
  if(subsonic_network_available() && subsonic_server_available(account.base_url,account.username))
   ok=root && subsonicoffline_start(&account,root,false);
  else gui_notify_popup("Selected for download. Sync when the server is available.");
 }
 free(ids);free(eligible);subsonicpage_downloads_changed();
 if(!ok)gui_notify_popup("Could not start this download action. Check storage and offline settings.");
 return ok;
}
static void remove_all_confirmed(void *unused){
 (void)unused;
 if(busy || subsonicoffline_busy()){gui_notify_popup("Wait for the current Navidrome operation to finish.");return;}
 char source[33];subsoniccache_source(&account,source);
 const char *root=music_storage_root();
 if(!account.password[0] || !root || !library_offline_clear(source)){gui_notify_popup("Cannot remove downloads while the library is busy.");return;}
 device_state_t state;device_state_get(&state);
 library_remote_track_t track;
 if(active_remote(state.current_file,&track))device_state_stop();
 if(!subsonicoffline_remove(&account,root,NULL,0,true))gui_notify_popup("Cannot start removing downloads.");
 subsonicpage_downloads_changed();
}
bool subsonicpage_remove_all_downloads(void){
 if(busy || subsonicoffline_busy()){gui_notify_popup("Wait for the current Navidrome operation to finish.");return false;}
 confirm_show("Remove all downloads?",
  "Remove all downloaded Navidrome music for this connection and reset to Selective mode with automatic caching off? Local music, library entries and playlists are kept. Playback of a downloaded server track will stop.",
  "Remove downloads",remove_all_confirmed,NULL);
 return true;
}

static void action_button(lv_obj_t *parent,const char *text,bool danger,lv_event_cb_t callback) {
 lv_obj_t *button=lv_button_create(parent);
 lv_obj_set_size(button,danger?LV_PCT(100):230,64);
 lv_obj_add_style(button,&theme_style_card,0);
 lv_obj_add_style(button,&theme_style_card_pressed,LV_STATE_PRESSED);
 lv_obj_set_style_bg_color(button,danger?lv_color_hex(0xc01c28):theme()->accent,0);
 lv_obj_set_style_radius(button,LV_RADIUS_CIRCLE,0);
 lv_obj_set_style_shadow_width(button,0,0);
 lv_obj_add_event_cb(button,callback,LV_EVENT_CLICKED,NULL);
 lv_obj_t *label=lv_label_create(button);lv_label_set_text(label,text);
 lv_obj_set_style_text_font(label,&font_ui_24,0);
 lv_obj_set_style_text_color(label,lv_color_white(),0);lv_obj_center(label);
}

static void edit_accept(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(field);
	if (editing == 0) snprintf(server, sizeof(server), "%s", text);
	else if (editing == 1) snprintf(username, sizeof(username), "%s", text);
	else if (editing == 2) snprintf(password, sizeof(password), "%s", text);
	lv_textarea_set_text(field, "");
	keyboard_set_visible(keyboard, false);
	switch_screen_return_to(subsonic_screen);
	rebuild_home();
}

static void editor_left(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(field, "");
	keyboard_set_visible(keyboard, false);
}

static void edit_cb(lv_event_t *e) {
	if (ignore_tap()) return;
	editing = (int)(intptr_t)lv_event_get_user_data(e);
    if(editing<0 || editing>2)return;
	const char *titles[] = {"Server address", "Username", "Password"};
	const char *values[] = {server, username, password};
	const int limits[] = {511, 127, 255};
	lv_label_set_text(edit_title, titles[editing]);
	lv_textarea_set_max_length(field, limits[editing]);
	lv_textarea_set_password_mode(field, editing == 2);
	keyboard_refresh_password(field);
	lv_textarea_set_text(field, values[editing]);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(editor);
}

static void cancel_cb(lv_event_t *e) {
 (void)e;
 if(switcher_back_drag_active())return;
 atomic_store(&play_cancelled,true);
 subsonic_download_cancel();subsonicoffline_cancel();
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 offline_retry_at=time(NULL)+(library_offline_options(source,&options)?options.interval_minutes*60:60);
 snprintf(message,sizeof(message),"Cancelling sync...");
 rebuild_home();
}

static const int refresh_minutes[]={15,30,60,120,360,720,1440};
static lv_obj_t *refresh_value;
static void automatic_cb(lv_event_t *e){
 if(switcher_back_drag_active())return;
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 if(!library_offline_options(source,&options))return;
 options.automatic=lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED);
 if(!library_offline_save_options(source,&options))gui_notify_popup("Cannot save sync settings while the library is busy.");
 rebuild_home();
}
static void refresh_cb(lv_event_t *e){
 lv_event_code_t code=lv_event_get_code(e);
 if(code!=LV_EVENT_VALUE_CHANGED && code!=LV_EVENT_RELEASED)return;
 int index=lv_slider_get_value(lv_event_get_target(e));
 char text[64];snprintf(text,sizeof(text),"%d minutes",refresh_minutes[index]);lv_label_set_text(refresh_value,text);
 if(code!=LV_EVENT_RELEASED || switcher_back_drag_active())return;
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 if(!library_offline_options(source,&options))return;
 options.interval_minutes=refresh_minutes[index];
 if(!library_offline_save_options(source,&options))gui_notify_popup("Cannot save sync settings while the library is busy.");
}
static void sync_controls(void){
 char source[33];offline_options_t options;subsoniccache_source(&account,source);
 if(!library_offline_options(source,&options))return;
 lv_obj_t *toggle;settingsrow_toggle(home,"Automatic sync",&toggle,automatic_cb);
 if(options.automatic)lv_obj_add_state(toggle,LV_STATE_CHECKED);
 if(options.automatic){
  lv_obj_t *slider;settingsrow_slider(home,"Refresh interval",7,&refresh_value,&slider,NULL);
  int index=0;for(int i=0;i<7;++i)if(abs(refresh_minutes[i]-options.interval_minutes)<abs(refresh_minutes[index]-options.interval_minutes))index=i;
  lv_slider_set_value(slider,index,LV_ANIM_OFF);
  char text[64];snprintf(text,sizeof(text),"%d minutes",refresh_minutes[index]);lv_label_set_text(refresh_value,text);
  lv_obj_add_event_cb(slider,refresh_cb,LV_EVENT_ALL,NULL);
 }
}
static void build_home(void) {

	lv_obj_clean(home);
	if(account.password[0]){
     sync_controls();
     settingsrow_add(home,"Offline / cache settings",NULL,offline_open_cb,NULL);
    }
 subsonic_offline_status_t offline_status;subsonicoffline_status(&offline_status);
 bool syncing=busy_sync || busy_play || (offline_status.running && !offline_status.removing);
 if(connected || syncing){
  action_button(home,syncing?"Cancel sync":"Sync",false,syncing?cancel_cb:sync_cb);
  if((busy && !syncing) || offline_status.removing)lv_obj_add_state(lv_obj_get_child(home,-1),LV_STATE_DISABLED);
	} else {
		lv_obj_t *value;
		settingsrow_add(home, "Server address", &value, edit_cb, (void *)(intptr_t)0);
		lv_label_set_text(value, server[0] ? server : "Not set");
		settingsrow_add(home, "Username", &value, edit_cb, (void *)(intptr_t)1);
		lv_label_set_text(value, username[0] ? username : "Not set");
		settingsrow_add(home, "Password", &value, edit_cb, (void *)(intptr_t)2);
		lv_label_set_text(value, (password[0] || (saved_connection && account.password[0])) ? "••••••••" : "");
		action_button(home,"Connect",false,connect_cb);
		settingsrow_heading(home, "Use your server root URL. Connecting saves the account on this device.");
	}

}

static void rebuild_home(void){
 if(remove_connection_icon){
  lv_obj_set_hidden(remove_connection_icon,!saved_connection);
  if(busy || subsonicoffline_busy())lv_obj_add_state(remove_connection_icon,LV_STATE_DISABLED);
  else lv_obj_remove_state(remove_connection_icon,LV_STATE_DISABLED);
 }
 int y=lv_obj_get_scroll_y(home);
 build_home();lv_obj_update_layout(home);
 lv_obj_scroll_to_y(home,y,LV_ANIM_OFF);
}

void subsonicpage_init(gui_config_t *cfg) {
 subsonic_set_availability_notify(queue_availability);
 subsonicoffline_set_notify(queue_offline);
 subsonicavailability_set_notify(queue_availability);
 library_set_change_notify(queue_availability);

#ifndef HOST_BUILD
 wifi_set_connection_callback(network_changed);
#endif
#ifdef HOST_BUILD
	if (cfg->sd_root_path && strlen(cfg->sd_root_path) < sizeof(host_storage_root))
		snprintf(host_storage_root, sizeof(host_storage_root), "%s", cfg->sd_root_path);
#endif
	subsonicofflinepage_init(cfg,&account);
	streamturn_register_abandon(subsonic_download_cancel);
	snprintf(server, sizeof(server), "%s", config_get("subsonic", "server", ""));
	snprintf(username, sizeof(username), "%s", config_get("subsonic", "username", ""));
	saved_connection = subsonicaccount_load(config_path(), &account);
	if (saved_connection) {
		strcpy(server, account.base_url); strcpy(username, account.username);
	}
	subsonic_screen = lv_obj_create(NULL);
	home = settingsrow_page(subsonic_screen, cfg, "Navidrome");
 settingsrow_title_corner_slots(settingsrow_page_title(subsonic_screen),cfg,1);
 remove_connection_icon=lv_button_create(subsonic_screen);
 lv_obj_set_size(remove_connection_icon,56,56);
 lv_obj_set_style_bg_opa(remove_connection_icon,LV_OPA_TRANSP,0);
 lv_obj_set_style_border_width(remove_connection_icon,0,0);
 lv_obj_set_style_shadow_width(remove_connection_icon,0,0);
 lv_obj_set_style_pad_all(remove_connection_icon,0,0);
 lv_obj_align(remove_connection_icon,LV_ALIGN_TOP_RIGHT,-cfg->padding,cfg->padding+cfg->top_bar_height);
 lv_obj_add_event_cb(remove_connection_icon,forget_cb,LV_EVENT_CLICKED,NULL);
 lv_obj_t *trash=lv_image_create(remove_connection_icon);lv_image_set_src(trash,&icon_trash);
 lv_obj_add_style(trash,&theme_style_icon,0);
 lv_obj_set_style_image_recolor_opa(trash,LV_OPA_COVER,0);lv_obj_center(trash);

	switcher_attach_back_gesture(subsonic_screen);
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
	lv_timer_create(maintenance_tick,1000,NULL);
	device_state_add_prepare_cb(prepare_track);
	theme_register_refresh(rebuild_home);
	rebuild_home();
	if (saved_connection) start_request(true);
}
